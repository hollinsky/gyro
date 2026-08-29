#pragma once

#include <cstdint>

#include "Geometry/Shape.h"
#include "Geometry/Space.h"
#include "Wayland/Server/Wayland.h"

// A `wl_region`: the object a client builds a shape in.
//
// **The shape itself is `Geometry/Shape.h`'s and not this file's.** What a client is building is a
// sequence of integer rectangles and one predicate over them, and the party that asks that predicate
// a question is `Scene/Hit.h` — which may not name `Protocol`, since the dependency runs the other
// way. So the value lives below both of them and this file holds only the wire: the two requests, the
// clamp that a negative extent gets, and the cap that ends a client rather than allocating for it.

// The `wl_region` object: a `SurfaceShape` a client is still building.
//
// It deletes itself in `OnGone` because its lifetime is the client's object and nothing else refers
// to it — a surface given this region took a copy, per `SurfaceShape` above.
class ClientRegion final : public Wayland::Server::WlRegionHandler
{
public:
	ClientRegion() = default;

	[[nodiscard]] const SurfaceShape& Shape() const noexcept { return m_Shape; }

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs and `OnGone` follows immediately, so there is
	// nothing for the destructor request itself to do.
	void OnDestroy() override {}

	void OnAdd(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

	void OnSubtract(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

private:
	// A rectangle as the wire spells one: an origin and an extent, both surface-local and integral,
	// with a negative extent clamped away rather than carried. The protocol does not forbid a client
	// sending one, and a rect whose right edge is left of its left edge would make `Contains` answer
	// for an inverted interval.
	[[nodiscard]] static PixelRect<SurfaceSpace>
	FromWire(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) noexcept;

	// True once the client has been ended for overrunning `MaxShapeRects`, so the second request
	// after that does not post a second error onto a connection already going away.
	bool m_Overrun = false;

	SurfaceShape m_Shape;
};
