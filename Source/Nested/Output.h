#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "Core/Result.h"
#include "Core/Time.h"
#include "Nested/Host.h"
#include "Nested/Sync.h"
#include "Seam/Allocator.h"
#include "Seam/Buffer.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Wayland/LinuxDmabufV1.h"
#include "Wayland/LinuxDrmSyncobjV1.h"
#include "Wayland/PresentationTime.h"
#include "Wayland/Wayland.h"
#include "Wayland/XdgDecorationUnstableV1.h"
#include "Wayland/XdgShell.h"

// One window on the host, as an `IPresenter`.
//
// **A window resize is a mode change, and building it that way now is the point.**
// Docs/Architecture.md#nested-wayland names it as one of two things to build deliberately rather than
// let emerge: `xdg_toplevel.configure` arrives with a new size, the target set is released, and the
// transition completes — which is decision 73's split exactly, with the host standing in for the
// hardware. What that buys is that real hotplug and real mode setting are the same path when the DRM
// backend arrives, exercised every time somebody drags the corner of the window they are developing
// in, rather than a rewrite nobody has run.
//
// **The targets are exported from the rendering device**, which is decision 120 and which is why this
// takes an `IDmabufAllocator` rather than owning one. Unlike KMS there is no GBM device handed over
// and unlike WSI nothing allocates on gyro's behalf; the only thing on the machine that can make one
// of these buffers is the device that is about to draw into it, and the composition root is the only
// thing that knows both. What the *host* contributes is the constraint: `zwp_linux_dmabuf_v1`'s
// feedback ranks format-and-modifier pairs against its own hardware, and this walks that ranking
// offering candidates until the device accepts one. Neither end guesses.
//
// **The clock is the host's vblank, read from `wp_presentation_feedback`.** That is the whole reason
// nested is worth building: Frame/FrameClock.h predicts against a real cadence with real jitter, so
// the deadline scheduler runs unmodified against a panel gyro does not own. `discarded` is a *miss*
// and reaches the loop through Seam/Presenter.h's `Missed` — silence there would leave the output
// flip-pending for good, which on a multi-window session is one window freezing while the others
// carry on and nothing anywhere reporting a failure.
//
// **Explicit sync where it can be had, a held commit where it cannot.** `Present` may never block, so
// a composite that has not landed is never waited for inline. Where the host speaks
// `wp_linux_drm_syncobj_v1` and there is a DRM node to mint a release timeline on, the commit goes out
// immediately naming a point the GPU has not reached and the host does the waiting — which is the
// path worth having, because it is what lets the host plan its own frame around gyro's. Where either
// is missing the commit is *held* until `IRenderer::IsComplete` says the pixels exist, and released
// on a later drain. That costs the overlap between drawing a frame and handing it over: the window
// runs a frame behind, and every timestamp the clock then reads carries gyro's own polling in it. The
// log says which path is live, and says that pacing figures from the second one are not to be
// trusted.
//
// **One layer, and more than one is refused.** There is a single `wl_surface` here and no plane
// catalog behind it, so a layer set with a second layer in it is `EINVAL` — Seam/Presenter.h's *the
// layers cannot be expressed*, which is a bug in the assigner rather than a condition to retry.
// Refusing rather than compositing the rest is what keeps such a bug from being invisible on this
// backend and discovered on the one with planes. Render/Renderer.cpp refuses what it cannot draw for
// the same reason.

namespace Nested
{
// What a host compositor's own pipeline wants. Three rather than two for `DefaultVirtualTargets`'
// reason: the host holds one while gyro draws into another, and two makes stalling the only
// behaviour every time the host is a frame behind. A test that wants the stall says two.
inline constexpr std::uint32_t DefaultNestedTargets = 3;

// How many commits this backend will have outstanding once the host has asked for a frame. See
// `NestedOutput::CommitDepth`.
inline constexpr std::uint32_t NestedCommitDepth = 2;

struct NestedOutputPolicy
{
	// How many images the ring holds. Clamped into `[2, MaxTargets]`; one is not expressible
	// here, because a host holds the committed buffer until it has finished with it and a ring of one
	// would have nothing to draw into until it let go.
	std::uint32_t Targets = DefaultNestedTargets;

	// What the window is called, and what a desktop groups it under.
	std::string Title = "gyro";
	std::string AppId = "dev.gyro.compositor";

	// **Ask the host to draw the frame, and it is deliberate that this is not the other way round.**
	// `xdg-decoration` is how a client says it draws its own, and gyro will — its dressing is a
	// material and a radius Seam/Dressing.h already fixes. It does not draw one *on this surface*
	// today, and a window with no titlebar and no client-side chrome is one nobody can move, resize by
	// edge, or close, on the backend whose entire job is being developed in. So the host is asked for
	// a frame until gyro's own is on the glass, and this flips when it is.
	bool Decorate = true;
};

class NestedOutput final : public IPresenter
{
public:
	// The host and the allocator are references because neither is copyable or movable and both
	// outlive every output — one connection and one device serve every window on a machine, exactly as
	// `VirtualOutput` takes its provider.
	//
	// `completion` is the renderer whose submissions land in these targets, or null where every point
	// is immediate. It is only ever asked `IsComplete`, and only on the held-commit path; a host with
	// explicit sync never consults it. Null there means a commit is never held, which is right for a
	// CPU blitter and wrong for anything with a queue.
	NestedOutput(
		NestedHost& host,
		IDmabufAllocator& allocator,
		const IRenderer* completion,
		const OutputConfiguration& configuration,
		NestedOutputPolicy policy = {}
	);

	~NestedOutput() override;

	// Create the surface, take the role, adopt the first configure, and build the target set.
	//
	// **It roundtrips, and it is the last thing in this module that may.** The initial `configure` is
	// a question with an answer that arrives later and there is no frame loop yet to arrive into; from
	// here on every host event is drained. It runs on whichever thread the composition root builds
	// outputs on, before the frame thread exists.
	//
	// A failure leaves the target set *empty*, which the frame loop already tolerates as the state
	// between `TargetsInvalidated` and `Reconfigured`, and `Status()` is where the reason lives.
	[[nodiscard]] Result<void> Open();

	// Seam/Presenter.h.
	[[nodiscard]] std::span<const RenderTarget> Targets() const override;

	[[nodiscard]] std::optional<std::uint32_t> AcquireTarget() override;

	[[nodiscard]] Result<void> Present(std::span<const PresentLayer> layers) override;

	void Reconfigure(const OutputConfiguration& wanted) override;

	// Everything this output has to finish once the socket has been read to empty: a configure the
	// host sent, and a commit that was waiting on a composite.
	//
	// Public where a virtual output's `Advance` is, and for the same reason — there is a device above
	// it that owns the drain. The obligation that comes with it is unchanged: all four signals emit
	// from here, so whoever calls this is the frame thread.
	void Settle();

	// When this output next wants looking at. `Duration::max()`'s instant where it is quiet, which is
	// the identity of a fold rather than a case to test.
	//
	// **Quiet is the ordinary answer, because the socket is a real descriptor.** The one thing that is
	// not on the socket is a held commit: no file becomes readable when the GPU finishes, so a
	// deferred commit asks to be looked at again soon, and how soon is what that path costs.
	[[nodiscard]] Instant NextEvent() const noexcept;

	[[nodiscard]] const OutputConfiguration& Configuration() const noexcept { return m_Configuration; }

	// **Two while the host has asked for a frame, one otherwise — and the condition is the entire
	// fix.** See decision 135.
	//
	// `wp_presentation_feedback` says a frame is *on the glass*, which is a refresh too late to be a
	// permission to draw the next one: a loop that waits for it starts every frame a period behind and
	// this window presents on every other vblank — 38 fps on a 60 Hz host, with nothing anywhere
	// reporting a miss. Answering a flat two instead only moves the failure: the loop then commits the
	// moment it has rendered, two commits land inside one host refresh, and the host discards the first
	// — measured at a third of every frame thrown away, each one invalidating the clock and re-damaging
	// the whole output.
	//
	// So the second commit is not gyro's to take by arithmetic. `wl_surface.frame` is the host saying
	// *now*, and it arrives before the host's next composite rather than after its last one, which is
	// exactly the moment one more frame can be accepted without superseding anything. Every other
	// Wayland client paces on it; this one keeps presentation feedback for the clock, where the
	// timestamp is what is wanted, and takes its pacing from the invitation.
	//
	// Clamped by the ring, because an outstanding commit is an image the host is holding: the loop
	// needs one left to render into or `AcquireTarget` answers nothing and the frame is skipped anyway.
	[[nodiscard]] std::uint32_t CommitDepth() const noexcept override
	{
		if (!m_Invited || m_TargetCount < 2)
		{
			return 1;
		}

		return std::min(NestedCommitDepth, m_TargetCount - 1);
	}

	// Why the target set is empty, where it is. Success once a set has been built.
	[[nodiscard]] const Result<void>& Status() const noexcept { return m_Status; }

	// Whether this window's commits carry acquire and release points. False until the first
	// non-immediate point, because a surface that has taken the syncobj role must name both on every
	// commit — so the role is taken when there is something to say rather than at construction, which
	// is what keeps a device that finishes inside `Record` off the path entirely.
	[[nodiscard]] bool IsExplicitlySynchronized() const noexcept { return m_SyncSurface.IsValid(); }

	// How many commits have been held for a composite rather than going out at once. Zero on the
	// explicit-sync path and on the floor tier; anything else is the figure that says why the window
	// feels a frame behind.
	[[nodiscard]] std::uint64_t Held() const noexcept { return m_HeldCommits; }

	// The user closed the window. The ordinary way to quit a nested session, and not a failure.
	[[nodiscard]] bool IsClosed() const noexcept { return m_Closed; }

	// How many commits this output has accepted, matching `VirtualOutput::Commits`.
	std::uint64_t Commits = 0;

	// How many frames the host said it never showed. Worth a line at shutdown: a session where this is
	// not zero is one whose pacing figures have holes in them.
	std::uint64_t Discarded = 0;

private:
	// What a target is doing. `Held` is the state a host is reading it in, and what ends it depends on
	// which mechanism that commit used — which is why the two are tracked per target rather than per
	// output: enabling explicit sync mid-session leaves one buffer out there whose release is still a
	// `wl_buffer.release`.
	enum class TargetState : std::uint8_t
	{
		Free,
		Acquired,
		Committed,
		Held,
	};

	enum class ReleaseKind : std::uint8_t
	{
		None,
		BufferEvent,
		TimelinePoint,
	};

	// `wl_buffer.release`: the host has finished with this image. One listener per target, because a
	// listener names one object and a target set is rebuilt on every resize.
	class BufferRelease final : public Wayland::WlBufferListener
	{
	public:
		void OnRelease() override { Released = true; }

		bool Released = false;
	};

	// The one-shot object the host answers a commit with. Reconstructed per frame rather than reused,
	// because a listener names one object for its life and the host destroys this one as soon as it
	// has spoken — which is also why the id is unbound from inside the handler.
	class Feedback final : public Wayland::WpPresentationFeedbackListener
	{
	public:
		Feedback(NestedOutput& output, std::uint32_t target) noexcept : m_Output{ &output }, m_Target{ target } {}

		// Which of the host's outputs the surface is mostly on. Not read: gyro binds no `wl_output`, so
		// there is nothing to resolve the proxy against, and the presenter is already the answer to
		// which of *gyro's* outputs is speaking.
		void OnSyncOutput(Wayland::WlOutput) override {}

		void OnPresented(
			std::uint32_t tvSecHi,
			std::uint32_t tvSecLo,
			std::uint32_t tvNsec,
			std::uint32_t refresh,
			std::uint32_t seqHi,
			std::uint32_t seqLo,
			Wayland::WpPresentationFeedbackKind flags
		) override;

		void OnDiscarded() override;

	private:
		NestedOutput* m_Output = nullptr;

		// Which image this commit was of. A commit ahead of another is a commit about a *different*
		// target, so the answer has to name one — releasing every committed image on the first
		// completion would hand back the one the host has not spoken about yet.
		std::uint32_t m_Target = 0;
	};

	// `wl_surface.frame`: the host asking for the next frame. One per commit, for `Feedback`'s reason —
	// a listener names one object for its life and the host destroys this one when it fires.
	class FrameCallback final : public Wayland::WlCallbackListener
	{
	public:
		FrameCallback(NestedOutput& output, std::uint32_t target) noexcept : m_Output{ &output }, m_Target{ target } {}

		// The argument is a host timestamp on a base nothing here shares, so it is dropped: what this
		// event carries that gyro wants is that it happened.
		void OnDone(std::uint32_t) override { m_Output->OnInvited(m_Target); }

	private:
		NestedOutput* m_Output = nullptr;
		std::uint32_t m_Target = 0;
	};

	// `xdg_surface.configure`: the point at which everything the host has said since the last one
	// becomes true at once.
	class Surface final : public Wayland::XdgSurfaceListener
	{
	public:
		explicit Surface(NestedOutput& output) noexcept : m_Output{ &output } {}

		void OnConfigure(std::uint32_t serial) override;

	private:
		NestedOutput* m_Output = nullptr;
	};

	// `xdg_toplevel`: the size, and the close button.
	class Toplevel final : public Wayland::XdgToplevelListener
	{
	public:
		explicit Toplevel(NestedOutput& output) noexcept : m_Output{ &output } {}

		void OnConfigure(std::int32_t width, std::int32_t height, std::span<const std::byte> states) override;

		void OnClose() override;

		// The largest the host would like this window to be. A hint rather than a constraint, and gyro
		// does not resize itself to fit one — `--output` is what says how large a nested output is.
		void OnConfigureBounds(std::int32_t, std::int32_t) override {}

		// Which of maximize, fullscreen, minimize and the window menu the host offers. Nothing here
		// asks for any of them.
		void OnWmCapabilities(std::span<const std::byte>) override {}

	private:
		NestedOutput* m_Output = nullptr;
	};

	struct Target
	{
		DmabufBuffer Buffer;
		Wayland::WlBuffer Handle;
		std::optional<BufferRelease> Release;

		// The host's end of explicit sync for this image, in both of the forms it takes: the syncobj
		// gyro made and reads, and the host's handle on the same object. One per target rather than one
		// per output, which the protocol asks for outright — a compositor may signal release points out
		// of order, so a shared timeline would report a buffer free because a *later* one was.
		DrmTimeline Timeline;
		Wayland::WpLinuxDrmSyncobjTimelineV1 Imported;
		std::uint64_t Point = 0;

		// The one-shot feedback object for the commit this image is in, live only while it is
		// `Committed`. Per target rather than per output for `Release` and `Timeline`'s reason exactly:
		// with more than one commit outstanding there is more than one of these in the air at a time,
		// and a single slot would emplace over a listener the host has still to speak to.
		std::optional<Feedback> Listener;

		// The invitation for the commit this image is in, live for the same window as `Listener`.
		std::optional<FrameCallback> Invitation;

		TargetState State = TargetState::Free;
		ReleaseKind Awaiting = ReleaseKind::None;
	};

	void OnPresented(const PresentationInfo& info, std::uint32_t target);

	void OnDiscarded(std::uint32_t target);

	// The host has asked for another frame. It is a permission rather than a wake: the frame loop is
	// already scheduled by its own clock, and what this changes is what it is allowed to do when it
	// gets there.
	void OnInvited(std::uint32_t target) noexcept;

	// The commit this image was in has been answered, whichever way. The listener is dropped and the
	// image moves to `Held`, because either way the host is still holding it and gives it back in its
	// own time.
	void Retire(std::uint32_t target) noexcept;

	void OnConfigured(std::uint32_t serial);

	void OnResized(PixelSize<DeviceSpace> size);

	void OnClosed();

	// Put the pending layer on the wire. Everything `Present` does that is not a check, so that the
	// held path and the immediate one commit through one piece of code rather than two that can drift.
	[[nodiscard]] Result<void> Commit();

	// Take the syncobj role on this surface, and import the renderer's timeline behind it.
	[[nodiscard]] bool EnsureExplicitSync(SyncPoint acquire);

	// Whether the host has given this image back, polled rather than waited on.
	[[nodiscard]] bool IsReleased(const Target& target) const noexcept;

	// Choose a format the host and the device both accept, and allocate the ring under it.
	[[nodiscard]] Result<void> BuildTargets();

	// Wrap one allocated buffer as a `wl_buffer` through `zwp_linux_buffer_params_v1`.
	[[nodiscard]] Result<void> Wrap(Target& target);

	void DropTargets() noexcept;

	NestedHost* m_Host = nullptr;
	IDmabufAllocator* m_Allocator = nullptr;
	const IRenderer* m_Completion = nullptr;
	NestedOutputPolicy m_Policy{};

	OutputConfiguration m_Configuration{};
	OutputConfiguration m_Wanted{};
	Result<void> m_Status{};

	Wayland::WlSurface m_Surface;
	Wayland::XdgSurface m_XdgSurface;
	Wayland::XdgToplevel m_Toplevel;
	Wayland::ZxdgToplevelDecorationV1 m_Decoration;
	Wayland::WpLinuxDrmSyncobjSurfaceV1 m_SyncSurface;
	Wayland::WpLinuxDrmSyncobjTimelineV1 m_Acquire;

	// The descriptor `m_Acquire` was imported from, so a renderer that was replaced — decision 41's
	// migration — is noticed rather than silently addressed on the old timeline.
	RawFd m_AcquireFrom;

	Wayland::WlSurfaceIgnoring m_SurfaceEvents;
	Wayland::ZxdgToplevelDecorationV1Ignoring m_DecorationEvents;
	std::optional<Surface> m_SurfaceListener;
	std::optional<Toplevel> m_ToplevelListener;

	std::array<Target, MaxTargets> m_Targets{};
	std::array<RenderTarget, MaxTargets> m_Descriptions{};
	std::uint32_t m_TargetCount = 0;
	std::uint32_t m_Next = 0;

	// The layer waiting to be committed, and whether it is waiting on anything.
	PresentLayer m_Pending{};
	bool m_Deferred = false;

	// The last configure the host sent, acknowledged. Held as an optional rather than a flag because
	// its absence is what says the window has never been configured, which is the one state in which
	// attaching a buffer would end the connection.
	std::optional<std::uint32_t> m_Serial;

	// A resize the drain has not acted on yet — the host's, or gyro's own `Reconfigure`.
	bool m_Resize = false;

	// Whether the host has asked for a frame that has not been given to it. Cleared by the commit that
	// answers it, so a single invitation buys a single extra commit.
	bool m_Invited = false;

	bool m_Closed = false;
	std::uint64_t m_HeldCommits = 0;
};
} // namespace Nested
