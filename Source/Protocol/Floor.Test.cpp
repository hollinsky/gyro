#include "Protocol/Floor.h"

#include <array>
#include <cstdint>

#include "Core/Clock.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"

// The floor and the placement, against the store rather than over a socket.
//
// What is worth asserting here is the arithmetic and nothing else: the wire half — that a real client
// gets as far as being placed — is Integration/ProtocolRoundTrip.Test.cpp's, and it cannot check where
// a window landed to within a pixel without reaching into the store anyway.

namespace
{
constexpr double PanelWidth = 1920.0;
constexpr double PanelHeight = 1080.0;

[[nodiscard]] SceneOutput Panel(double left = 0.0, double top = 0.0)
{
	return { .Bounds = { { left, top }, { PanelWidth, PanelHeight } },
		     .Density = Scale::FromInteger(1),
		     .Grid = { 1920, 1080 } };
}

[[nodiscard]] Size<SurfaceSpace, float> Window(float width, float height) noexcept
{
	return { width, height };
}
} // namespace

GYRO_TEST(Floor, TheFloorIsOneContainerAndOnlyEverOne)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SessionFloor floor;
	GYRO_REQUIRE(floor.Open(scene).has_value());
	GYRO_CHECK(!floor.Container().IsNull());
	GYRO_CHECK_EQ(scene.Count(), std::uint32_t{ 1 });

	// A second call is a wiring mistake rather than a state to serve, and the failure of letting it
	// through is two floors with windows split between them and no way to tell which is on screen.
	GYRO_CHECK(!floor.Open(scene).has_value());
	GYRO_CHECK_EQ(scene.Count(), std::uint32_t{ 1 });
}

GYRO_TEST(Floor, AWindowIsCentredOnTheOutputHoldingThePointer)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	SessionFloor floor;
	GYRO_REQUIRE(floor.Open(scene).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floor.Container(), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, *window, Window(800.0F, 600.0F));
	}

	const Entity* const placed = scene.Find(*window);
	GYRO_REQUIRE(placed != nullptr);

	// Half the leftover on each side. Written out rather than recomputed with the same expression the
	// implementation uses, so a sign error has somewhere to show up.
	GYRO_CHECK_EQ(placed->Translation.Model().X, (1920.0 - 800.0) / 2.0);
	GYRO_CHECK_EQ(placed->Translation.Model().Y, (1080.0 - 600.0) / 2.0);
}

GYRO_TEST(Floor, ThePlacementIsInTheOutputsOwnCornerRatherThanTheWorldsOrigin)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	// The second monitor in a left-to-right arrangement. A placement computed against the extent alone
	// would put every window on the first panel, which is what a person with two screens would report
	// as *it always opens on the wrong monitor*.
	const std::array outputs{ Panel(PanelWidth, 0.0) };
	scene.SetOutputs(outputs);

	SessionFloor floor;
	GYRO_REQUIRE(floor.Open(scene).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floor.Container(), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, *window, Window(800.0F, 600.0F));
	}

	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().X, PanelWidth + (1920.0 - 800.0) / 2.0);
}

GYRO_TEST(Floor, AWindowLargerThanTheScreenHangsOffBothEdgesEqually)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	SessionFloor floor;
	GYRO_REQUIRE(floor.Open(scene).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floor.Container(), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, *window, Window(2400.0F, 1400.0F));
	}

	// Negative, and deliberately not clamped: a window too big for the screen is centred on it, which
	// puts its middle where a person is looking. Clamping to the corner would hide the far half of a
	// dialog whose buttons are at the bottom.
	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().X, (1920.0 - 2400.0) / 2.0);
	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().Y, (1080.0 - 1400.0) / 2.0);
}

GYRO_TEST(Floor, APlacementWithNoOutputsChangesNothing)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SessionFloor floor;
	GYRO_REQUIRE(floor.Open(scene).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floor.Container(), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, *window, Window(800.0F, 600.0F));
	}

	// There is no honest centre of nothing, and the window stays at the origin rather than being moved
	// to a coordinate invented here. Nothing is drawing this scene in any case — a world with no
	// outputs publishes a wake schedule nothing can read.
	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().X, 0.0);
}
