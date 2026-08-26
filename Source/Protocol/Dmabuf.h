#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Protocol/Buffer.h"
#include "Protocol/Context.h"
#include "Scene/Textures.h"
#include "Wayland/Server/LinuxDmabufV1.h"
#include "Wayland/Server/Wayland.h"

// `zwp_linux_dmabuf_v1`: a client handing gyro descriptors instead of pixels, which is what makes a
// window something a panel can scan out.
//
// **This is the protocol half of decision 152.** The partition promotes an item onto a plane only if
// there is a framebuffer for its texture, and a framebuffer is a thing the display engine made out of
// a descriptor — so a compositor that speaks only `wl_shm` composites every window on the machine no
// matter how much of the mechanism above it exists. Every buffer gyro has seen until now arrived as a
// mapping, was copied into gyro's own memory at commit, and was refused by the scanout importer by
// design. This is the file that makes a client able to hand over something else.
//
// **Nothing is copied, and that is the whole difference from `wl_shm`.** A pool is mapped and read;
// descriptors are duplicated and *held*, and the memory stays the client's for as long as an id names
// it. What follows from that is the one thing this file has that `Shm.h` does not: `wl_buffer.release`
// is owed rather than immediate, and the number that says when is the watermark. A client told it may
// redraw while a panel is still scanning the buffer out is a window tearing into itself, on the frame
// after it started moving, and it reaches a log as nothing at all — so `ClientDmabufBuffer` is an
// `ITextureRelease` and answers when the texture space says nobody is reading.
//
// **Version 3 and not 4, because 4 is a promise about a device gyro cannot yet name.** From version 4
// the format and modifier events are deprecated and a compositor is expected to answer
// `get_default_feedback` with a main device and format tranches — which is a *slow-loop* negotiation
// about what a client should allocate, and per Seam/Scanout.h it is deliberately a different question
// from what the per-frame partition decides. Advertising 4 without honouring it is a client waiting
// forever on a roundtrip; advertising 3 is a client being told the pairs gyro will take, which every
// toolkit still handles because every compositor shipped it for years. The number goes up in the
// commit that builds feedback.
//
// **The pairs come from `Scene/Textures.h` rather than from a device**, per decision 87's rule as it
// survives here: an author relays the client's own fourcc and modifier and reasons about neither. What
// gyro will take is the intersection over every renderer on the machine, computed by the composition
// root, and it is empty on a machine with no GPU — where this global advertises nothing and a client
// falls back to `wl_shm` and still gets a window.

// The `zwp_linux_dmabuf_v1` global's advertised version. See above: 3 is the last version whose
// contract is a list of format-modifier pairs rather than a device to allocate against.
inline constexpr std::uint32_t DmabufVersion = 3;

// One `wl_buffer` made out of descriptors.
//
// **It outlives its own `Adopt` calls and counts them**, which is what a release has to be keyed on
// rather than the last one: the same buffer may be attached to two surfaces, or re-committed to one
// before the first frame that drew it has left the screen, and every one of those is an id of its own
// that the frame thread may still be recording from. So the release goes out when the count reaches
// zero and not when any single id retires.
class ClientDmabufBuffer final : public ClientBuffer, public ITextureRelease
{
public:
	// One plane, owning its descriptor. The `Fd`s are the ones `add` collected; the texture space
	// duplicates them again for itself, because an id has to keep the memory reachable after the client
	// destroys the `wl_buffer`.
	struct Plane
	{
		Fd Descriptor;
		std::uint32_t Offset = 0;
		std::uint32_t Stride = 0;
	};

	ClientDmabufBuffer(
		HostContext& context,
		PixelSize<BufferSpace> size,
		TextureFormat format,
		std::vector<Plane> planes
	) noexcept
		: m_Context{ &context }, m_Size{ size }, m_Format{ format }, m_Planes{ std::move(planes) }
	{}

	~ClientDmabufBuffer() override;

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	[[nodiscard]] Result<TextureId> Adopt(ITextures& textures) override;

	[[nodiscard]] PixelSize<BufferSpace> Extent() const noexcept override { return m_Size; }

	// False, and this is the field the whole file turns on. See the header comment.
	[[nodiscard]] bool ReleasesImmediately() const noexcept override { return false; }

	void OnTextureReleased() noexcept override;

	// How many ids still name this buffer's memory. A test's way of asserting that a release was owed
	// rather than that nothing crashed.
	[[nodiscard]] std::size_t OutstandingCount() const noexcept { return m_Outstanding; }

	// True where `create` or `create_immed` could not make anything of the parameters. Such a buffer
	// exists only so the client has an object to destroy, and adopting it fails rather than aborting.
	[[nodiscard]] bool IsInert() const noexcept { return m_Planes.empty(); }

private:
	HostContext* m_Context = nullptr;

	PixelSize<BufferSpace> m_Size{};
	TextureFormat m_Format{};
	std::vector<Plane> m_Planes;

	std::size_t m_Outstanding = 0;
};

// One `zwp_linux_buffer_params_v1`: descriptors accumulating until `create` turns them into a buffer.
//
// **Every rule the protocol states about this object is a fatal error rather than a `failed` event**,
// with one exception, and the split is the protocol's own: a client that set the same plane twice or
// named a negative width is broken, and a compositor that quietly went along with it would be one that
// draws a window sheared across the screen with nothing in any log. What comes back as `failed`
// instead is an import gyro attempted and could not complete, which a client is entitled to recover
// from by allocating differently — and that is exactly the case a client's fallback path exists for.
class ClientDmabufParams final : public Wayland::Server::ZwpLinuxBufferParamsV1Handler
{
public:
	explicit ClientDmabufParams(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	void OnAdd(
		Fd fd,
		std::uint32_t planeIdx,
		std::uint32_t offset,
		std::uint32_t stride,
		std::uint32_t modifierHi,
		std::uint32_t modifierLo
	) override;

	void OnCreate(
		std::int32_t width,
		std::int32_t height,
		std::uint32_t format,
		Wayland::Server::ZwpLinuxBufferParamsV1Flags flags
	) override;

	Wayland::Server::WlBufferHandler* OnCreateImmed(
		std::int32_t width,
		std::int32_t height,
		std::uint32_t format,
		Wayland::Server::ZwpLinuxBufferParamsV1Flags flags
	) override;

	// Version 6, which this global does not advertise, so a conforming client cannot send it and
	// libwayland refuses one that tries before it reaches here. Answered by doing nothing rather than
	// left out, because the binding is generated from the whole XML.
	void OnSetSamplingDevice(std::span<const std::byte>) override {}

private:
	// One slot of the accumulating set.
	struct Slot
	{
		Fd Descriptor;
		std::uint32_t Offset = 0;
		std::uint32_t Stride = 0;
		std::uint64_t Modifier = 0;
		bool Set = false;
	};

	// The whole of `create` and `create_immed`, since the two differ only in how they answer. Null where
	// the parameters were refused, which is either a protocol error already posted or a `failed` event
	// already sent — the caller can tell which from `fatal`.
	[[nodiscard]] ClientDmabufBuffer* Build(
		std::int32_t width,
		std::int32_t height,
		std::uint32_t format,
		Wayland::Server::ZwpLinuxBufferParamsV1Flags flags,
		bool immediate
	);

	// Whether gyro will take this pair, asked of the texture space rather than of a device.
	[[nodiscard]] bool Advertises(TextureFormat format) const noexcept;

	HostContext* m_Context = nullptr;

	std::array<Slot, MaxTexturePlanes> m_Slots;

	// `create` may be sent once, and the object is only good for `destroy` afterwards.
	bool m_Used = false;
};

// One client's `zwp_linux_dmabuf_v1`.
class ClientDmabuf final : public Wayland::Server::ZwpLinuxDmabufV1Handler
{
public:
	explicit ClientDmabuf(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// The pair list, which is what this interface's contract begins with below version 4: a client
	// binds and expects to be told what it may allocate before it has asked anything.
	void OnBound() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. Buffers made
	// through it stay valid, which the protocol says out loud.
	void OnDestroy() override {}

	Wayland::Server::ZwpLinuxBufferParamsV1Handler* OnCreateParams() override
	{
		return new ClientDmabufParams{ *m_Context };
	}

	// Version 4, which this global does not advertise. Refused by returning null — which ends the
	// client — rather than by handing back a feedback object that would never send `done` and leave a
	// toolkit blocked on a roundtrip forever.
	Wayland::Server::ZwpLinuxDmabufFeedbackV1Handler* OnGetDefaultFeedback() override { return nullptr; }

	Wayland::Server::ZwpLinuxDmabufFeedbackV1Handler* OnGetSurfaceFeedback(Wayland::Server::WlSurface) override
	{
		return nullptr;
	}

private:
	HostContext* m_Context = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class DmabufGlobal final : public Wayland::Server::ZwpLinuxDmabufV1Binding
{
public:
	explicit DmabufGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::ZwpLinuxDmabufV1Handler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
