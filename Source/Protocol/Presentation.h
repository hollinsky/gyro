#pragma once

#include <cstdint>

#include "Protocol/Context.h"
#include "Protocol/Surface.h"
#include "Wayland/Server/PresentationTime.h"
#include "Wayland/Server/Wayland.h"

// `wp_presentation`: when a client's pixels actually became light, and on which panel.
//
// **This is the global whose absence meant gyro could not be nested inside gyro.** `Nested/Host.h`
// requires four interfaces of whatever it is a client of, and this was the one gyro itself did not
// serve — so the inner compositor failed its bind with *the wayland host does not offer an interface a
// nested output needs* and never opened a window. Every other compositor a person might develop
// against has served this for a decade, which is why it went unnoticed for as long as the daily driver
// was somebody else's desktop.
//
// **But the reason to serve it is not the nesting.** A `wl_surface.frame` callback answers *you may
// draw again* and carries a millisecond stamp; it says nothing about *when what you drew was seen*.
// The difference is a whole refresh, and it is the difference between a video player that holds
// audio-video sync and one that drifts, and between a toolkit that can measure its own latency and one
// that can only guess. Decision 115 already derives the fact — an entity's pixels reached the glass at
// an instant — and this is the second consumer of it rather than a second mechanism.
//
// **Advertised at version 1 and not 2**, which is [Compositor.h](Compositor.h)'s rule that the version
// is a promise about the events gyro sends. The two differ only in what `refresh` means on an output
// with no constant refresh rate: 1 obliges zero there and 2 lets a compositor pick a representative
// rate. gyro's world model carries the mode's nominal period and nothing else — the VRR servo commands
// a period on the frame clock and never writes one into `Scene/Output.h` — so there is no output here
// whose refresh is variable in a way this module can see, and claiming 2 would be claiming an answer
// to a question gyro cannot yet be asked. The number goes up in the commit that builds it.
//
// **The clock is `CLOCK_MONOTONIC`, and that is a fact rather than a choice made here.** Core/Time.h's
// timebase is monotonic throughout, decision 57 converts at ingest and nowhere else, and this event is
// the one place the identifier itself is stated to somebody outside the process. A client must be able
// to read the same clock directly — the protocol says so — which is what rules out a compositor-private
// domain however tidy it would be.
inline constexpr std::uint32_t PresentationVersion = 1;

// One `wp_presentation_feedback`: one content update, and the single event that ends it.
//
// **It has no requests at all**, which makes this the thinnest handler in the module and puts the
// whole of its behaviour in its lifetime. It is created against a surface, staged the way a frame
// callback is, and destroyed by whichever of the two events fires — `presented` and `discarded` are
// both destructors on the client's side, and the resource is gyro's to free.
//
// The back-pointer is [Surface.h](Surface.h)'s `FrameCallback` arrangement exactly: a client that
// disconnects takes its objects with it, so the object removes itself from the surface's lists on the
// way out rather than leaving the surface holding freed resources it means to send an event to.
class ClientPresentationFeedback final : public Wayland::Server::WpPresentationFeedbackHandler
{
public:
	// Null only where the client named something that was not a `wl_surface`, which the caller has
	// already ended it for — and where the object then answers `discarded` rather than nothing, because
	// a client holding a feedback that will never fire is one waiting on a frame forever.
	ClientPresentationFeedback(HostContext& context, ClientSurface* surface) noexcept;

	~ClientPresentationFeedback() override;

	void OnGone() override { delete this; }

	// The surface this feedback was asked on, so the surface can tell whose `wl_output` resource to
	// name in `sync_output` — a client may have bound the global more than once, and the event carries
	// an object rather than an output.
	[[nodiscard]] ClientSurface* Surface() const noexcept { return m_Surface; }

	// The surface went out from under this object. Nothing is owed after that: the content update is
	// discarded, which is what the protocol says happens to one whose surface was destroyed.
	void ForgetSurface() noexcept { m_Surface = nullptr; }

private:
	HostContext* m_Context = nullptr;

	// The surface this feedback is staged on. Not owned, and null once that surface has gone.
	ClientSurface* m_Surface = nullptr;
};

// One client's `wp_presentation`. No per-client state beyond the clock it is told about at bind, which
// is why the handler is per resource exactly as `wl_compositor`'s is.
class ClientPresentation final : public Wayland::Server::WpPresentationHandler
{
public:
	explicit ClientPresentation(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// **The contract begins with an event**, the way `wl_output`'s does: a client binds and is told the
	// clock domain before it has sent anything, and the protocol says that domain does not change for
	// the life of the connection.
	void OnBound() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. Destroying
	// the factory does not touch the feedbacks it made, which the protocol states outright.
	void OnDestroy() override {}

	Wayland::Server::WpPresentationFeedbackHandler* OnFeedback(Wayland::Server::WlSurface surface) override;

private:
	HostContext* m_Context = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class PresentationGlobal final : public Wayland::Server::WpPresentationBinding
{
public:
	explicit PresentationGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::WpPresentationHandler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
