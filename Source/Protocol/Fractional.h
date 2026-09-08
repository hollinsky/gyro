#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "Geometry/Scale.h"
#include "Protocol/Context.h"
#include "Scene/Output.h"
#include "Wayland/Server/FractionalScaleV1.h"
#include "Wayland/Server/Wayland.h"

class ClientSurface;

// `wp_fractional_scale_v1`: what scale gyro would like this window drawn at.
//
// **This is the half of decision 56 that reaches the client, and without it a scaled panel is soft
// everywhere.** `wl_output.scale` is an integer, so a client on a 1.5x screen is told 2 and draws
// twice the pixels it needs; gyro then minifies, which decision 56 accepts as the fallback because
// minification degrades gracefully. What it does not do is *look right* — text laid out for a 2x grid
// and resampled to 1.5x has no stem landing on a whole pixel anywhere, and that is every glyph on the
// machine slightly wrong rather than a few edges. Telling the client 180/120 instead lets it lay out
// on the grid the panel actually has.
//
// **It rests on `wp_viewporter` and is inert without it** (171). A client told to render at 1.5x has
// no way to say what the resulting buffer *means* except `wp_viewport.set_destination` —
// `wl_surface.set_buffer_scale` is an integer and always was — so the two are one feature seen from
// two ends, and the viewporter had to land first.
//
// **Advertised at version 1, and there is no other.**
//
// **The value is the maximum over the outputs the surface is on**, which is `Scene/Reach.h`'s mask
// folded through `Scene/Output.h`'s density — the same mask `wl_surface.enter` is sent from, computed
// once per surface per iteration and read twice. Maximum rather than largest-overlap for decision
// 56's reason: largest-overlap flips at the halfway line of a drag, which is the worst moment
// available, and it makes the leading edge of a window entering a denser screen the part that looks
// worst.
//
// **Changes are symmetric today and decision 56 asks for them not to be.** The promise is that a
// scale is raised the instant a window touches a denser output and lowered only after the
// manipulation settles, so that dragging a window along a monitor boundary is not a buffer
// reallocation storm inside the client. That hysteresis is not here, and the reason it is not is that
// it does not belong here: `wl_surface.enter`/`leave` has the same flap and the same cost — a toolkit
// reallocates off either — so a debounce on this alone would leave gyro with two answers to *has this
// window left that screen*. The place for it is the reach comparison in [Output.h](Output.h), and the
// interval it needs is one of the constants Docs/Open.md is still holding.
inline constexpr std::uint32_t FractionalScaleVersion = 1;

// What to ask a surface with this reach to draw at.
//
// **An empty reach answers with the densest output on the machine rather than with 1x**, and that is
// the same cheap-side trade `Scene/Reach.h` takes for a quad it cannot project. A client creates this
// object before its first commit, when its surface is on no screen yet, and whatever it hears is what
// it sizes its first buffer at: too high costs one buffer drawn larger than it needed to be, and too
// low is the first frame of every window on a HiDPI panel arriving soft and then popping sharp. That
// asymmetry is the one decision 56 already rests on, applied to the moment before there is anything
// to be asymmetric about.
//
// Identity where there are no outputs at all, which is a machine with nothing plugged in and a client
// that will be told again the moment there is.
[[nodiscard]] Scale PreferredScale(OutputReach reach, std::span<const SceneOutput> outputs) noexcept;

// One `wp_fractional_scale_v1`: the scale one `wl_surface` is being asked for.
//
// **It holds no client-writable state, because the protocol gives it no request but `destroy`.** What
// it holds is the last value sent, which is what makes this an event per *change* rather than an event
// per iteration — the fold above runs on every wakeup and a client that has not moved hears nothing.
//
// **A surface destroyed out from under it leaves it inert rather than erroring.** `wp_viewport` owes
// `no_surface` because it has requests that would have to act on one; this has none, so there is
// nothing left to refuse and the client is holding an id that will simply never speak again.
class ClientFractionalScale final : public Wayland::Server::WpFractionalScaleV1Handler
{
public:
	// Null only where the client named something that was not a `wl_surface`, which the caller has
	// already ended it for — the object still has to exist because the client is holding an id.
	ClientFractionalScale(HostContext& context, ClientSurface* surface) noexcept;

	~ClientFractionalScale() override;

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. The protocol
	// says `preferred_scale` stops at the destroy, which it does by there being nothing to send it on.
	void OnDestroy() override {}

	// Tell the client this scale, if it is not already the one it was told.
	//
	// **The comparison is the whole point of the stored value.** `SyncOutputEntry` runs on every
	// dispatch wakeup, which is input rate while somebody is dragging a window, and an event per wakeup
	// would be a toolkit asked to reconsider its buffer size a few hundred times a second.
	void Send(Scale scale);

	// The first `preferred_scale`, sent once the resource behind this object exists. See the definition
	// for why it cannot be sent from the request that created the object.
	void OnBound() override;

	// The `wl_surface` went out from under this object. Nothing is unstaged — this object never wrote
	// any surface state — and nothing further is ever sent.
	void ForgetSurface() noexcept { m_Surface = nullptr; }

	// What the client was last told, or nothing where it has been told nothing yet. For the tests,
	// which is the only place that can see it: on the wire it is an event the client already has.
	[[nodiscard]] std::optional<Scale> Sent() const noexcept { return m_Sent; }

private:
	HostContext* m_Context = nullptr;

	// The surface this object is about. Not owned, and null once that surface has gone.
	ClientSurface* m_Surface = nullptr;

	// The last value sent, or nothing before the first. **The `optional` is load-bearing rather than a
	// null scale**: `Scale{}` is identity and identity is a perfectly ordinary preferred scale, so a
	// sentinel value would silently withhold the first event from every client on an unscaled panel.
	std::optional<Scale> m_Sent;
};

// One client's `wp_fractional_scale_manager_v1`. No per-client state, exactly as `wl_compositor` has
// none; a handler per bind because the bindings pair a handler with one resource.
class ClientFractionalScaleManager final : public Wayland::Server::WpFractionalScaleManagerV1Handler
{
public:
	explicit ClientFractionalScaleManager(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. Destroying
	// the manager does not touch the objects it made, which the protocol states outright.
	void OnDestroy() override {}

	Wayland::Server::WpFractionalScaleV1Handler* OnGetFractionalScale(Wayland::Server::WlSurface surface) override;

private:
	HostContext* m_Context = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class FractionalScaleGlobal final : public Wayland::Server::WpFractionalScaleManagerV1Binding
{
public:
	explicit FractionalScaleGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::WpFractionalScaleManagerV1Handler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
