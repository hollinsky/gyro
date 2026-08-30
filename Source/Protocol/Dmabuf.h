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
// **Version 5, because 3 stopped being the older path and became no path.** Decision 154 chose 3 on
// the reading that the format and modifier events are what every toolkit still handles, and that
// advertising 4 without honouring it leaves a client blocked forever on a roundtrip. The second half
// stands. The first was wrong about *who asks*: a toolkit never asks which GPU it is on, and the layer
// underneath it — Mesa's Wayland platform — has deleted every way of finding out except this one.
// There is no `wl_drm` left in `libEGL_mesa.so`. So a compositor that lists layouts and names no
// device is one where every GL and Vulkan client on the machine falls back to llvmpipe, which is a
// person watching a browser burn a core to scroll a page. That is what this file now answers, and the
// revision is recorded at decision 154.
//
// **A default feedback is a constant, which is what makes this small.** One format table, one main
// device, one tranche, sent at bind and never re-sent. The negotiation decision 154 refused — a
// tranche that changes as a window moves between two cards, which is `get_surface_feedback` meaning
// what it says — is still refused, and Open.md carries it. Both requests are answered here and both
// answer the same thing, which the protocol permits: surface feedback is a hint a compositor may
// sharpen, not a fact it owes.
//
// **The version is 5 rather than 4 because 5's whole obligation was already discharged**, that being
// the `invalid_format` error for planes that do not share a modifier, which `Build` has posted since
// decision 154. It is not 6: at 6 `main_device` must stop being sent, every tranche must carry the
// sampling flag, and `set_sampling_device` becomes a request that means something — three promises
// rather than a number.
//
// **The pairs come from `Scene/Textures.h` rather than from a device**, per decision 87's rule as it
// survives here: an author relays the client's own fourcc and modifier and reasons about neither. What
// gyro will take is the intersection over every renderer on the machine, computed by the composition
// root, and it is empty on a machine with no GPU — where this global falls back to version 3, offers
// an empty list, and a client draws into shared memory and still gets a window.

// What this global is advertised at where gyro can name a device to allocate against.
inline constexpr std::uint32_t DmabufVersion = 5;

// The version from which feedback replaces the pair list. Below it a client is told the layouts as
// events; at it and above those events are forbidden and the same client asks instead.
inline constexpr std::uint32_t FeedbackFromVersion = 4;

// What it is advertised at where gyro cannot.
//
// **A machine with no GPU has no main device, and feedback without one is not a thing the protocol
// lets a compositor send** — `main_device` is required and there is exactly one. So the fallback is
// the version whose contract is a list rather than a device, carrying the empty list, which is what
// the whole of this file did before feedback was built. It is the honest answer rather than a feedback
// object that would send `done` having named nothing.
inline constexpr std::uint32_t DmabufVersionWithoutDevice = 3;

// The parameters every `zwp_linux_dmabuf_feedback_v1` on the machine is sent, built once.
//
// **One table and one device for every client**, because that is what a default feedback is: the
// format table is a sealed descriptor a client maps read-only, so handing the same one to everybody is
// the arrangement the protocol was shaped for rather than a shortcut. The indices are `0` to `n - 1`
// and kept rather than rebuilt per bind, since the one tranche is the whole table.
//
// **Nothing here can change after the first client has been told**, which the protocol states: a
// compositor that wants to say something different must build a *new* table and re-send. That is the
// shape decision 41 requires for device migration — a client can be told its allocation stopped being
// valid — and the party that would call for it is the migration this tree does not have yet. What
// exists is the mechanism; what is missing is the caller, and Open.md says so.
class DmabufFeedback
{
public:
	// Build the table from what an author advertises. Called once, before the global is.
	//
	// A failure is a descriptor the kernel would not give, and the caller's answer to it is to
	// advertise version 3 rather than to refuse to start: a compositor that came up with no
	// `zwp_linux_dmabuf_v1` at all is a worse machine than one whose clients use shared memory.
	[[nodiscard]] Result<void> Describe(const ITextures& textures);

	// Whether there is something to send. False leaves the global at `DmabufVersionWithoutDevice`.
	[[nodiscard]] bool IsValid() const noexcept { return m_Table.IsValid() && m_Device != 0; }

	// What the global should be advertised at, which is the whole of how the two versions are chosen.
	[[nodiscard]] std::uint32_t Version() const noexcept
	{
		return IsValid() ? DmabufVersion : DmabufVersionWithoutDevice;
	}

	// Send the lot: the table, the device, one tranche, and `done`.
	void SendTo(Wayland::Server::ZwpLinuxDmabufFeedbackV1 object) const;

	// The descriptor and its length, for a test that maps what a client would map.
	[[nodiscard]] RawFd Table() const noexcept { return m_Table.Borrow(); }

	[[nodiscard]] std::uint32_t TableBytes() const noexcept { return m_Size; }

	[[nodiscard]] std::uint64_t MainDevice() const noexcept { return m_Device; }

private:
	// SPEC: what one entry of the table is, which the protocol fixes: a fourcc, four bytes of padding,
	// and a modifier, in the machine's own byte order.
	static constexpr std::uint32_t EntryBytes = 16;

	Fd m_Table;
	std::uint32_t m_Size = 0;
	std::uint64_t m_Device = 0;

	std::vector<std::uint16_t> m_Indices;
};

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

// One `zwp_linux_dmabuf_feedback_v1`, default or per surface.
//
// **It holds nothing of its own and sends the same thing either way.** A feedback object exists so a
// client can be told again, and gyro has nothing new to say until there is a second device on the
// machine to say it about — so the parameters are the global's, borrowed, and this is the object they
// go out on. What a per-surface one *would* carry is a tranche for the card the window is currently
// on, which is the half decision 154 still defers.
class ClientDmabufFeedback final : public Wayland::Server::ZwpLinuxDmabufFeedbackV1Handler
{
public:
	explicit ClientDmabufFeedback(const DmabufFeedback& feedback) noexcept : m_Feedback{ &feedback } {}

	void OnGone() override { delete this; }

	// The whole parameter set, which is what this interface's contract is: a client asks once and
	// expects everything to arrive without asking again.
	void OnBound() override { m_Feedback->SendTo(Object()); }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

private:
	const DmabufFeedback* m_Feedback = nullptr;
};

// One client's `zwp_linux_dmabuf_v1`.
class ClientDmabuf final : public Wayland::Server::ZwpLinuxDmabufV1Handler
{
public:
	ClientDmabuf(HostContext& context, const DmabufFeedback& feedback) noexcept
		: m_Context{ &context }, m_Feedback{ &feedback }
	{}

	void OnGone() override { delete this; }

	// The pair list, which is what this interface's contract begins with below version 4: a client
	// binds and expects to be told what it may allocate before it has asked anything. Nothing at 4 and
	// above, where the same events are not merely superseded but forbidden, and where a client is
	// expected to ask for feedback instead.
	void OnBound() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. Buffers made
	// through it stay valid, which the protocol says out loud.
	void OnDestroy() override {}

	Wayland::Server::ZwpLinuxBufferParamsV1Handler* OnCreateParams() override
	{
		return new ClientDmabufParams{ *m_Context };
	}

	// **Both answer, and both answer the same parameters.** A client that reached either of these bound
	// at 4 or above, which this global only offers where there is a device to name — so the object is
	// always one that has something to send, and the *refusal* these used to be is now unreachable
	// rather than merely unlikely.
	Wayland::Server::ZwpLinuxDmabufFeedbackV1Handler* OnGetDefaultFeedback() override
	{
		return new ClientDmabufFeedback{ *m_Feedback };
	}

	// The surface is ignored, which is legal and is the whole of what is deferred here: surface
	// feedback lets a compositor say *this window is on that card, allocate for it*, and gyro composites
	// every window on one device. A client that asks gets the default answer, which is true — rather
	// than a refusal, which would end a toolkit that asked a reasonable question.
	Wayland::Server::ZwpLinuxDmabufFeedbackV1Handler* OnGetSurfaceFeedback(Wayland::Server::WlSurface) override
	{
		return new ClientDmabufFeedback{ *m_Feedback };
	}

private:
	HostContext* m_Context = nullptr;

	// The global's, borrowed, and it outlives every client that binds.
	const DmabufFeedback* m_Feedback = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class DmabufGlobal final : public Wayland::Server::ZwpLinuxDmabufV1Binding
{
public:
	explicit DmabufGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	// Build the feedback parameters. Called once, before `Advertise`, because the version this global
	// goes up at is an answer about what was built here.
	[[nodiscard]] Result<void> Describe(const ITextures& textures) { return m_Feedback.Describe(textures); }

	// What to advertise at, per `DmabufFeedback::Version`.
	[[nodiscard]] std::uint32_t Version() const noexcept { return m_Feedback.Version(); }

	[[nodiscard]] const DmabufFeedback& Feedback() const noexcept { return m_Feedback; }

	Wayland::Server::ZwpLinuxDmabufV1Handler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;

	DmabufFeedback m_Feedback;
};
