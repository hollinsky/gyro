#include "Protocol/Dmabuf.h"

#include <spdlog/spdlog.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <ranges>
#include <utility>
#include <vector>

#include "Protocol/Sealed.h"
#include "Protocol/Trace.h"

// What `main_device` and `tranche_target_device` carry is a `dev_t`, and a client memcpys one out of
// the array by its own `sizeof`. Eight bytes on every Linux this compiles for, and an assertion rather
// than a comment because a mismatch would be a device number silently truncated — a client opening the
// wrong node, or none, with nothing anywhere saying why.
static_assert(sizeof(::dev_t) == sizeof(std::uint64_t));

using namespace Wayland::Server;

namespace
{
// Whether `flags` names something gyro can put on screen.
//
// **Both refused flags are refused because there is nowhere to carry them.** A y-inverted buffer
// would need the sampling transform to gain a flip that `TransformClass` does not have a bit for,
// and an interlaced one needs a deinterlacer. Refusing is what the protocol asks a compositor that
// cannot guarantee the picture to do, and it comes back as `failed` for a `create` — which is a
// client's own fallback path — rather than as a dead connection.
[[nodiscard]] bool Expressible(ZwpLinuxBufferParamsV1Flags flags) noexcept
{
	return !Any(flags & (ZwpLinuxBufferParamsV1Flags::YInvert | ZwpLinuxBufferParamsV1Flags::Interlaced));
}

// The length of what a descriptor names, or zero where it cannot be asked.
//
// **Asked so that `out_of_bounds` is a protocol error rather than a fault.** A client naming an
// offset and a stride that run off the end of its own buffer produces an import that either fails
// obscurely in the driver or reads memory the client did not allocate, and neither reaches a person
// as anything they could report. A descriptor whose size cannot be determined is let through: some
// dma-buf exporters are not seekable, and refusing every buffer from one of them would be refusing
// the whole client for a check that is a courtesy.
[[nodiscard]] std::size_t LengthOf(RawFd descriptor) noexcept
{
	const off_t end = lseek(descriptor.Value, 0, SEEK_END);

	return end > 0 ? static_cast<std::size_t>(end) : 0;
}
} // namespace

Result<void> DmabufFeedback::Describe(const ITextures& textures)
{
	const std::span<const TextureFormat> formats = textures.Formats();
	const std::uint64_t device = textures.MainDevice();

	if (formats.empty() || device == 0)
	{
		// Not a failure. A machine with no GPU has no device to name and nothing to offer, and the
		// caller reads `IsValid` and advertises version 3 with an empty list — which is a client drawing
		// into shared memory and getting a window, exactly as it did before this existed.
		return {};
	}

	if (formats.size() > std::numeric_limits<std::uint16_t>::max())
	{
		// A tranche's indices are sixteen bits each, so a list this long could not be pointed at. It is
		// unreachable on any device anybody has — the pairs are two fourccs times a driver's modifier
		// list — and it is checked because the alternative is a truncation that advertises the wrong
		// layouts rather than none.
		return Failure(E2BIG, "more format-modifier pairs than a tranche can index");
	}

	// The table as the protocol fixes it: a fourcc, four bytes that are *not* the modifier's high half
	// and must be written rather than left as whatever the stack held, and the modifier. Native byte
	// order, because the client maps the same memory rather than reading it off a wire.
	std::vector<std::byte> table(formats.size() * EntryBytes, std::byte{});

	m_Indices.clear();
	m_Indices.reserve(formats.size());

	for (std::size_t index = 0; index < formats.size(); ++index)
	{
		std::byte* const entry = table.data() + index * EntryBytes;
		const std::uint32_t code = formats[index].Code;
		const std::uint64_t modifier = formats[index].Modifier;

		std::memcpy(entry, &code, sizeof(code));
		std::memcpy(entry + 8, &modifier, sizeof(modifier));

		m_Indices.push_back(static_cast<std::uint16_t>(index));
	}

	Result<Fd> sealed = SealedMemfd("gyro-dmabuf-formats", table);

	if (!sealed)
	{
		m_Indices.clear();

		return std::unexpected{ sealed.error() };
	}

	m_Table = std::move(*sealed);
	m_Size = static_cast<std::uint32_t>(table.size());
	m_Device = device;

	return {};
}

void DmabufFeedback::SendTo(ZwpLinuxDmabufFeedbackV1 object) const
{
	if (!object.IsValid() || !IsValid())
	{
		return;
	}

	// **The order is the protocol's own**, and the reason it is worth stating is that `done` is what
	// makes the set atomic: a client that has read `format_table` and no `done` is a client still
	// waiting, which is the failure decision 154 refused version 4 to avoid. Everything before it is
	// sent unconditionally, so there is no path through here that reaches the end without one.
	object.FormatTable(m_Table.Borrow(), m_Size);

	const std::span<const std::byte> device = std::as_bytes(std::span{ &m_Device, 1 });

	object.MainDevice(device);

	// **One tranche, targeting the device that composites, with no flags.** The flag that could go here
	// is `scanout`, which says *allocate this way and a plane may take it directly* — and gyro cannot
	// honestly say it yet: what a display engine will scan out is the plane's own `IN_FORMATS` table
	// intersected with this list, which Open.md carries as unbuilt. A tranche claiming scanout that
	// gyro then composites is a client paying for an allocation constraint it gets nothing for.
	object.TrancheTargetDevice(device);
	object.TrancheFlags(ZwpLinuxDmabufFeedbackV1TrancheFlags{});
	object.TrancheFormats(std::as_bytes(std::span{ m_Indices }));
	object.TrancheDone();

	object.Done();
}

ClientDmabufBuffer::~ClientDmabufBuffer()
{
	// **The ids stay live and only the notification is dropped.** A client destroying its `wl_buffer`
	// says nothing about whether a frame on screen is still drawn from it — decision 131's rule is that
	// only the watermark can say that — so the texture space keeps its own duplicated descriptors and
	// retires them in its own time. What has to stop is this pointer being called back, and there is no
	// release owed to a client that is no longer there to hear one.
	if (m_Context != nullptr)
	{
		if (ITextures* const textures = m_Context->Textures(); textures != nullptr)
		{
			textures->Abandon(*this);
		}
	}
}

Result<TextureId> ClientDmabufBuffer::Adopt(ITextures& textures, SyncTimelinePoint release)
{
	if (IsInert())
	{
		return Failure(EINVAL, "a dmabuf buffer that was never built");
	}

	std::array<TexturePlane, MaxTexturePlanes> planes{};

	for (std::size_t index = 0; index < m_Planes.size(); ++index)
	{
		planes[index] = TexturePlane{ .Descriptor = m_Planes[index].Descriptor.Borrow(),
			                          .Offset = m_Planes[index].Offset,
			                          .Stride = m_Planes[index].Stride };
	}

	const Result<TextureId> adopted = textures.Adopt(m_Size, m_Format, { planes.data(), m_Planes.size() }, this);

	if (adopted)
	{
		++m_Outstanding;

		if (release.IsSet())
		{
			m_Releases.push_back(std::move(release));
		}
	}

	return adopted;
}

void ClientDmabufBuffer::OnTextureReleased() noexcept
{
	if (m_Outstanding == 0)
	{
		return;
	}

	--m_Outstanding;

	// **Only at zero.** A buffer committed twice before the first frame left the screen has two ids
	// against it, and telling the client it may redraw after the first retires is the tearing this whole
	// file is arranged to prevent.
	if (m_Outstanding != 0)
	{
		return;
	}

	// **The release points go out here and the event goes out beside them**, rather than instead of.
	// The protocol says the delivery of `wl_buffer.release` becomes *undefined* for a surface with a
	// synchronization object, which is permission to stop sending it rather than a requirement to —
	// and a buffer's outstanding count is per buffer while a synchronization object is per surface, so
	// the same `wl_buffer` can legitimately have been committed to one of each. Sending both is the
	// only answer that is right for both, and a client that is not listening for the event is a client
	// that ignores it.
	for (const SyncTimelinePoint& point : m_Releases)
	{
		if (const Result<void> signalled = point.Timeline->Signal(point.Point); !signalled)
		{
			// The client waits forever for a buffer it will never get back, which is a hung window and
			// not a hung compositor. There is nothing to answer it with: the protocol has no error for
			// *gyro could not signal*, and ending the connection over gyro's own ioctl failing would
			// take the window down rather than let it recover.
			spdlog::warn("could not signal a client's buffer release point: {}", signalled.error());
		}
	}

	m_Releases.clear();

	if (Object().IsValid())
	{
		// **The borrowed half of `ClientSurface::ReleaseStaged`'s mark, and the informative one.** A
		// descriptor is never copied, so the client cannot touch these pixels again until this event —
		// which means the gap from its commit to here is the number of frames gyro kept it out of its own
		// buffer, and a client that never gets a second one back is a client stalled on gyro.
		TraceMark(
			"buffer released",
			m_Context->TraceRow(Object().WireClient()),
			TraceTag(wl_resource_get_id(Object().WireResource()))
		);

		Object().Release();
	}
}

void ClientDmabufParams::OnAdd(
	Fd fd,
	std::uint32_t planeIdx,
	std::uint32_t offset,
	std::uint32_t stride,
	std::uint32_t modifierHi,
	std::uint32_t modifierLo
)
{
	if (m_Used)
	{
		Object().PostError(ZwpLinuxBufferParamsV1Error::AlreadyUsed, "add after create");

		return;
	}

	if (planeIdx >= MaxTexturePlanes)
	{
		Object().PostError(ZwpLinuxBufferParamsV1Error::PlaneIdx, "plane index out of bounds");

		return;
	}

	if (m_Slots[planeIdx].Set)
	{
		Object().PostError(ZwpLinuxBufferParamsV1Error::PlaneSet, "plane index already set");

		return;
	}

	m_Slots[planeIdx] = Slot{ .Descriptor = std::move(fd),
		                      .Offset = offset,
		                      .Stride = stride,
		                      .Modifier = (static_cast<std::uint64_t>(modifierHi) << 32U) | modifierLo,
		                      .Set = true };
}

bool ClientDmabufParams::Advertises(TextureFormat format) const noexcept
{
	const ITextures* const textures = m_Context != nullptr ? m_Context->Textures() : nullptr;

	if (textures == nullptr)
	{
		return false;
	}

	return std::ranges::contains(textures->Formats(), format);
}

ClientDmabufBuffer* ClientDmabufParams::Build(
	std::int32_t width,
	std::int32_t height,
	std::uint32_t format,
	ZwpLinuxBufferParamsV1Flags flags,
	bool immediate
)
{
	if (m_Used)
	{
		Object().PostError(ZwpLinuxBufferParamsV1Error::AlreadyUsed, "create sent twice");

		return nullptr;
	}

	m_Used = true;

	if (width <= 0 || height <= 0)
	{
		Object().PostError(ZwpLinuxBufferParamsV1Error::InvalidDimensions, "a buffer with no extent");

		return nullptr;
	}

	// **Consecutive from zero, which the protocol states and which is the whole of what `incomplete`
	// means here.** A set with plane 0 and plane 2 filled is not a two-plane image with a gap; it is a
	// client that lost track of what it sent.
	std::size_t count = 0;

	while (count < MaxTexturePlanes && m_Slots[count].Set)
	{
		++count;
	}

	const bool contiguous =
		std::ranges::none_of(m_Slots | std::views::drop(count), [](const Slot& slot) noexcept { return slot.Set; });

	if (count == 0 || !contiguous)
	{
		Object().PostError(ZwpLinuxBufferParamsV1Error::Incomplete, "missing or non-consecutive planes");

		return nullptr;
	}

	// **One modifier for the whole image**, which version 5 makes a protocol error and which is a
	// physical fact below it: a modifier describes the layout of the allocation, and planes of one image
	// laid out differently is not something `AddFB2` or Vulkan can be told about.
	const std::uint64_t modifier = m_Slots[0].Modifier;

	for (std::size_t index = 1; index < count; ++index)
	{
		if (m_Slots[index].Modifier != modifier)
		{
			Object().PostError(ZwpLinuxBufferParamsV1Error::InvalidFormat, "planes with differing modifiers");

			return nullptr;
		}
	}

	for (std::size_t index = 0; index < count; ++index)
	{
		const std::size_t length = LengthOf(m_Slots[index].Descriptor.Borrow());
		const std::size_t needed = static_cast<std::size_t>(m_Slots[index].Offset) +
		                           static_cast<std::size_t>(m_Slots[index].Stride) * static_cast<std::size_t>(height);

		if (length != 0 && needed > length)
		{
			Object().PostError(ZwpLinuxBufferParamsV1Error::OutOfBounds, "a plane that runs off its dmabuf");

			return nullptr;
		}
	}

	const TextureFormat described{ .Code = format, .Modifier = modifier };

	// **A refusal from here on is `failed` rather than fatal, and `create_immed` has nowhere to put
	// one.** The protocol's own answer for the immediate form is `invalid_wl_buffer` where the cause is
	// the compositor's rather than the arguments', which is exactly this: the client asked for a pair
	// gyro does not take, having been told the list, or gyro has no device to take it onto at all.
	if (!Advertises(described) || !Expressible(flags))
	{
		if (immediate)
		{
			Object().PostError(ZwpLinuxBufferParamsV1Error::InvalidWlBuffer, "a format this compositor cannot import");
		}
		else
		{
			Object().Failed();
		}

		return nullptr;
	}

	std::vector<ClientDmabufBuffer::Plane> planes;

	planes.reserve(count);

	for (std::size_t index = 0; index < count; ++index)
	{
		planes.push_back(
			ClientDmabufBuffer::Plane{ .Descriptor = std::move(m_Slots[index].Descriptor),
		                               .Offset = m_Slots[index].Offset,
		                               .Stride = m_Slots[index].Stride }
		);
	}

	const PixelSize<BufferSpace> size{ width, height };

	return new ClientDmabufBuffer{ *m_Context, size, described, std::move(planes) };
}

void ClientDmabufParams::OnCreate(
	std::int32_t width,
	std::int32_t height,
	std::uint32_t format,
	ZwpLinuxBufferParamsV1Flags flags
)
{
	ClientDmabufBuffer* const buffer = Build(width, height, format, flags, false);

	if (buffer == nullptr)
	{
		return;
	}

	wl_client* const client = Object().WireClient();

	// The id is zero, which is libwayland's *you pick one* — a `create` makes a buffer the client has
	// not named, unlike every other factory in this file.
	const WlBuffer created = client != nullptr ? WlBuffer::Create(*client, Object().Version(), 0, *buffer) : WlBuffer{};

	if (!created.IsValid())
	{
		delete buffer;

		Object().Failed();

		return;
	}

	Object().Created(created);
}

WlBufferHandler* ClientDmabufParams::OnCreateImmed(
	std::int32_t width,
	std::int32_t height,
	std::uint32_t format,
	ZwpLinuxBufferParamsV1Flags flags
)
{
	ClientDmabufBuffer* const buffer = Build(width, height, format, flags, true);

	// **An inert buffer rather than null where the parameters were refused.** Returning null is how this
	// binding says *no memory*, which ends the client with a reason that is not the reason — and the
	// error the client should see has already been posted by `Build`. The object it hands back exists
	// only so the id the client already chose has something behind it while the connection unwinds.
	return buffer != nullptr ? buffer :
	                           new ClientDmabufBuffer{ *m_Context, {}, {}, std::vector<ClientDmabufBuffer::Plane>{} };
}

void ClientDmabuf::OnBound()
{
	const ITextures* const textures = m_Context != nullptr ? m_Context->Textures() : nullptr;

	if (textures == nullptr)
	{
		return;
	}

	const std::uint32_t version = Object().Version();

	// **Nothing at all from version 4 up.** The protocol does not merely deprecate these two events
	// there, it forbids them — a client bound at 4 has asked to be told through feedback instead, and
	// the pair list arriving anyway is a compositor speaking a contract it did not agree to. What that
	// client gets is `get_default_feedback`, which is answered on this same object.
	if (version >= FeedbackFromVersion)
	{
		return;
	}

	// **`format` for every distinct fourcc and `modifier` for every pair**, which is what version 3
	// asks for and is not redundant: a client bound at 1 or 2 has only the first list and reads it as
	// *this format under whatever layout you both work out*, which in practice means linear.
	std::vector<std::uint32_t> announced;

	for (const TextureFormat& format : textures->Formats())
	{
		if (!std::ranges::contains(announced, format.Code))
		{
			announced.push_back(format.Code);

			Object().Format(format.Code);
		}

		if (version >= 3)
		{
			Object().Modifier(
				format.Code,
				static_cast<std::uint32_t>(format.Modifier >> 32U),
				static_cast<std::uint32_t>(format.Modifier & 0xffffffffULL)
			);
		}
	}
}

ZwpLinuxDmabufV1Handler* DmabufGlobal::OnBind(wl_client&, std::uint32_t)
{
	return new ClientDmabuf{ *m_Context, m_Feedback };
}
