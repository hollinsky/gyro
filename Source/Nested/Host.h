#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Nested/Feedback.h"
#include "Nested/Sync.h"
#include "Seam/EventSource.h"
#include "Wayland/LinuxDmabufV1.h"
#include "Wayland/LinuxDrmSyncobjV1.h"
#include "Wayland/PresentationTime.h"
#include "Wayland/Wayland.h"
#include "Wayland/XdgDecorationUnstableV1.h"
#include "Wayland/XdgShell.h"
#include "Wire/Connection.h"

// gyro's connection to the compositor it is running inside, and everything that is one connection's
// rather than one window's.
//
// **One connection, pumped by the frame thread**, which is decision 81 as revised. Two `wl_display`
// connections are two clients of the host and a `wl_surface` id means nothing outside the one that
// made it, so the connection that owns the windows owns both presentation feedback and input; and
// partitioning one connection by event queue — what libwayland-client does — costs a lock the frame
// thread meets and the dispatch thread holds. So: one reader, and the reader is the thread whose
// deadline depends on what arrives. Nothing here is safe to touch from two threads and nothing here
// locks.
//
// **It is the backend's `IEventSource` because the descriptor is the backend's.** Seam/EventSource.h
// puts a source at whatever granularity its file actually has: KMS has one DRM file per device
// carrying flips for every CRTC, and this has one socket carrying feedback for every window gyro
// opened. One descriptor and N presenters, both times — which is why the presenters are what this
// emits *into* rather than sources of their own.
//
// **Construction happens before the frame thread exists, and that is the only handoff.** The
// composition root opens this, binds the globals, and builds the outputs while it is still the only
// thread in the process; `std::thread`'s construction is the happens-before that hands the whole of
// it over. After that the socket has exactly one reader for the rest of its life.
//
// **Input is bound here and handed across.** *(Decision 173; this said there was no input path.)*
// `wl_seat` is one of the globals, the pointer's events are decoded on this thread with everything
// else on the socket, and `Nested/Input.h` is the queue and the doorbell that carry them to the
// dispatch thread — which is the handoff decision 81 recorded as the price of one connection, built
// in the direction that owes no deadline.

namespace Nested
{
class NestedInput;
class NestedOutput;

// How many host windows one connection carries. Matched to Frame's `MaxOutputs` the way
// `MaxVirtualOutputs` is: `Nested` cannot name `Frame` — the module graph does not have that edge and
// should not — so the number is repeated with the reason rather than reached for.
inline constexpr std::size_t MaxNestedOutputs = 16;

// The globals a nested output drives, bound once.
//
// `wl_seat` is not here because it is not one *object*: what a seat gives is a pointer, a keyboard
// and a touch device, and `Nested/Input.h` owns all four. `wp_viewporter` is
// absent because nothing scales a nested surface yet: gyro's composite is the mode's resolution and
// the window is that size, so a viewport would be an identity transform the host still has to apply.
// It comes back with fractional output scale, which is what it is for.
struct HostGlobals
{
	Wayland::WlCompositor Compositor;
	Wayland::XdgWmBase Shell;
	Wayland::ZwpLinuxDmabufV1 Dmabuf;
	Wayland::WpPresentation Presentation;

	// Optional. A host without it is a host gyro falls back on, not one it refuses.
	Wayland::WpLinuxDrmSyncobjManagerV1 Syncobj;
	Wayland::ZxdgDecorationManagerV1 Decoration;
};

class NestedHost final : public IEventSource
{
public:
	// The clock is the input path's: a pointer event is stamped when the socket produced it, which is
	// the `t₀` decision 26 has an animation start from. The host itself reads no time.
	explicit NestedHost(const IClock& clock);

	~NestedHost() override;

	// Connect, bind, and learn what the host will accept.
	//
	// Everything fallible about a nested session happens here and comes back as a sentence: no socket,
	// a host missing a global gyro cannot work without, a `zwp_linux_dmabuf_v1` too old to have
	// feedback. `ENOTCONN` for the socket, `ENOPROTOOPT` for a missing global, and the message names
	// which — because the machine that hits this is usually somebody's desktop and the fix is a
	// different host rather than a different gyro.
	[[nodiscard]] Result<void> Open();

	// Seam/EventSource.h's two verbs.
	[[nodiscard]] RawFd Descriptor() const noexcept override;

	// Read to empty, then finish whatever the outputs were waiting on, then push what that queued.
	//
	// **The order is the content.** Handlers marshal — an `xdg_wm_base.ping` is answered with a pong,
	// a configure with an ack — and a commit held for a composite that has now finished goes out in
	// the same breath. Flushing last is what makes all of that one `sendmsg` instead of three, and
	// leaving it to the next `Present` would leave a pong sitting in the buffer for as long as the
	// output had nothing to draw, which is how a host decides gyro is unresponsive while it is idle.
	[[nodiscard]] Result<void> Drain() override;

	// When this backend next has something to say, or `Duration::max()`'s instant for never.
	//
	// **Never is the ordinary answer, and that is what makes this a real source.** Decision 121 left
	// the question of whether `IEventSource` should be able to answer this open, on the grounds that
	// every source with a genuine answer was a clock-driven fake; this is the first one that is not.
	// The socket becomes readable when the host has something, so the ring wakes on the descriptor and
	// this contributes nothing — except where an output is holding a commit for a composite that has
	// not landed, which nothing will make a file readable for.
	[[nodiscard]] Instant NextEvent() const noexcept;

	// Send everything queued. Called by `Present` at the end of a commit, which is what puts the frame
	// on the wire in the iteration that drew it rather than the next one.
	[[nodiscard]] Result<void> Flush();

	// Marshal a `wl_display.sync` and pump until the host answers it.
	//
	// **The one place this blocks, and it runs before the frame thread exists.** Binding a global,
	// learning what the host will accept, and adopting the first window size are all questions with an
	// answer that arrives later, and there is no loop yet to arrive into. `ETIMEDOUT` rather than
	// forever: a host that has stopped answering should produce a sentence at startup rather than a
	// process that never reaches its first frame and cannot say why.
	[[nodiscard]] Result<void> Roundtrip();

	[[nodiscard]] Wire::Connection& Connection() noexcept { return m_Connection; }

	[[nodiscard]] const Wire::Connection& Connection() const noexcept { return m_Connection; }

	[[nodiscard]] const HostGlobals& Globals() const noexcept { return m_Globals; }

	[[nodiscard]] const DmabufSupport& Support() const noexcept { return m_Feedback.Support(); }

	[[nodiscard]] const DrmSyncobjDevice& SyncDevice() const noexcept { return m_SyncDevice; }

	// Whether a surface may set acquire and release points. Both halves are needed: the host's
	// protocol, and a DRM node to mint the release timeline on.
	[[nodiscard]] bool HasExplicitSync() const noexcept
	{
		return m_Globals.Syncobj.IsValid() && m_SyncDevice.IsValid();
	}

	// Whether the host's presentation timestamps are in gyro's own timebase.
	//
	// `wp_presentation.clock_id` is a `clockid_t`, and decision 57 converts at ingest — but there is
	// nothing to convert *from* if the host is quoting a clock this process does not read. So a host on
	// anything but `CLOCK_MONOTONIC` produces feedback whose `HardwareClock` is false and whose
	// timestamps are the host's own, which is Seam/PresentationInfo.h's *a backend that does not know
	// must say so rather than fill the field in*.
	[[nodiscard]] bool IsMonotonic() const noexcept { return m_Monotonic; }

	// Register an output so the drain finishes its business and the wake folds its answer in. The
	// output is neither copyable nor movable — it is an `IPresenter` — so this takes its address and
	// the composition root is what keeps it alive.
	void Adopt(NestedOutput& output);

	// Tell the input path what this window is now: which surface carries it, and how big that surface
	// is. Called when the window takes its role and again on every resize.
	//
	// **It goes through the host rather than from the output straight to the input**, because the index
	// is the host's — an output does not know which window it is, and the index is what makes the
	// pointer's device the one bound to that output.
	void Track(const NestedOutput& output);

	// The host's seat, as gyro's own device set. Always present, even where the host has no seat: what
	// is then absent is the pointer rather than the source, so the composition root registers one
	// descriptor either way and a host that gains a seat later needs no second path.
	[[nodiscard]] NestedInput& Input() noexcept { return *m_Input; }

	[[nodiscard]] const NestedInput& Input() const noexcept { return *m_Input; }

	// What the host said before it hung up, where `Drain` came back `EPROTO`. The connection carries
	// the text; this is only where a reader is pointed at it.
	[[nodiscard]] const std::optional<Wire::ProtocolFault>& Fault() const noexcept { return m_Connection.Fault(); }

private:
	// The registry, collecting what the host advertises.
	//
	// It binds nothing itself. Binding at `global` time would mean binding in whatever order the host
	// announced and having no way to say *this one is required and missing* — so the names and versions
	// are collected, `Open` binds against the whole set, and a missing global is one sentence rather
	// than a silent absence discovered at the first request.
	class Registry final : public Wayland::WlRegistryListener
	{
	public:
		struct Advertised
		{
			std::uint32_t Name = 0;
			std::uint32_t Version = 0;
		};

		void OnGlobal(std::uint32_t name, std::string_view interface, std::uint32_t version) override;

		void OnGlobalRemove(std::uint32_t name) override;

		[[nodiscard]] Advertised Find(std::string_view interface) const noexcept;

	private:
		// Only the handful gyro binds. A host advertises forty globals and remembering the rest would
		// be a vector that grows with somebody else's protocol suite.
		struct Slot
		{
			std::string_view Interface;
			Advertised Global;
		};

		std::array<Slot, 8> m_Slots{};
		std::size_t m_Count = 0;
	};

	// `xdg_wm_base.ping` has to be answered or the host declares gyro unresponsive and, on most
	// desktops, offers to kill it. It is the one event on this connection that is a liveness check
	// rather than information.
	class Shell final : public Wayland::XdgWmBaseListener
	{
	public:
		void OnPing(std::uint32_t serial) override { Object().Pong(serial); }
	};

	// `wp_presentation.clock_id`, which arrives once and decides whether a timestamp means anything.
	class Presentation final : public Wayland::WpPresentationListener
	{
	public:
		void OnClockId(std::uint32_t clkId) override { Clock = clkId; }

		// A sentinel no `clockid_t` uses, so *not said yet* is distinguishable from `CLOCK_REALTIME`,
		// which is zero.
		std::uint32_t Clock = 0xffffffffU;
	};

	// The barrier `Roundtrip` waits on.
	class Barrier final : public Wayland::WlCallbackListener
	{
	public:
		void OnDone(std::uint32_t) override { Done = true; }

		bool Done = false;
	};

	[[nodiscard]] Result<void> Bind();

	// One drain of the socket into whatever is bound, blocking for at most `within`.
	[[nodiscard]] Result<void> Pump(Duration within);

	Wire::Connection m_Connection;

	Registry m_Registry;
	Shell m_Shell;

	// `zwp_linux_dmabuf_v1`'s own two events are the flat `format` and `modifier` lists, deprecated at
	// version 4 in favour of the feedback object below — so the manager is bound with them ignored in
	// one word, which is what `Ignoring` is for.
	Wayland::ZwpLinuxDmabufV1Ignoring m_DmabufEvents;
	Presentation m_Presentation;
	DmabufFeedback m_Feedback;

	HostGlobals m_Globals{};
	Wayland::ZwpLinuxDmabufFeedbackV1 m_FeedbackObject;

	DrmSyncobjDevice m_SyncDevice;

	// By pointer because `Nested/Input.h` names this file for `MaxNestedOutputs` and a member would
	// close the include cycle. It exists for the whole of the host's life either way.
	std::unique_ptr<NestedInput> m_Input;

	std::array<NestedOutput*, MaxNestedOutputs> m_Outputs{};
	std::size_t m_Count = 0;

	bool m_Monotonic = false;
};
} // namespace Nested
