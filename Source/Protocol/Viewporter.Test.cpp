#include "Protocol/Viewporter.h"

#include <optional>

#include <wayland-server-core.h>

#include "Geometry/Space.h"
#include "Protocol/Context.h"
#include "Protocol/Surface.h"
#include "Testing/Test.h"

// The crop and scale: what it derives, and what a request does with it.
//
// **A viewport here has no `wl_resource` behind it**, which is `Surface.Test.cpp`'s arrangement and
// buys the same thing: what a request did is read straight off the surface's committed state, which
// is the one place a client can never look. What it costs is the same too — `PostError` on an absent
// resource does nothing, so a refused request is asserted by what did not change.
//
// **The two errors that need a buffer are not here at all**, and that is the same boundary rather
// than a gap: `out_of_buffer` and `bad_size` are asked of the state at the commit that carries a
// buffer, and attaching one needs a `wl_buffer` and a texture space.
// Integration/ProtocolRoundTrip.Test.cpp is where a real client attaches real pixels, and it is where
// the error a client actually receives is checked.

namespace
{
[[nodiscard]] wl_fixed_t Fixed(double value) noexcept
{
	return wl_fixed_from_double(value);
}

// A committed state with pixels of a stated size. An aggregate rather than a `ClientSurface`, because
// what the three cases below are about is the derivation and not the staging — and the two are worth
// keeping apart, since a surface cannot be given content without a buffer resource behind it.
[[nodiscard]] SurfaceState Buffered(std::int32_t width, std::int32_t height, std::int32_t scale)
{
	SurfaceState state;

	state.BufferScale = scale;
	state.ContentSize = PixelSize<BufferSpace>{ width, height };

	return state;
}
} // namespace

GYRO_TEST(Viewporter, NothingSetLeavesTheBufferAndTheScale)
{
	const SurfaceState state = Buffered(2560, 1944, 2);

	GYRO_CHECK_EQ(state.Extent(), (Size<SurfaceSpace, float>{ 1280.0F, 972.0F }));
	GYRO_CHECK_EQ(state.Texels(), (Rect<BufferSpace>{ {}, { 2560.0F, 1944.0F } }));
}

GYRO_TEST(Viewporter, ADestinationIsTheSurfaceSize)
{
	// Firefox's own numbers: a 2560x1944 buffer with no scale on it at all, and a destination that says
	// what those pixels mean. Without the destination this surface is 2560 wide, which on a 2x panel is
	// a window covering four times the area a person asked for.
	SurfaceState state = Buffered(2560, 1944, 1);
	state.Viewport.Destination = PixelSize<SurfaceSpace>{ 1280, 972 };

	GYRO_CHECK_EQ(state.Extent(), (Size<SurfaceSpace, float>{ 1280.0F, 972.0F }));

	// And the texels are still the whole buffer, because a destination scales rather than crops.
	GYRO_CHECK_EQ(state.Texels(), (Rect<BufferSpace>{ {}, { 2560.0F, 1944.0F } }));
}

GYRO_TEST(Viewporter, ASourceAloneCropsWithoutScaling)
{
	// Surface-local, so on a 2x buffer this names 200 texels across starting 100 texels in — which is
	// the whole of what the scale is doing in `Texels`.
	SurfaceState state = Buffered(800, 600, 2);
	state.Viewport.Source = Rect<SurfaceSpace>{ { 50.0F, 25.0F }, { 100.0F, 80.0F } };

	GYRO_CHECK_EQ(state.Extent(), (Size<SurfaceSpace, float>{ 100.0F, 80.0F }));
	GYRO_CHECK_EQ(state.Texels(), (Rect<BufferSpace>{ { 100.0F, 50.0F }, { 200.0F, 160.0F } }));
}

GYRO_TEST(Viewporter, ADestinationOverridesTheSourcesSize)
{
	// The pair a video player sends: sample a rectangle of the decoded frame, and put it in a box whose
	// size has nothing to do with how many texels that is.
	SurfaceState state = Buffered(400, 400, 1);
	state.Viewport.Source = Rect<SurfaceSpace>{ {}, { 320.0F, 180.0F } };
	state.Viewport.Destination = PixelSize<SurfaceSpace>{ 1600, 900 };

	GYRO_CHECK_EQ(state.Extent(), (Size<SurfaceSpace, float>{ 1600.0F, 900.0F }));
	GYRO_CHECK_EQ(state.Texels(), (Rect<BufferSpace>{ {}, { 320.0F, 180.0F } }));
}

GYRO_TEST(Viewporter, TheCropAndScaleIsDoubleBuffered)
{
	HostContext context;
	ClientSurface surface{ context };
	ClientViewport viewport{ context, &surface };

	viewport.OnSetDestination(500, 400);

	// Nothing the world can see has changed yet, which is the whole of why the state is on the surface:
	// a toolkit resizing states a new buffer and a new destination and they land in one frame.
	GYRO_CHECK(!surface.Current().Viewport.Destination.has_value());

	surface.OnCommit();

	GYRO_CHECK_EQ(surface.Current().Extent(), (Size<SurfaceSpace, float>{ 500.0F, 400.0F }));
}

GYRO_TEST(Viewporter, ASourceStagesInSurfaceCoordinates)
{
	HostContext context;
	ClientSurface surface{ context };
	ClientViewport viewport{ context, &surface };

	viewport.OnSetSource(Fixed(12.5), Fixed(4.25), Fixed(100.0), Fixed(80.0));
	surface.OnCommit();

	GYRO_REQUIRE(surface.Current().Viewport.Source.has_value());
	GYRO_CHECK_EQ(*surface.Current().Viewport.Source, (Rect<SurfaceSpace>{ { 12.5F, 4.25F }, { 100.0F, 80.0F } }));
}

GYRO_TEST(Viewporter, UnsettingIsTheOneArrangementOfNegativesThatIsLegal)
{
	HostContext context;
	ClientSurface surface{ context };
	ClientViewport viewport{ context, &surface };

	viewport.OnSetSource(Fixed(0.0), Fixed(0.0), Fixed(320.0), Fixed(240.0));
	viewport.OnSetDestination(100, 100);
	surface.OnCommit();

	GYRO_REQUIRE(surface.Current().Viewport.Source.has_value());
	GYRO_REQUIRE(surface.Current().Viewport.Destination.has_value());

	viewport.OnSetSource(Fixed(-1.0), Fixed(-1.0), Fixed(-1.0), Fixed(-1.0));
	viewport.OnSetDestination(-1, -1);
	surface.OnCommit();

	GYRO_CHECK(!surface.Current().Viewport.Source.has_value());
	GYRO_CHECK(!surface.Current().Viewport.Destination.has_value());
}

GYRO_TEST(Viewporter, DestroyingTheViewportRemovesTheCropAtTheNextCommit)
{
	HostContext context;
	ClientSurface surface{ context };

	{
		ClientViewport viewport{ context, &surface };

		GYRO_REQUIRE(surface.AdoptViewport(viewport));

		viewport.OnSetDestination(200, 200);
		surface.OnCommit();

		GYRO_REQUIRE(surface.Current().Viewport.Destination.has_value());
	}

	// **Still set, because the protocol says the removal lands on the next commit.** A window that
	// snapped back to its buffer size the instant an object was destroyed would do it in the middle of
	// whatever frame the client was in.
	GYRO_CHECK(surface.Current().Viewport.Destination.has_value());

	surface.OnCommit();

	GYRO_CHECK(!surface.Current().Viewport.Destination.has_value());
}

GYRO_TEST(Viewporter, ARefusedSourceChangesNothing)
{
	HostContext context;
	ClientSurface surface{ context };
	ClientViewport viewport{ context, &surface };

	viewport.OnSetSource(Fixed(0.0), Fixed(0.0), Fixed(320.0), Fixed(240.0));
	surface.OnCommit();

	// A negative origin and an empty extent are both `bad_value`, and neither may leave the surface
	// holding half of what was asked for. Refused at the request rather than the commit because neither
	// answer depends on the buffer.
	viewport.OnSetSource(Fixed(-5.0), Fixed(0.0), Fixed(100.0), Fixed(100.0));
	viewport.OnSetSource(Fixed(0.0), Fixed(0.0), Fixed(0.0), Fixed(100.0));
	surface.OnCommit();

	GYRO_REQUIRE(surface.Current().Viewport.Source.has_value());
	GYRO_CHECK_EQ(surface.Current().Viewport.Source->Extent, (Size<SurfaceSpace, float>{ 320.0F, 240.0F }));
}

GYRO_TEST(Viewporter, ARefusedDestinationChangesNothing)
{
	HostContext context;
	ClientSurface surface{ context };
	ClientViewport viewport{ context, &surface };

	viewport.OnSetDestination(320, 240);
	surface.OnCommit();

	viewport.OnSetDestination(0, 100);
	viewport.OnSetDestination(100, -4);
	surface.OnCommit();

	GYRO_REQUIRE(surface.Current().Viewport.Destination.has_value());
	GYRO_CHECK_EQ(*surface.Current().Viewport.Destination, (PixelSize<SurfaceSpace>{ 320, 240 }));
}

GYRO_TEST(Viewporter, ASurfaceTakesOneViewport)
{
	HostContext context;
	ClientSurface surface{ context };
	ClientViewport first{ context, &surface };
	ClientViewport second{ context, &surface };

	GYRO_CHECK(surface.AdoptViewport(first));
	GYRO_CHECK(!surface.AdoptViewport(second));

	// And the first one keeps it: the object the client asked for second is refused rather than
	// becoming a second writer of one surface's crop and scale.
	surface.ForgetViewport(second);

	GYRO_CHECK(!surface.AdoptViewport(second));
}

GYRO_TEST(Viewporter, AViewportOutlivesItsSurfaceAndWritesNothing)
{
	HostContext context;
	ClientViewport viewport{ context, nullptr };

	// The client is holding an id and every request on it owes `no_surface`. There is nothing to assert
	// but that it does not reach through the null, which is the whole point: a client destroying a
	// `wl_surface` and then its viewport, in that order, is legal and is what a toolkit tearing a window
	// down actually does.
	viewport.OnSetDestination(100, 100);
	viewport.OnSetSource(Fixed(0.0), Fixed(0.0), Fixed(10.0), Fixed(10.0));
}
