#include "Nested/Output.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Wire/Message.h"

namespace Nested
{
namespace
{
// How long a held commit waits before it is looked at again.
//
// **A number this file would rather not have, and says so.** Nothing makes a file readable when the
// GPU finishes, so the only way to notice on the fallback path is to come back and ask. Short enough
// that it does not add a visible fraction of a frame at 60 Hz, long enough that it is not a spin —
// and it is only ever armed while a composite is actually outstanding, so an idle session still costs
// nothing, which is what Docs/Architecture.md#doing-nothing-must-cost-nothing asks. On the
// explicit-sync path this is never reached.
constexpr Duration CompletionPoll = std::chrono::microseconds{ 500 };

[[nodiscard]] constexpr std::uint32_t High(std::uint64_t value) noexcept
{
	return static_cast<std::uint32_t>(value >> 32);
}

[[nodiscard]] constexpr std::uint32_t Low(std::uint64_t value) noexcept
{
	return static_cast<std::uint32_t>(value & 0xffffffffU);
}

[[nodiscard]] constexpr std::uint64_t Wide(std::uint32_t high, std::uint32_t low) noexcept
{
	return (static_cast<std::uint64_t>(high) << 32) | low;
}

// `zwp_linux_buffer_params_v1`'s two events, and neither reaches this backend.
//
// `created` is the answer to the *deferred* `create` request, which a nested output does not use —
// see `Wrap` — so the listener it would have to bind is null, and the generated dispatcher's rule
// that an id is never live without an implementation is what makes returning null a refusal rather
// than a hole. `failed` on the immediate path means the host built a `wl_buffer` it has already
// marked broken, which shows up as a window that stays blank; it is recorded so the log can say so
// rather than acted on, because there is nothing to fall back to at that point.
class Params final : public Wayland::ZwpLinuxBufferParamsV1Listener
{
public:
	Wayland::WlBufferListener* OnCreated(Wayland::WlBuffer) override { return nullptr; }

	void OnFailed() override { spdlog::error("the wayland host refused a nested output's target as a wl_buffer"); }
};

[[nodiscard]] constexpr bool
Has(Wayland::WpPresentationFeedbackKind flags, Wayland::WpPresentationFeedbackKind wanted) noexcept
{
	return (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(wanted)) != 0;
}
} // namespace

void NestedOutput::Feedback::OnPresented(
	std::uint32_t tvSecHi,
	std::uint32_t tvSecLo,
	std::uint32_t tvNsec,
	std::uint32_t refresh,
	std::uint32_t seqHi,
	std::uint32_t seqLo,
	Wayland::WpPresentationFeedbackKind flags
)
{
	// Decision 57's conversion, at the one place a nested timestamp enters gyro. Seconds and
	// nanoseconds are separate on the wire because the protocol predates 64-bit arguments; nothing
	// downstream ever sees either again.
	const std::int64_t nanoseconds =
		static_cast<std::int64_t>(Wide(tvSecHi, tvSecLo)) * 1'000'000'000 + static_cast<std::int64_t>(tvNsec);

	const PresentationInfo info{
		.PresentedAt = Monotonic::FromNanoseconds(nanoseconds),

		// What the host says its own next refresh is, which is the interval this output actually ran
		// at. Zero where it does not know, which Seam/PresentationInfo.h says is a state the clock can
		// represent and a guess is not.
		.Period = Duration{ refresh },
		.Sequence = Wide(seqHi, seqLo),
		.Vsync = Has(flags, Wayland::WpPresentationFeedbackKind::Vsync),

		// **Both halves, and the second is gyro's rather than the host's.** The host saying its
		// timestamp came from display hardware means nothing if it is quoting a clock this process
		// does not read, so a host on anything but `CLOCK_MONOTONIC` is not precise however good its
		// hardware is. That is the *must say so rather than fill the field in* rule with the one
		// caveat this backend actually has.
		.HardwareClock = Has(flags, Wayland::WpPresentationFeedbackKind::HwClock) && m_Output->m_Host->IsMonotonic(),

		// The host telling gyro its dmabuf reached a plane. Information about the *host's* behaviour,
		// never about gyro's own plane assignment.
		.ZeroCopy = Has(flags, Wayland::WpPresentationFeedbackKind::ZeroCopy),
	};

	m_Output->OnPresented(info, m_Target);
}

void NestedOutput::Feedback::OnDiscarded()
{
	m_Output->OnDiscarded(m_Target);
}

void NestedOutput::Surface::OnConfigure(std::uint32_t serial)
{
	m_Output->OnConfigured(serial);
}

void NestedOutput::Toplevel::OnConfigure(std::int32_t width, std::int32_t height, std::span<const std::byte>)
{
	// The states array is read past for now. Maximized, fullscreen, activated and the tiled edges are
	// all layout facts a compositor acts on, and gyro's window is one output rather than a window in a
	// session it is arranging — what it needs from a configure is the extent. `activated` becomes
	// interesting the day there is an input path, which decision 81 records as the thing this backend
	// is still missing.
	m_Output->OnResized(PixelSize<DeviceSpace>{ width, height });
}

void NestedOutput::Toplevel::OnClose()
{
	m_Output->OnClosed();
}

NestedOutput::NestedOutput(
	NestedHost& host,
	IDmabufAllocator& allocator,
	const IRenderer* completion,
	const OutputConfiguration& configuration,
	NestedOutputPolicy policy
)
	: m_Host{ &host }, m_Allocator{ &allocator }, m_Completion{ completion }, m_Policy{ std::move(policy) },
	  m_Configuration{ configuration }, m_Wanted{ configuration }
{
	m_Policy.Targets = std::clamp(m_Policy.Targets, 2U, MaxTargets);
	host.Adopt(*this);
}

NestedOutput::~NestedOutput()
{
	DropTargets();

	// Torn down in the order the protocol requires: a role object before the surface it was made from,
	// and everything before the connection. A host that is already gone makes each of these a no-op,
	// because the connection latches its failure and every verb returns it.
	if (m_SyncSurface.IsValid())
	{
		m_SyncSurface.Destroy();
	}

	if (m_Acquire.IsValid())
	{
		m_Acquire.Destroy();
	}

	if (m_Decoration.IsValid())
	{
		m_Decoration.Destroy();
	}

	if (m_Toplevel.IsValid())
	{
		m_Toplevel.Destroy();
	}

	if (m_XdgSurface.IsValid())
	{
		m_XdgSurface.Destroy();
	}

	if (m_Surface.IsValid())
	{
		m_Surface.Destroy();
	}
}

Result<void> NestedOutput::Open()
{
	const HostGlobals& globals = m_Host->Globals();

	m_Surface = globals.Compositor.CreateSurface(m_SurfaceEvents);

	if (!m_Surface.IsValid())
	{
		return Failure(ENOTCONN, "creating the wl_surface for a nested output");
	}

	m_SurfaceListener.emplace(*this);
	m_XdgSurface = globals.Shell.GetXdgSurface(m_Surface, *m_SurfaceListener);

	m_ToplevelListener.emplace(*this);
	m_Toplevel = m_XdgSurface.GetToplevel(*m_ToplevelListener);

	if (!m_XdgSurface.IsValid() || !m_Toplevel.IsValid())
	{
		return Failure(ENOTCONN, "taking the xdg_toplevel role for a nested output");
	}

	m_Toplevel.SetTitle(m_Policy.Title);
	m_Toplevel.SetAppId(m_Policy.AppId);

	if (m_Policy.Decorate && globals.Decoration.IsValid())
	{
		m_Decoration = globals.Decoration.GetToplevelDecoration(m_Toplevel, m_DecorationEvents);
		m_Decoration.SetMode(Wayland::ZxdgToplevelDecorationV1Mode::ServerSide);
	}

	// **The first commit carries no buffer, and that is the protocol rather than an omission.** A
	// surface takes its role, says nothing else, and commits; the host answers with a configure that
	// is the window's first size. Attaching a buffer before that configure is `unconfigured_buffer`
	// and the end of the connection.
	m_Surface.Commit();

	if (const Result<void> settled = m_Host->Roundtrip(); !settled)
	{
		return settled;
	}

	if (!m_Serial.has_value())
	{
		return Failure(ETIMEDOUT, "the wayland host never configured a nested output's window");
	}

	// **The first configure is adopted here rather than through `Settle`, and it is not a mode
	// change.** There is nothing to invalidate — no images, no renderer bound — and no loop to hear
	// the two signals a transition emits. A window manager that answered with a size of its own has
	// already moved `m_Wanted` through `OnResized`, so this adopts whichever of the two is true:
	// `--output`'s extent on a floating desktop, and the tiler's on a tiling one.
	m_Configuration = m_Wanted;
	m_Resize = false;
	m_Status = BuildTargets();

	if (m_Status)
	{
		// What the host should treat as the window rather than as shadow. Sent once the extent is
		// settled, because it is the extent.
		m_XdgSurface.SetWindowGeometry(0, 0, m_Configuration.Resolution.Width, m_Configuration.Resolution.Height);
	}

	// **Pushed here rather than left to the first commit**, which would work and would be worse. A host
	// imports a dmabuf when it is told about it — a page-table walk and, on some drivers, a
	// `GEM_IMPORT` — and stacking every target of every window onto the same `sendmsg` as the first
	// frame puts that cost inside the first deadline. Sent now, while the loop has not started, it is
	// paid where every other allocation on this path is.
	(void)m_Host->Flush();

	return m_Status;
}

std::span<const RenderTarget> NestedOutput::Targets() const
{
	return { m_Descriptions.data(), m_TargetCount };
}

bool NestedOutput::IsReleased(const Target& target) const noexcept
{
	switch (target.Awaiting)
	{
		case ReleaseKind::None:
			return true;

		case ReleaseKind::BufferEvent:
			return target.Release.has_value() && target.Release->Released;

		case ReleaseKind::TimelinePoint:
			// One ioctl, and only for an image the host is still holding. A ring under no pressure
			// answers from the first free slot and never reaches this; a ring under pressure is asking
			// the only question that has an answer.
			return target.Timeline.Signalled() >= target.Point;
	}

	return true;
}

std::optional<std::uint32_t> NestedOutput::AcquireTarget()
{
	for (std::uint32_t offset = 0; offset < m_TargetCount; ++offset)
	{
		const std::uint32_t candidate = (m_Next + offset) % m_TargetCount;
		Target& target = m_Targets[candidate];

		if (target.State == TargetState::Held && IsReleased(target))
		{
			target.State = TargetState::Free;
			target.Awaiting = ReleaseKind::None;
		}

		if (target.State != TargetState::Free)
		{
			continue;
		}

		target.State = TargetState::Acquired;
		m_Next = (candidate + 1) % m_TargetCount;

		return candidate;
	}

	// The host is holding everything gyro allocated, which Seam/Presenter.h calls an ordinary answer:
	// the frame loop skips this output rather than waiting, because waiting here would put another
	// process's flow control on the frame thread.
	return std::nullopt;
}

Result<void> NestedOutput::Present(std::span<const PresentLayer> layers)
{
	if (!m_Configuration.Powered)
	{
		return Failure(EINVAL, "present on an unpowered nested output");
	}

	if (m_Resize || m_TargetCount == 0)
	{
		return Failure(EBUSY, "nested output is mid-reconfiguration");
	}

	if (m_Deferred)
	{
		return Failure(EBUSY, "a commit is already waiting on this nested output's composite");
	}

	// One surface, so one layer. Stated as a refusal rather than by compositing the rest, because an
	// assigner that produced two here would otherwise be silently ignored on this backend and
	// discovered on the one with planes.
	if (layers.size() != 1)
	{
		return Failure(EINVAL, "a nested output takes exactly one layer");
	}

	const PresentLayer& layer = layers.front();

	if (layer.Target >= m_TargetCount || m_Targets[layer.Target].State != TargetState::Acquired)
	{
		return Failure(EINVAL, "layer names a target that was not acquired");
	}

	m_Pending = layer;

	// Explicit sync where it can be had, and the commit goes out now naming a point the GPU has not
	// reached. Where it cannot, the commit waits — never inline, which is what `Present` may never
	// block means: it is queued here and released by a later drain.
	if (layer.Acquire.IsImmediate() || EnsureExplicitSync(layer.Acquire))
	{
		return Commit();
	}

	m_Deferred = true;
	++m_HeldCommits;

	return {};
}

bool NestedOutput::EnsureExplicitSync(SyncPoint acquire)
{
	if (!m_Host->HasExplicitSync() || !acquire.Timeline.IsValid())
	{
		return false;
	}

	// **The role is taken on the first non-immediate point rather than at construction**, because a
	// surface that has it must name an acquire *and* a release point on every commit that attaches a
	// buffer — `no_acquire_point` otherwise, and the end of the connection. A device that finishes
	// inside `Record` has no point to name, so it never reaches here and never takes the role.
	if (!m_SyncSurface.IsValid())
	{
		// Every target must already carry a release timeline, because from the moment this role
		// exists *every* commit has to name one — so a ring where one image could not get a syncobj
		// stays on the fallback path whole rather than half.
		for (std::uint32_t index = 0; index < m_TargetCount; ++index)
		{
			if (!m_Targets[index].Imported.IsValid())
			{
				return false;
			}
		}

		m_SyncSurface = m_Host->Globals().Syncobj.GetSurface(m_Surface);

		if (!m_SyncSurface.IsValid())
		{
			return false;
		}
	}

	// The renderer's own timeline, imported once. A renderer replaced under decision 41's migration
	// hands out points on a different descriptor, and comparing rather than assuming is what makes
	// that a re-import instead of points named on a timeline nobody signals.
	if (!m_Acquire.IsValid() || m_AcquireFrom.Value != acquire.Timeline.Value)
	{
		if (m_Acquire.IsValid())
		{
			m_Acquire.Destroy();
		}

		// Duplicated because `import_timeline` takes ownership of what it is handed and the renderer
		// owns the original — Seam/SyncPoint.h's descriptor is borrowed for exactly this reason.
		Fd copy = Duplicate(acquire.Timeline);

		if (!copy.IsValid())
		{
			return false;
		}

		m_Acquire = m_Host->Globals().Syncobj.ImportTimeline(std::move(copy));
		m_AcquireFrom = acquire.Timeline;
	}

	return m_Acquire.IsValid();
}

Result<void> NestedOutput::Commit()
{
	Target& target = m_Targets[m_Pending.Target];

	m_Surface.Attach(target.Handle, 0, 0);

	// Buffer coordinates rather than surface coordinates, which is what `damage_buffer` is for and why
	// Geometry/Region.h is templated on the space: the damage the renderer produced is in the target's
	// own grid, and mapping it into the surface's would round in a backend rather than at the seam.
	for (const PixelRect<DeviceSpace>& rect : m_Pending.Damage.Rects())
	{
		m_Surface.DamageBuffer(rect.Origin.X, rect.Origin.Y, rect.Extent.Width, rect.Extent.Height);
	}

	target.Awaiting = ReleaseKind::BufferEvent;

	if (m_SyncSurface.IsValid() && m_Acquire.IsValid() && target.Imported.IsValid())
	{
		m_SyncSurface.SetAcquirePoint(m_Acquire, High(m_Pending.Acquire.Value), Low(m_Pending.Acquire.Value));

		// Monotone per target, because a syncobj point that went backwards would be signalled the
		// moment it was named. One per commit is all the counter has to be.
		++target.Point;
		m_SyncSurface.SetReleasePoint(target.Imported, High(target.Point), Low(target.Point));

		target.Awaiting = ReleaseKind::TimelinePoint;
	}

	// Reconstructed rather than reused: a listener names one object for its life, and the host
	// destroys a feedback object as soon as it has spoken. Placement-new into the optional, so nothing
	// on this path allocates — `Present` runs inside Core/FrameSection.h's guard. Into the target's own
	// slot, because the commit ahead of this one is still waiting to be answered about its image.
	target.Listener.emplace(*this, m_Pending.Target);
	(void)m_Host->Globals().Presentation.Feedback(m_Surface, *target.Listener);

	// The invitation this commit spends, and the one it asks for. Requested on every commit rather than
	// only where one is owed, because a host answers `frame` for the surface's next composite and a
	// commit that asked for nothing is a window that never hears *now* again.
	m_Invited = false;
	target.Invitation.emplace(*this, m_Pending.Target);
	(void)m_Surface.Frame(*target.Invitation);

	m_Surface.Commit();

	target.State = TargetState::Committed;
	m_Deferred = false;
	++Commits;

	// **Flushed here rather than left to the next drain**, because the next drain is on the far side
	// of a wait the loop is about to enter — and the thing it would be waiting for is the feedback for
	// this commit. A frame left in the output buffer is a frame the host never hears about.
	return m_Host->Flush();
}

void NestedOutput::Settle()
{
	// The configure first, because adopting it drops whatever was in flight — including a commit that
	// was waiting on a composite into a target that is about to stop existing.
	if (m_Resize)
	{
		m_Resize = false;

		// The images go before the new set exists, which is the order `IRenderer::ReleaseTargets` is
		// written for and which Frame/Loop.h relies on: the whole-output damage this accumulates is in
		// the extent the output *had*, and the re-damage after the adoption is what covers a mode that
		// grew.
		DropTargets();
		TargetsInvalidated.Emit();

		m_Configuration = m_Wanted;
		m_Status = BuildTargets();

		// **The window's size follows the buffer gyro attaches**, so there is nothing to program and
		// nothing to wait for — which is why there is no latency here where Headless/Output.h has one.
		// What is preserved is the half that is a contract rather than a duration: `Reconfigure`
		// returned without having done it, the images were released before the new set existed, and
		// both completions arrive on a drain.
		m_XdgSurface.SetWindowGeometry(0, 0, m_Configuration.Resolution.Width, m_Configuration.Resolution.Height);
		Reconfigured.Emit(m_Configuration);

		return;
	}

	if (!m_Deferred || m_Completion == nullptr)
	{
		return;
	}

	if (!m_Completion->IsComplete(m_Pending.Acquire))
	{
		return;
	}

	// The composite has landed, so the commit that was waiting for it goes now. A failure is dropped
	// here rather than returned: the call site was `Present`, which has already answered, and the
	// connection latches its own failure for the drain to report.
	(void)Commit();
}

Instant NestedOutput::NextEvent() const noexcept
{
	// The socket is what wakes this backend for everything else. The one thing that is not on the
	// socket is a composite, and this is the cost of the path that has to poll for one.
	if (m_Deferred)
	{
		MonotonicClock clock;

		return Advanced(clock.Now(), CompletionPoll);
	}

	return Instant{ Duration::max() };
}

void NestedOutput::Reconfigure(const OutputConfiguration& wanted)
{
	// Decision 73: this returns before anything has been programmed, and completion arrives as an
	// event the loop already polls for. Here the *host* is the hardware, and a nested window's size is
	// whatever buffer gyro attaches — so what a reconfiguration does is release the target set and
	// build another, which the drain does.
	m_Wanted = wanted;
	m_Resize = true;
}

void NestedOutput::OnConfigured(std::uint32_t serial)
{
	// Everything the host has said since the last configure becomes true at once, which is what the
	// ack is acknowledging. It goes out on this connection's next flush, and the drain flushes.
	m_Serial = serial;
	m_XdgSurface.AckConfigure(serial);
}

void NestedOutput::OnResized(PixelSize<DeviceSpace> size)
{
	// **Zero is the host saying *you choose*,** which is the first configure and every one after it
	// where the window is not being resized. gyro chooses `--output`, so this is not a mode change.
	if (size.IsEmpty() || size == m_Configuration.Resolution)
	{
		return;
	}

	// A window resize *is* a mode change, per Docs/Architecture.md#nested-wayland. The generation moves
	// so that a completion is legible as this one rather than a superseded one, exactly as it would on
	// a panel — Seam/OutputConfiguration.h has the argument, and the point of building it here is that
	// real hotplug is then the same path rather than a rewrite.
	m_Wanted = m_Configuration;
	m_Wanted.Generation = m_Configuration.Generation + 1;
	m_Wanted.Resolution = size;
	m_Resize = true;
}

void NestedOutput::OnClosed()
{
	m_Closed = true;
}

void NestedOutput::Retire(std::uint32_t target) noexcept
{
	if (target >= m_TargetCount)
	{
		return;
	}

	Target& retiring = m_Targets[target];

	if (retiring.Listener.has_value())
	{
		// The host destroys the object the moment it has spoken, so the id is already free on its side;
		// unbinding is what lets Wire/Connection.h recycle it when the `delete_id` lands — and
		// recycling is what keeps the object table from climbing by one per frame forever.
		m_Host->Connection().Unbind(retiring.Listener->Object().Id());
		retiring.Listener.reset();
	}

	if (retiring.Invitation.has_value())
	{
		m_Host->Connection().Unbind(retiring.Invitation->Object().Id());
		retiring.Invitation.reset();
	}

	if (retiring.State == TargetState::Committed)
	{
		retiring.State = TargetState::Held;
	}
}

void NestedOutput::OnPresented(const PresentationInfo& info, std::uint32_t target)
{
	Retire(target);

	Presented.Emit(info);
}

void NestedOutput::OnInvited(std::uint32_t target) noexcept
{
	m_Invited = true;

	// The object is one-shot and has already spoken, so the binding goes now — the alternative is one
	// dead id per frame held until the commit it belongs to is answered.
	if (target < m_TargetCount && m_Targets[target].Invitation.has_value())
	{
		m_Host->Connection().Unbind(m_Targets[target].Invitation->Object().Id());
		m_Targets[target].Invitation.reset();
	}
}

void NestedOutput::OnDiscarded(std::uint32_t target)
{
	// **The buffer is still the host's**, which is the difference between this and a failed commit: the
	// frame was never shown and the image is still out on loan, so the target moves to `Held` exactly
	// as a presented one does and comes back when the host says so. That is `Retire`, and it is the
	// same call either way.
	Retire(target);

	// **Everything else in flight goes with it, and this is the one place a depth above one costs
	// something.** `Missed` has the frame loop drop what it committed and start its clock again, and a
	// second commit still out there would answer afterwards against a prediction that no longer
	// includes it. A host that discarded one frame is a host that is occluding, moving, or superseding
	// the window, so the frame after it is very rarely the one to save.
	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		if (index != target)
		{
			Retire(index);
		}
	}

	++Discarded;

	Missed.Emit();
}

Result<void> NestedOutput::BuildTargets()
{
	const PixelSize<DeviceSpace> size = m_Configuration.Resolution;

	if (!m_Configuration.Powered || size.IsEmpty())
	{
		// Not a failure. An unpowered output has no images by design, and reporting the last
		// allocation error here would make a deliberate power-down look like a broken device.
		return {};
	}

	const std::uint32_t code = m_Configuration.Format.IsValid() ? m_Configuration.Format.Code : FormatXrgb8888;
	const std::vector<PixelFormat> candidates = m_Host->Support().Candidates(code);

	if (candidates.empty())
	{
		return Failure(ENOTSUP, "the wayland host offers no modifier for the format this output is configured as");
	}

	// **The host ranks and the device vetoes.** Walked in the host's own order, and the first pair both
	// ends accept is what the whole ring is allocated under — a set with two modifiers in it would be
	// two `wl_buffer`s the host imports differently, which is a picture that changes when the ring
	// wraps.
	PixelFormat chosen{};

	for (const PixelFormat& candidate : candidates)
	{
		Result<DmabufBuffer> first = m_Allocator->Allocate(size, candidate);

		if (!first)
		{
			continue;
		}

		chosen = first->Format();
		m_Targets[0].Buffer = std::move(*first);

		break;
	}

	if (!chosen.IsValid())
	{
		return Failure(ENOTSUP, "no format the host offered is one this device will export a target under");
	}

	for (std::uint32_t index = 1; index < m_Policy.Targets; ++index)
	{
		Result<DmabufBuffer> buffer = m_Allocator->Allocate(size, chosen);

		if (!buffer)
		{
			// Partial success is not expressible, for `IRenderer::BindTargets`' reason one seam over: a
			// set is what `AcquireTarget` indexes into, so half of one is a numbering with holes.
			DropTargets();

			return std::unexpected{ buffer.error() };
		}

		m_Targets[index].Buffer = std::move(*buffer);
	}

	for (std::uint32_t index = 0; index < m_Policy.Targets; ++index)
	{
		if (const Result<void> wrapped = Wrap(m_Targets[index]); !wrapped)
		{
			DropTargets();

			return wrapped;
		}

		m_Descriptions[index] = m_Targets[index].Buffer.Describe();
		m_Targets[index].State = TargetState::Free;
	}

	m_TargetCount = m_Policy.Targets;

	// **What was achieved rather than what was asked for.** Seam/OutputConfiguration.h makes the
	// comparison between the two the way a request the hardware could not honour reports itself, and a
	// host that would not take `XR24` linear is exactly that case — so the format the ring really is
	// goes into the configuration `Reconfigured` carries.
	m_Configuration.Format = chosen;

	const std::uint64_t device = m_Host->Support().DeviceFor(chosen);

	// Named rather than discovered as a host protocol error three frames later, which is decision
	// 120's *what must be checked at bind*. A laptop with two GPUs is the ordinary case: the tranche's
	// device is the constraint, and gyro drawing on the other one is a condition worth a line even
	// where the import happens to work.
	spdlog::info(
		"nested output: {} targets {} via {}, host tranche device {:#x}", size, chosen, m_Allocator->Name(), device
	);

	return {};
}

Result<void> NestedOutput::Wrap(Target& target)
{
	const RenderTarget described = target.Buffer.Describe();
	const DmabufImage* const image = described.AsDmabuf();

	if (image == nullptr || image->PlaneCount == 0)
	{
		return Failure(EINVAL, "a nested output's target is not a dmabuf");
	}

	// The params object is transient — one buffer's worth of state on the host, destroyed by
	// `create_immed` — so its listener is too. `failed` is the only event it has, and on the immediate
	// path it means the host built a `wl_buffer` it has already marked broken.
	Params failures;
	Wayland::ZwpLinuxBufferParamsV1 params = m_Host->Globals().Dmabuf.CreateParams(failures);

	if (!params.IsValid())
	{
		return Failure(ENOTCONN, "creating the buffer params for a nested output's target");
	}

	for (std::uint32_t plane = 0; plane < image->PlaneCount; ++plane)
	{
		const DmabufPlane& described_plane = image->Planes[plane];

		// Duplicated because the request takes ownership and the buffer owns the original —
		// Seam/RenderTarget.h's plane descriptor is borrowed, and this is the one place that matters.
		Fd copy = Duplicate(described_plane.Descriptor);

		if (!copy.IsValid())
		{
			return Failure(errno, "duplicating a target's descriptor for the wayland host");
		}

		params.Add(
			std::move(copy),
			plane,
			described_plane.Offset,
			described_plane.Stride,
			High(described.Format.Modifier),
			Low(described.Format.Modifier)
		);
	}

	target.Release.emplace();

	// `create_immed` rather than `create`: the host answers `create` with an event, and a target set
	// built across a roundtrip per image would put the window resize path behind several. Failure is
	// then either a protocol error — which ends the connection and is a bug in the arguments above —
	// or a `wl_buffer` the host marks broken, which is what the params listener would hear.
	target.Handle = params.CreateImmed(
		described.Size.Width,
		described.Size.Height,
		described.Format.Code,
		static_cast<Wayland::ZwpLinuxBufferParamsV1Flags>(0),
		*target.Release
	);

	params.Destroy();

	if (!target.Handle.IsValid())
	{
		return Failure(ENOTCONN, "importing a nested output's target as a wl_buffer");
	}

	// One release timeline per target, imported into the host. Created here rather than at the first
	// commit because a resize rebuilds the ring and a syncobj is a device call — which belongs on this
	// path, where allocation already does, and never inside a frame.
	if (m_Host->HasExplicitSync())
	{
		Result<DrmTimeline> timeline = m_Host->SyncDevice().CreateTimeline();

		if (timeline)
		{
			Fd copy = Duplicate(timeline->Descriptor());

			if (copy.IsValid())
			{
				const Wayland::WpLinuxDrmSyncobjTimelineV1 imported =
					m_Host->Globals().Syncobj.ImportTimeline(std::move(copy));

				if (imported.IsValid())
				{
					target.Timeline = std::move(*timeline);
					target.Imported = imported;
				}
			}
		}
		else
		{
			// Reported once and carried on from: a target without a release timeline falls back to
			// `wl_buffer.release`, which is correct and only slower.
			spdlog::warn("no release timeline for a nested target: {}", timeline.error());
		}
	}

	return {};
}

void NestedOutput::DropTargets() noexcept
{
	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		Target& target = m_Targets[index];

		if (target.Handle.IsValid())
		{
			target.Handle.Destroy();
		}

		target.Release.reset();

		// A frame in flight names a target that no longer exists, so its feedback is nothing this output
		// can act on. Unbound rather than left, because the host will still answer it and an event for an
		// id nothing is bound to is the end of the connection.
		if (target.Listener.has_value())
		{
			m_Host->Connection().Unbind(target.Listener->Object().Id());
			target.Listener.reset();
		}

		if (target.Invitation.has_value())
		{
			m_Host->Connection().Unbind(target.Invitation->Object().Id());
			target.Invitation.reset();
		}

		if (target.Imported.IsValid())
		{
			target.Imported.Destroy();
		}

		target.Timeline = DrmTimeline{};
		target.Point = 0;
		target.Buffer = DmabufBuffer{};
		target.State = TargetState::Free;
		target.Awaiting = ReleaseKind::None;

		m_Descriptions[index] = RenderTarget{};
	}

	m_TargetCount = 0;
	m_Next = 0;
	m_Deferred = false;
}
} // namespace Nested
