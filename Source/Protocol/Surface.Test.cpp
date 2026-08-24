#include "Protocol/Surface.h"

#include "Geometry/Space.h"
#include "Testing/Test.h"

// The double buffering, and the two ways it is asymmetric.
//
// **A surface here has no `wl_resource` behind it**, and that is what these tests are for rather than
// a limitation of them: every case below is about what the handler does with a request, and a request
// arriving over a real socket is Compositor.Test.cpp's subject. A resourceless handler makes the
// staging observable directly — `Current()` before and after a commit — which is exactly the thing a
// client cannot see, because the protocol's whole point is that it only ever sees the committed side.
//
// What that costs is the paths that end the client: `PostError` on an absent resource does nothing, so
// a rejected request is asserted here by *what did not change* and over the wire by the error the
// client receives. Both halves are worth having; a state machine that quietly accepted a scale of zero
// and a compositor that failed to say so are different bugs.
//
// The one path with no coverage on either side is a region *reaching* a surface, because the shape
// lives in a `wl_region` resource and a client cannot read back what it set. It arrives with the input
// routing that asks the question.

GYRO_TEST(Surface, NothingIsVisibleUntilCommit)
{
	ClientSurface surface;

	surface.OnSetBufferScale(2);
	surface.OnOffset(4, 5);

	GYRO_CHECK_EQ(surface.Current().BufferScale, 1);
	GYRO_CHECK_EQ(surface.Current().Offset, (PixelOffset<SurfaceSpace>{ 0, 0 }));

	surface.OnCommit();

	GYRO_CHECK_EQ(surface.Current().BufferScale, 2);
	GYRO_CHECK_EQ(surface.Current().Offset, (PixelOffset<SurfaceSpace>{ 4, 5 }));
}

GYRO_TEST(Surface, StateIsStickyAcrossCommits)
{
	ClientSurface surface;

	surface.OnSetBufferScale(3);
	surface.OnSetBufferTransform(Wayland::Server::WlOutputTransform::_90);
	surface.OnCommit();

	// A client sets a scale once and commits for the rest of its life. A pending state that reset
	// would put every window back to 1x on its second frame.
	surface.OnCommit();

	GYRO_CHECK_EQ(surface.Current().BufferScale, 3);
	GYRO_CHECK(surface.Current().BufferTransform == Wayland::Server::WlOutputTransform::_90);
}

GYRO_TEST(Surface, DamageIsConsumedByTheCommitThatCarriesIt)
{
	ClientSurface surface;

	surface.OnDamage(0, 0, 10, 10);
	surface.OnDamageBuffer(0, 0, 20, 20);
	surface.OnCommit();

	GYRO_CHECK_EQ(surface.Current().SurfaceDamage.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(surface.Current().BufferDamage.size(), std::size_t{ 1 });

	// The second commit carries no damage of its own, and the first commit's must not be repeated.
	// Carrying it forward is a region that has been correct for a thousand frames being repainted on
	// every one of them.
	surface.OnCommit();

	GYRO_CHECK(surface.Current().SurfaceDamage.empty());
	GYRO_CHECK(surface.Current().BufferDamage.empty());
}

GYRO_TEST(Surface, DamageIsKeptInTheSpaceItArrivedIn)
{
	ClientSurface surface;

	surface.OnDamage(1, 2, 3, 4);
	surface.OnDamageBuffer(5, 6, 7, 8);
	surface.OnCommit();

	// Converting between the two needs the buffer's scale and transform, which are not settled until
	// the commit that carries them — so the two lists stay separate until something has both.
	GYRO_REQUIRE(surface.Current().SurfaceDamage.size() == 1);
	GYRO_REQUIRE(surface.Current().BufferDamage.size() == 1);
	GYRO_CHECK_EQ(surface.Current().SurfaceDamage.front(), (PixelRect<SurfaceSpace>{ { 1, 2 }, { 3, 4 } }));
	GYRO_CHECK_EQ(surface.Current().BufferDamage.front(), (PixelRect<BufferSpace>{ { 5, 6 }, { 7, 8 } }));
}

GYRO_TEST(Surface, ANegativeExtentIsClampedRatherThanCarried)
{
	ClientSurface surface;

	surface.OnDamage(10, 10, -5, -5);
	surface.OnCommit();

	GYRO_REQUIRE(surface.Current().SurfaceDamage.size() == 1);

	// An inverted rectangle would answer every containment test backwards, and nothing in the protocol
	// forbids a client sending one.
	GYRO_CHECK_EQ(surface.Current().SurfaceDamage.front(), (PixelRect<SurfaceSpace>{ { 10, 10 }, { 0, 0 } }));
}

GYRO_TEST(Surface, AnInvalidScaleIsRejectedRatherThanStaged)
{
	ClientSurface surface;

	surface.OnSetBufferScale(2);
	surface.OnSetBufferScale(0);
	surface.OnSetBufferScale(-1);
	surface.OnCommit();

	// The client is being ended over the wire; what matters here is that the bad value never became
	// the surface's, because a scale of zero divides.
	GYRO_CHECK_EQ(surface.Current().BufferScale, 2);
}

GYRO_TEST(Surface, AnInvalidTransformIsRejectedRatherThanStaged)
{
	ClientSurface surface;

	surface.OnSetBufferTransform(Wayland::Server::WlOutputTransform::Flipped180);

	// libwayland checks an argument's type and not its range, so an enumerator the protocol never
	// defined arrives looking like one that exists.
	surface.OnSetBufferTransform(static_cast<Wayland::Server::WlOutputTransform>(99));
	surface.OnCommit();

	GYRO_CHECK(surface.Current().BufferTransform == Wayland::Server::WlOutputTransform::Flipped180);
}

GYRO_TEST(Surface, AnUnsetInputRegionIsInfiniteAndAnUnsetOpaqueRegionIsEmpty)
{
	ClientSurface surface;

	surface.OnCommit();

	// The two defaults differ, and this is the pair a compositor that shared their code gets wrong: a
	// surface accepts input everywhere until it says otherwise, and promises opacity nowhere until it
	// says otherwise.
	GYRO_CHECK(!surface.Current().Input.has_value());
	GYRO_CHECK(surface.Current().Opaque.IsUnset());
}

GYRO_TEST(Surface, ANullInputRegionRestoresTheInfiniteDefault)
{
	ClientSurface surface;

	surface.OnSetInputRegion({});
	surface.OnCommit();

	GYRO_CHECK(!surface.Current().Input.has_value());
}
