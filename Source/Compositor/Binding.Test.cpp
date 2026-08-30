#include "Compositor/Binding.h"

#include <array>
#include <cmath>
#include <span>

#include "Core/Input.h"
#include "Scene/Output.h"
#include "Testing/Test.h"

// **The two things worth testing here are the two that are invisible on the machine this was written
// on.** The rung selection is a policy with a refusal in it, and a refusal is exactly the behaviour
// nobody notices missing until a finger lands on the wrong monitor. The mapping is arithmetic that is
// *correct on an unrotated 1x panel whichever way you write it*, which is the one screen anybody
// developing this has in front of them — so a portrait 2x output that nobody owns is the whole reason
// this file exists.

namespace
{
// A panel as the world would hold it, run through `SceneOutput::Placement()` rather than through a
// hand-written adapter. That is deliberate: what is being checked is that a fraction unprojects
// through the *same* transform the frame walk projects with, and an adapter assembled here by hand
// would be a second copy that agrees on the day it is written.
AbsolutePanel Panel(
	std::string_view connector,
	PanelSize millimetres,
	PixelSize<DeviceSpace> grid,
	Point<GlobalSpace> origin,
	Scale density = Scale::FromInteger(1),
	AxisOrientation orientation = AxisOrientation::Normal
)
{
	const bool swapped = SwapsAxes(orientation);
	const double logicalWidth =
		static_cast<double>(density.LogicalFromDevice(swapped ? grid.Height : grid.Width, Rounding::Nearest));
	const double logicalHeight =
		static_cast<double>(density.LogicalFromDevice(swapped ? grid.Width : grid.Height, Rounding::Nearest));

	const SceneOutput output{
		.Bounds = { origin, { logicalWidth, logicalHeight } },
		.Density = density,
		.Grid = grid,
		.Orientation = orientation,
	};

	return { .Connector = connector, .Size = millimetres, .Grid = grid, .Placement = output.Placement() };
}

InputDevice Touchscreen(InputDeviceSize size = { 344.0, 194.0 }, std::string_view property = {})
{
	return { .Id = {}, .Name = "a touchscreen", .Absolute = true, .Size = size, .Output = property };
}

// Within a hundredth of a logical pixel, which is far finer than anything a finger resolves and far
// coarser than the ulp an exact inverse leaves behind.
bool Near(Point<GlobalSpace> point, double x, double y)
{
	return std::abs(point.X - x) < 0.01 && std::abs(point.Y - y) < 0.01;
}
} // namespace

GYRO_TEST(Binding, TheSoleOutputTakesADeviceWithNoEvidenceAtAll)
{
	// The laptop, the tablet and the kiosk: the ambiguity does not exist, so no evidence is needed and
	// a device with no millimetres and no property still works.
	const std::array panels{ Panel("eDP-1", { 0, 0 }, { 1920, 1080 }, { 0.0, 0.0 }) };
	const InputDevice device{ .Id = {}, .Name = "a touchscreen", .Absolute = true, .Size = {}, .Output = {} };

	GYRO_CHECK(Bind(device, panels) == DeviceBinding{ .Rung = BindRung::Sole, .Output = 0 });
}

GYRO_TEST(Binding, NoOutputsBindNothing)
{
	GYRO_CHECK(!Bind(Touchscreen(), {}).IsBound());
}

GYRO_TEST(Binding, ThePropertyBeatsASizeThatWouldHaveMatchedSomethingElse)
{
	// The rung order is the whole of what this checks: the device's millimetres are DP-1's exactly, and
	// the property says HDMI-A-1 anyway. A property that lost to an inference would be unfixable.
	const std::array panels{
		Panel("DP-1", { 344, 194 }, { 1920, 1080 }, { 0.0, 0.0 }),
		Panel("HDMI-A-1", { 700, 390 }, { 1920, 1080 }, { 1920.0, 0.0 }),
	};

	GYRO_CHECK(
		Bind(Touchscreen({ 344.0, 194.0 }, "HDMI-A-1"), panels) ==
		DeviceBinding{ .Rung = BindRung::Property, .Output = 1 }
	);
}

GYRO_TEST(Binding, APropertyNamingAConnectorThatIsNotHereFallsThrough)
{
	// Somebody unplugged the monitor their rule names. The rung below still answers, and the
	// alternative — binding to nothing because a stale string exists — would be a touchscreen that
	// stopped working when an unrelated cable came out.
	const std::array panels{ Panel("eDP-1", { 0, 0 }, { 1920, 1080 }, { 0.0, 0.0 }) };

	GYRO_CHECK(Bind(Touchscreen({ 344.0, 194.0 }, "DP-3"), panels).Rung == BindRung::Sole);
}

GYRO_TEST(Binding, TheSizeMatchTakesTheOnePanelItIsTheSameGlassAs)
{
	const std::array panels{
		Panel("DP-1", { 700, 390 }, { 2560, 1440 }, { 0.0, 0.0 }),
		Panel("eDP-1", { 344, 194 }, { 1920, 1080 }, { 2560.0, 0.0 }),
	};

	GYRO_CHECK(Bind(Touchscreen({ 344.0, 194.0 }), panels) == DeviceBinding{ .Rung = BindRung::Size, .Output = 1 });
}

GYRO_TEST(Binding, ACentimetreOfRoundingStillMatches)
{
	// EDID states an image size in whole centimetres in the basic block and in millimetres in a detailed
	// descriptor, so a panel reporting only the first arrives rounded. A tolerance tighter than that
	// unit would refuse a match that is correct.
	const std::array panels{ Panel("DP-1", { 340, 190 }, { 1920, 1080 }, { 0.0, 0.0 }),
		                     Panel("DP-2", { 700, 390 }, { 1920, 1080 }, { 1920.0, 0.0 }) };

	GYRO_CHECK(Bind(Touchscreen({ 344.0, 194.0 }), panels).Output == 0);

	// And a centimetre and a half does not, which is what keeps the tolerance a rounding allowance
	// rather than a licence.
	GYRO_CHECK(!Bind(Touchscreen({ 355.0, 194.0 }), panels).IsBound());
}

GYRO_TEST(Binding, TwoPanelsOfOneSizeLeaveTheDeviceUnboundRatherThanGuessing)
{
	const std::array panels{
		Panel("DP-1", { 344, 194 }, { 1920, 1080 }, { 0.0, 0.0 }),
		Panel("DP-2", { 344, 194 }, { 1920, 1080 }, { 1920.0, 0.0 }),
	};

	const InputDevice device = Touchscreen();

	GYRO_CHECK(!Bind(device, panels).IsBound());

	// And the log line can say which two, because the tie is re-derived with the same predicate that
	// refused rather than with a second one written for the message.
	std::array<std::size_t, 4> candidates{};

	GYRO_REQUIRE(SizeCandidates(device, panels, candidates) == 2);
	GYRO_CHECK(candidates[0] == 0);
	GYRO_CHECK(candidates[1] == 1);
}

GYRO_TEST(Binding, APanelThatReportsNoSizeIsNoEvidenceRatherThanAMatchAgainstZero)
{
	// A projector and every virtual output say nothing here, and a device that says nothing does too.
	const std::array panels{
		Panel("DP-1", { 0, 0 }, { 1920, 1080 }, { 0.0, 0.0 }),
		Panel("DP-2", { 0, 0 }, { 1920, 1080 }, { 1920.0, 0.0 }),
	};

	GYRO_CHECK(!Bind(Touchscreen({ 0.0, 0.0 }), panels).IsBound());
	GYRO_CHECK(!Bind(Touchscreen({ 344.0, 194.0 }), panels).IsBound());

	const InputDevice unmeasured{ .Id = {}, .Name = "a touchscreen", .Absolute = true, .Size = {}, .Output = {} };

	GYRO_CHECK(!Bind(unmeasured, panels).IsBound());
}

GYRO_TEST(Binding, AFractionLandsOnTheOutputItIsBoundTo)
{
	const AbsolutePanel panel = Panel("DP-1", { 344, 194 }, { 1920, 1080 }, { 0.0, 0.0 });

	GYRO_CHECK(Near(Land(panel, 0.0, 0.0), 0.0, 0.0));
	GYRO_CHECK(Near(Land(panel, 1.0, 1.0), 1920.0, 1080.0));
	GYRO_CHECK(Near(Land(panel, 0.5, 0.25), 960.0, 270.0));
}

GYRO_TEST(Binding, ASecondOutputLandsWhereItSitsInTheLayout)
{
	// The fraction is of the device's own glass and knows nothing about the layout, so the origin has
	// to come out of the placement rather than out of the contact.
	const AbsolutePanel panel = Panel("DP-2", { 344, 194 }, { 1920, 1080 }, { 1920.0, 0.0 });

	GYRO_CHECK(Near(Land(panel, 0.0, 0.0), 1920.0, 0.0));
	GYRO_CHECK(Near(Land(panel, 1.0, 1.0), 3840.0, 1080.0));
}

GYRO_TEST(Binding, TheGridRatherThanTheLayoutRectangleIsWhatAFractionSpans)
{
	// A 2x panel: 2560x1440 of glass is 1280x720 of world. Mapping the fraction onto the layout
	// rectangle would be right here *by accident*, because both errors are proportional — the test
	// below is where that stops being true.
	const AbsolutePanel panel = Panel("eDP-1", { 344, 194 }, { 2560, 1440 }, { 0.0, 0.0 }, Scale::FromInteger(2));

	GYRO_CHECK(Near(Land(panel, 1.0, 1.0), 1280.0, 720.0));
	GYRO_CHECK(Near(Land(panel, 0.5, 0.5), 640.0, 360.0));
}

GYRO_TEST(Binding, APortraitPanelTurnsTheGlassWithIt)
{
	// The one that fails if the fraction is stretched over the layout rectangle. The monitor is
	// mounted turned a quarter, so 1080x1920 of glass is 1080 wide in the world — and the top of the
	// panel, which is where the person is touching, is not the top of the layout rectangle.
	const AbsolutePanel panel =
		Panel("DP-1", { 344, 194 }, { 1920, 1080 }, { 0.0, 0.0 }, Scale::FromInteger(1), AxisOrientation::Rotate90);

	const Point<GlobalSpace> topLeft = Land(panel, 0.0, 0.0);
	const Point<GlobalSpace> topRight = Land(panel, 1.0, 0.0);

	// Along the glass's own x axis is *down* the screen once it is stood on end. Getting this wrong is
	// a person touching the top of their monitor and pressing the side of it.
	GYRO_CHECK(std::abs(topRight.X - topLeft.X) < 0.01);
	GYRO_CHECK(std::abs(topRight.Y - topLeft.Y) > 1000.0);
}

GYRO_TEST(Binding, EveryCornerOfEveryOrientationStaysOnTheOutput)
{
	// The property that has to hold whatever the panel is doing: a touchscreen fills rather than fits,
	// so the four corners of the glass are the four corners of the output's rectangle in some order,
	// and nothing lands outside it. This is the check that would have caught an inverse composed the
	// wrong way round in any of the eight.
	constexpr std::array Orientations{ AxisOrientation::Normal,     AxisOrientation::Rotate90,
		                               AxisOrientation::Rotate180,  AxisOrientation::Rotate270,
		                               AxisOrientation::Flipped,    AxisOrientation::Flipped90,
		                               AxisOrientation::Flipped180, AxisOrientation::Flipped270 };

	for (const AxisOrientation orientation : Orientations)
	{
		const bool swapped = SwapsAxes(orientation);
		const double width = swapped ? 540.0 : 960.0;
		const double height = swapped ? 960.0 : 540.0;

		const AbsolutePanel panel =
			Panel("DP-1", { 344, 194 }, { 1920, 1080 }, { 100.0, 200.0 }, Scale::FromInteger(2), orientation);

		for (const double x : { 0.0, 1.0 })
		{
			for (const double y : { 0.0, 1.0 })
			{
				const Point<GlobalSpace> at = Land(panel, x, y);

				GYRO_CHECK(at.X > 100.0 - 0.01 && at.X < 100.0 + width + 0.01);
				GYRO_CHECK(at.Y > 200.0 - 0.01 && at.Y < 200.0 + height + 0.01);
			}
		}

		// And the centre of the glass is the centre of the output whichever way it is turned, which is
		// the one point no orientation may move.
		GYRO_CHECK(Near(Land(panel, 0.5, 0.5), 100.0 + width / 2.0, 200.0 + height / 2.0));
	}
}
