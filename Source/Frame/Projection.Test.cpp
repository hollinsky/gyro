#include "Frame/Projection.h"

#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <source_location>
#include <string_view>

#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "Seam/Renderer.h"
#include "Testing/Test.h"

// What a node looks like once it has been placed on an output, which is the last thing that happens
// to it before a renderer sees it.
//
// The assertions here are the four ways this can be wrong on screen rather than wrong in arithmetic.
// A quad wound the other way is Seam/Renderer.h's diagonal tear. A corner narrowed to single
// precision before the output's origin came off it is a window that cannot be placed within a
// wl_fixed of where it was asked for, on the far monitor of a wide desk. A back-face test that reads
// one node at a time passes every still window and fails the moment two rotations compose, which is
// the card flip inside a tilted deck. And a mirrored output whose front faces all read as back faces
// is a screen with nothing on it.

constexpr float Pi = std::numbers::pi_v<float>;

// A quarter of a wl_fixed unit, which is the resolution the wire delivers positions in — so anything
// admitted here is finer than a client could have asked for. It is far looser than the double path
// actually achieves and far tighter than the single-precision path could reach, which is what makes
// it a test of the fold rather than of rounding.
constexpr float DeviceTolerance = 1.0F / 1024.0F;

[[nodiscard]] static float Radians(float degrees)
{
	return degrees * (Pi / 180.0F);
}

// The harness prints the expression and nothing else, which for an approximate comparison leaves out
// the values and the tolerance they missed by. Same shape as Geometry/NodeTransform.Test.cpp's, and
// for the same reason.
static bool CheckNear(
	Point<DeviceSpace> actual,
	Point<DeviceSpace> expected,
	float tolerance,
	std::string_view what,
	const std::source_location& where = std::source_location::current()
)
{
	if (std::abs(actual.X - expected.X) <= tolerance && std::abs(actual.Y - expected.Y) <= tolerance)
	{
		return true;
	}

	ReportFailure(what, std::format("{} vs {} (tolerance {})", actual, expected, tolerance), where);
	return false;
}

// An output showing the world one-to-one from the global origin, which is the arrangement every test
// below varies from rather than the one it happens to have.
[[nodiscard]] static OutputView Screen()
{
	return OutputView{ AxisTransform<GlobalSpace, DeviceSpace>::Identity(), { 1920, 1080 } };
}

// One node under a view, which is what the walk does on its first push.
[[nodiscard]] static ComposedTransform
Under(const OutputView& view, const NodeTransform& node, Size<SurfaceSpace> extent)
{
	return view.Root().Push(node, node.BoundingRadius(extent.Width, extent.Height));
}

GYRO_TEST(Projection, TheWindingIsTheOneARendererAssumes)
{
	const OutputView view = Screen();
	const Size<SurfaceSpace> extent{ 100.0F, 50.0F };
	const std::optional<Quad> quad = view.Project(Under(view, NodeTransform{}, extent), extent);

	GYRO_REQUIRE(quad.has_value());

	// Top-left, top-right, bottom-right, bottom-left of [0, Width] x [0, Height], Y-down. The other
	// order is a diagonal tear rather than an error, so it is asserted positionally and not as a set.
	GYRO_CHECK_EQ(quad->Corners[0], Point<DeviceSpace>{ 0.0F, 0.0F });
	GYRO_CHECK_EQ(quad->Corners[1], Point<DeviceSpace>{ 100.0F, 0.0F });
	GYRO_CHECK_EQ(quad->Corners[2], Point<DeviceSpace>{ 100.0F, 50.0F });
	GYRO_CHECK_EQ(quad->Corners[3], Point<DeviceSpace>{ 0.0F, 50.0F });

	// Seam/Renderer.h's DrawItem states the same thing from the other side: local (0, 0) is corner
	// zero and local (Width, Height) is corner two, which is what turns four points back into a
	// parameterization a texture can be sampled through.
	GYRO_CHECK_EQ(quad->Bounds(), Rect<DeviceSpace>::FromEdges({ 0.0F, 0.0F }, { 100.0F, 50.0F }));

	// An orthographic chain divides by one everywhere, and it does so exactly rather than nearly, so
	// a still window takes the same path through a perspective-correct interpolation as a moving one.
	for (const float weight : quad->Weights)
	{
		GYRO_CHECK_EQ(weight, 1.0F);
	}
}

GYRO_TEST(Projection, TheOutputOriginComesOffBeforeAnythingNarrows)
{
	// Space.h's own number: single precision resolves 1/256 of a logical pixel around 32768, which is
	// exactly wl_fixed's resolution and leaves nothing underneath it. A window three 1024ths of a
	// pixel off the grid at that distance is a window whose subpixel placement survives only if the
	// output's origin is subtracted while both sides are still double.
	constexpr double Far = 32768.0;
	constexpr double Offset = 3.0 / 1024.0;

	AxisTransform<GlobalSpace, DeviceSpace> placement;
	placement.Translation = { -Far, 0.0 };

	const OutputView view{ placement, { 1920, 1080 } };
	const Size<SurfaceSpace> extent{ 100.0F, 50.0F };
	const NodeTransform node{ .Translation = { Far + Offset, Offset, 0.0 } };
	const std::optional<Quad> quad = view.Project(Under(view, node, extent), extent);

	GYRO_REQUIRE(quad.has_value());

	// The route this is a test against, spelled out because it is the one somebody writes: taking the
	// corner to global space and converting it there gives 32768.00390625, because that is the
	// nearest float, and the offset comes out a 1024th of a pixel wrong in the other direction.
	CheckNear(
		quad->Corners[0],
		{ static_cast<float>(Offset), static_cast<float>(Offset) },
		DeviceTolerance,
		"the subpixel offset survives the fold"
	);

	// And the extent is undisturbed by the distance, which is the second half of the same statement:
	// a window at the far edge of the desk is not a wider window.
	CheckNear(
		quad->Corners[2],
		{ static_cast<float>(Offset) + 100.0F, static_cast<float>(Offset) + 50.0F },
		DeviceTolerance,
		"and so does the far corner"
	);
}

GYRO_TEST(Projection, TheBackFaceTestIsTheComposedOneAndNotThePerNodeOne)
{
	// Two levels each turned eighty degrees about Y. Every node here faces the viewer by its own
	// reckoning; the chain, at a hundred and sixty, is showing its back. This is the assertion
	// decision 86's original sentence would have failed, and the shape it fails on is ordinary — a
	// card mid-flip inside a deck that is itself tilted.
	//
	// Every node here is translated to the middle of the screen, so that a quad the face test should
	// have caught cannot be culled by the target test standing behind it — which is what a rotation
	// about a node's own origin at the top-left corner would otherwise arrange.
	const NodeTransform turned{ .Translation = { 500.0, 500.0, 0.0 },
		                        .Rotation = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(80.0F)) };

	GYRO_CHECK(turned.FacesViewer());

	const OutputView view = Screen();
	const Size<SurfaceSpace> extent{ 100.0F, 50.0F };
	const float radius = turned.BoundingRadius(extent.Width, extent.Height);
	const ComposedTransform once = view.Root().Push(turned, radius);
	const ComposedTransform twice = once.Push(turned, radius);

	GYRO_CHECK(view.Project(once, extent).has_value());
	GYRO_CHECK(!view.Project(twice, extent).has_value());

	// The single-node case still answers the way decision 55 says it must, so what changed is which
	// question is asked and not what the answer means.
	const NodeTransform mirrored{ .Translation = { 500.0, 500.0, 0.0 }, .Scale = { -1.0F, 1.0F, 1.0F } };

	GYRO_CHECK(!view.Project(Under(view, mirrored, extent), extent).has_value());

	// And a single node past the quarter turn goes on its own, which is why a card flip is two nodes
	// and a catalog transition rather than a double-sided quad. Exactly ninety degrees is deliberately
	// not asserted: it is a sliver whose side is decided by the last bit of a cosine, and
	// Geometry/NodeTransform.Test.cpp declines to have an answer there for the same reason.
	const NodeTransform past{ .Translation = { 500.0, 500.0, 0.0 },
		                      .Rotation = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(100.0F)) };

	GYRO_CHECK(!view.Project(Under(view, past, extent), extent).has_value());
}

GYRO_TEST(Projection, AMirroredOutputIsNotAScreenOfBackFaces)
{
	// A flipped output mirrors everything on it, text included, because that is what was configured.
	// Reading the signed area against a fixed sign would cull every window on such an output, which
	// is a blank screen rather than a visible bug.
	AxisTransform<GlobalSpace, DeviceSpace> placement{ .Orientation = AxisOrientation::Flipped };
	placement.Translation = { 200.0, 0.0 };

	const OutputView view{ placement, { 200, 100 } };
	const Size<SurfaceSpace> extent{ 100.0F, 50.0F };
	const std::optional<Quad> quad = view.Project(Under(view, NodeTransform{}, extent), extent);

	GYRO_REQUIRE(quad.has_value());
	GYRO_CHECK_EQ(quad->Corners[0], Point<DeviceSpace>{ 200.0F, 0.0F });
	GYRO_CHECK_EQ(quad->Corners[1], Point<DeviceSpace>{ 100.0F, 0.0F });

	// And the correction is about the view rather than about the sign: a node that mirrors itself is
	// still showing its back on a mirrored output, where the two mirrors would otherwise cancel into
	// a front face nobody asked for.
	const NodeTransform mirrored{ .Scale = { -1.0F, 1.0F, 1.0F }, .Anchor = { 50.0F, 25.0F, 0.0F } };

	GYRO_CHECK(!view.Project(Under(view, mirrored, extent), extent).has_value());
}

GYRO_TEST(Projection, ACornerBehindTheEyeTakesTheWholeNode)
{
	// A container with a perspective and a child leaning into it, which is the arrangement decision
	// 92 says is the ordinary one: with no camera, a container is the only thing that can give its
	// children a shared vanishing point, so the descendant case is the case.
	const Size<SurfaceSpace> containerExtent{ 100.0F, 100.0F };
	const NodeTransform container{ .Anchor = { 50.0F, 50.0F, 0.0F },
		                           .Projection = Perspective::FromRadii(Perspective::MinimumRadii) };

	const OutputView view = Screen();
	const ComposedTransform chain = Under(view, container, containerExtent);

	// Leaned far enough that its far edge passes through the container's eye. The near corner is in
	// front of it, so this is one corner behind and not four — decision 92 rejects near-plane
	// clipping, so the node goes rather than being trimmed to the part that could be drawn.
	const NodeTransform leaning{ .Rotation = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(60.0F)) };
	const Size<SurfaceSpace> through{ 400.0F, 100.0F };

	GYRO_CHECK(chain.Push(leaning, leaning.BoundingRadius(through.Width, through.Height)).Project({}).IsVisible());
	GYRO_CHECK(
		!view.Project(chain.Push(leaning, leaning.BoundingRadius(through.Width, through.Height)), through).has_value()
	);

	// The same lean, short enough to stay in front of the eye, is drawn — and its weights are not all
	// one, which is what a renderer divides by to keep the texture from swimming across it.
	const Size<SurfaceSpace> clear{ 100.0F, 100.0F };
	const std::optional<Quad> quad =
		view.Project(chain.Push(leaning, leaning.BoundingRadius(clear.Width, clear.Height)), clear);

	GYRO_REQUIRE(quad.has_value());
	GYRO_CHECK(quad->Weights[0] != quad->Weights[1]);

	for (const float weight : quad->Weights)
	{
		GYRO_CHECK(weight >= Projected::MinimumWeight);
	}
}

GYRO_TEST(Projection, WhatMissesTheTargetIsGone)
{
	const OutputView view = Screen();
	const Size<SurfaceSpace> extent{ 100.0F, 50.0F };

	// Wholly past the right edge, which is the workspace nobody is looking at and the reason a scene
	// with one visible out of nine costs what it does.
	const NodeTransform past{ .Translation = { 1920.0, 0.0, 0.0 } };

	GYRO_CHECK(!view.Project(Under(view, past, extent), extent).has_value());

	// Straddling it is drawn whole, because clipping to the target is a scissor the renderer sets
	// once from the damage region and never a per-item intersection.
	const NodeTransform straddling{ .Translation = { 1900.0, 0.0, 0.0 } };
	const std::optional<Quad> quad = view.Project(Under(view, straddling, extent), extent);

	GYRO_REQUIRE(quad.has_value());
	GYRO_CHECK_EQ(quad->Corners[1], Point<DeviceSpace>{ 2000.0F, 0.0F });

	// Off the top by its own height, and off to the left by its own width, are the two the sign of
	// the comparison gets wrong separately.
	const NodeTransform above{ .Translation = { 0.0, -50.0, 0.0 } };
	const NodeTransform left{ .Translation = { -100.0, 0.0, 0.0 } };

	GYRO_CHECK(!view.Project(Under(view, above, extent), extent).has_value());
	GYRO_CHECK(!view.Project(Under(view, left, extent), extent).has_value());
}

GYRO_TEST(Projection, AnOutputWithNoModeDrawsNothing)
{
	// The default view, which is what a walk against an output that has not been configured would
	// hold. It culls rather than placing windows on a target with no pixels in it.
	const OutputView view;
	const Size<SurfaceSpace> extent{ 100.0F, 50.0F };

	GYRO_CHECK(!view.Project(Under(view, NodeTransform{}, extent), extent).has_value());

	// A node with no extent has no area to be a side of, and it goes by the same test rather than by
	// a case written for it.
	const OutputView screen = Screen();

	GYRO_CHECK(!screen.Project(Under(screen, NodeTransform{}, Size<SurfaceSpace>{}), Size<SurfaceSpace>{}).has_value());
}
