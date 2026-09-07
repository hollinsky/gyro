#pragma once

#include <cstdint>

#include "Protocol/Context.h"
#include "Wayland/Server/Wayland.h"

// The `wl_compositor` global: where a client's surfaces and regions come from, and gyro's first
// advertised global.
//
// **Advertised at version 6, and the number is a promise rather than a ceiling.** Version 4 is
// `wl_surface.damage_buffer` and version 5 is `wl_surface.offset`, both of which are requests gyro
// answers. Version 6 adds `preferred_buffer_scale` and `preferred_buffer_transform`, which are
// *events* — the first is what this version costs and now sends, and the second is deliberately never
// sent. Version 7 adds `wl_surface.get_release`, whose contract is a per-commit release fence and
// belongs with the buffer path.
//
// Naming a version gyro does not implement is not a lint failure, it is a client rendering at the
// wrong scale on a HiDPI panel and never being told. So the number goes up when the thing it names is
// built, and `ClientSurface::OnGetRelease` records a fault if this is ever raised without it.
//
// **`preferred_buffer_transform` is not sent, and the protocol's default is why that is honest rather
// than owed.** A surface's preferred transform is normal until told otherwise, and normal is genuinely
// what gyro prefers: `wl_surface.set_buffer_transform` is parsed and staged and nothing downstream
// reads it, so a client that took a hint to pre-rotate for a monitor stood on end would hand gyro a
// buffer gyro then drew unturned — a sideways window, which is worse than the redundant rotation the
// hint would have saved. It becomes sendable when the world carries a buffer transform.
//
// **One handler per bind rather than one shared object**, which the bindings require: a handler is
// bound to exactly one resource and refuses a second, so a global that handed the same implementation
// to two clients would serve the first and fault on the second. There is no per-client state in a
// compositor object to make that a cost.
inline constexpr std::uint32_t CompositorVersion = 6;

// The version at which a `wl_surface` is owed `preferred_buffer_scale`.
//
// **A client may bind `wl_compositor` below what is advertised, and a surface is capped at the version
// of the object that made it** — libwayland's rule, not a courtesy — so an old toolkit's surfaces are
// still version 5 on a compositor serving 6. Named here rather than spelled `6` at the one comparison
// in [Surface.cpp](Surface.cpp), because the number that matters there is this interface's and not
// that file's.
inline constexpr std::uint32_t PreferredBufferScaleVersion = 6;

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
