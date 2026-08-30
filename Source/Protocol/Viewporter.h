#pragma once

#include <cstdint>

#include "Protocol/Context.h"
#include "Protocol/Surface.h"
#include "Wayland/Server/Viewporter.h"
#include "Wayland/Server/Wayland.h"

// `wp_viewporter`: how a client says how big its surface is, independently of the buffer it drew.
//
// **This is the global whose absence made Firefox twice the size it should be**, and the shape of
// that is worth stating because it is not the obvious one. A toolkit on a 2x output renders at 2x and
// says so with `wl_surface.set_buffer_scale`; Firefox does that for the GTK shell surface around its
// window and *not* for the subsurface its content is actually in — that one gets a 2560x1944 buffer
// and no scale at all, because the size it means is carried by `wp_viewport.set_destination` instead.
// A compositor with no viewporter has nothing to read it from, so the buffer lands at its own pixel
// count in surface coordinates and the window covers four times the area a person asked for. There is
// no fallback to fall back to: Firefox never sends a scale on that surface at all.
//
// **And it is the protocol the fractional scale rests on**, which is the other reason to have it
// first. `wp_fractional_scale_v1` tells a client to render at 1.7x; the only way the client then
// states what that buffer *means* is a viewport destination, because `wl_surface.set_buffer_scale` is
// an integer and always was. So the two land in that order or the second one lands inert.
//
// **Advertised at version 1, and there is no other.** `wp_viewporter` has never been revised.
//
// **The destination is a resize of the node and not a second field beside its extent.** A surface's
// size is one number in the world — `Scene` gets `Resize` and the frame walk reads a quad — and the
// alternative was to publish the buffer's own extent and carry the destination as a scale factor down
// beside it. That would have put a second multiply in the frame thread's projection for a fact that
// is constant between commits, and it would have made *how big is this window* a question with two
// answers depending on who asked. The source rectangle is genuinely separate and stays separate: it
// is which texels are sampled rather than how much room they take, and `World/Content.h` has held a
// real-valued `Source` for it since before there was anything to fill it in.
inline constexpr std::uint32_t ViewporterVersion = 1;

// One `wp_viewport`: the crop and scale on one `wl_surface`.
//
// **It holds no state of its own.** Both requests stage into the surface's pending state, land at
// that surface's commit, and are removed by this object being destroyed — which is the protocol's own
// double buffering rather than a second one. What this object is, is the client's handle onto those
// two fields plus the four errors reaching them can raise.
class ClientViewport final : public Wayland::Server::WpViewportHandler
{
public:
	// Null only where the client named something that was not a `wl_surface`, which the caller has
	// already ended it for. The object still has to exist because the client is holding an id, and
	// every request on it then answers `no_surface` — see `ForgetSurface`.
	ClientViewport(HostContext& context, ClientSurface* surface) noexcept;

	~ClientViewport() override;

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. The surface's
	// crop and scale is dropped in the destructor rather than here, so that both ways this object goes
	// away — the client destroying it, and the client disconnecting — take the state with them.
	void OnDestroy() override {}

	void OnSetSource(wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) override;

	void OnSetDestination(std::int32_t width, std::int32_t height) override;

	// The `wl_surface` went out from under this object, which the protocol allows and turns every
	// remaining request into `no_surface`. There is nothing to unstage: the state lives on the surface
	// that has just been destroyed.
	void ForgetSurface() noexcept { m_Surface = nullptr; }

private:
	// Whether the surface is still there, having ended the client with `no_surface` if it is not. The
	// gate on both requests and the whole of what `ForgetSurface` is for.
	[[nodiscard]] bool HasSurface() const;

	HostContext* m_Context = nullptr;

	// The surface this viewport is on. Not owned, and null once that surface has gone.
	ClientSurface* m_Surface = nullptr;
};

// One client's `wp_viewporter`. No per-client state, exactly as `wl_compositor` has none; a handler
// per bind because the bindings pair a handler with one resource.
class ClientViewporter final : public Wayland::Server::WpViewporterHandler
{
public:
	explicit ClientViewporter(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. Destroying
	// the factory does not touch the viewports it made, which the protocol states outright.
	void OnDestroy() override {}

	Wayland::Server::WpViewportHandler* OnGetViewport(Wayland::Server::WlSurface surface) override;

private:
	HostContext* m_Context = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class ViewporterGlobal final : public Wayland::Server::WpViewporterBinding
{
public:
	explicit ViewporterGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::WpViewporterHandler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
