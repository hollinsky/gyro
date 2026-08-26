#include "Protocol/Dmabuf.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <ranges>
#include <utility>
#include <vector>

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

Result<TextureId> ClientDmabufBuffer::Adopt(ITextures& textures)
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
	if (m_Outstanding == 0 && Object().IsValid())
	{
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
	return new ClientDmabuf{ *m_Context };
}
