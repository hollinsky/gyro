#pragma once

#include <cstdint>

#include "Protocol/Context.h"
#include "Wayland/Server/Wayland.h"

// The `wl_compositor` global: where a client's surfaces and regions come from, and gyro's first
// advertised global.
//
// **Advertised at version 5, and the number is a promise rather than a ceiling.** Version 4 is
// `wl_surface.damage_buffer` and version 5 is `wl_surface.offset`, both of which are requests gyro
// answers. Version 6 adds `preferred_buffer_scale` and `preferred_buffer_transform`, which are
// *events* — advertising 6 obliges gyro to tell each surface which output's scale to draw for, and
// there is no output model reaching this module yet. Version 7 adds `wl_surface.get_release`, whose
// contract is a per-commit release fence and belongs with the buffer path.
//
// Naming a version gyro does not implement is not a lint failure, it is a client rendering at the
// wrong scale on a HiDPI panel and never being told. So the number goes up when the thing it names is
// built, and `ClientSurface::OnGetRelease` records a fault if this is ever raised without it.
//
// **One handler per bind rather than one shared object**, which the bindings require: a handler is
// bound to exactly one resource and refuses a second, so a global that handed the same implementation
// to two clients would serve the first and fault on the second. There is no per-client state in a
// compositor object to make that a cost.
inline constexpr std::uint32_t CompositorVersion = 5;

// One client's `wl_compositor`.
class ClientCompositor final : public Wayland::Server::WlCompositorHandler
{
public:
	explicit ClientCompositor(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnRelease() override {}

	Wayland::Server::WlSurfaceHandler* OnCreateSurface() override;

	Wayland::Server::WlRegionHandler* OnCreateRegion() override;

private:
	// Handed to every surface this client makes, because a `wl_surface.commit` is a change to the world
	// and the world arrives as an argument to `Advance`. Context.h carries that argument.
	HostContext* m_Context = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class CompositorGlobal final : public Wayland::Server::WlCompositorBinding
{
public:
	explicit CompositorGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::WlCompositorHandler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
