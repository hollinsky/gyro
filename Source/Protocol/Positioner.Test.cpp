#include "Protocol/Positioner.h"

#include "Geometry/Space.h"
#include "Testing/Test.h"

// Where a popup lands, which is the whole of what a person notices about a menu before they read it.
//
// **These are the cases that break on a real desk rather than the ones that enumerate the
// enumerations.** A menu opened from a button at the bottom of a screen, one taller than the panel it
// is on, one anchored to a caret at the very edge: every one of them is a rule in `xdg_positioner`
// that reads obvious and produces the wrong rectangle if the arithmetic is a sign or a division out.
// The cases where nothing is constrained are here too, and they are the ones that catch a wrong
// gravity — a menu half a button's width to the left is not a bug anybody files, it is a compositor
// that feels slightly off.

namespace
{
using Anchor = Wayland::Server::XdgPositionerAnchor;
using Gravity = Wayland::Server::XdgPositionerGravity;
using Adjustment = Wayland::Server::XdgPositionerConstraintAdjustment;

// A menu bar button 100 wide and 30 tall, 50 in from the left of a window: the shape most of these
// are about.
constexpr PixelRect<SurfaceSpace> Button{ { 50, 0 }, { 100, 30 } };

// The same button 500 down a 600-tall screen, which is the one that has to open upwards.
constexpr PixelRect<SurfaceSpace> Low{ { 50, 500 }, { 100, 30 } };

// A text caret near the right edge: no width at all, which is legal and is what a completion list
// hangs off.
constexpr PixelRect<SurfaceSpace> Caret{ { 700, 100 }, { 0, 20 } };

// A screen with the parent's window geometry at its origin, which is the arrangement that makes the
// constrained cases readable: an x in these tests is an x on the panel.
constexpr PixelRect<SurfaceSpace> Screen{ { 0, 0 }, { 800, 600 } };

// A complete positioner. Written as a function rather than as an aggregate at each call site because
// the two fields nothing here sets — what the client believed about its parent — would otherwise have
// to be spelled out in every case, which is a dozen lines of noise between a reader and the numbers
// the test is about.
[[nodiscard]] PopupPlacement Rules(
	PixelSize<SurfaceSpace> extent,
	PixelRect<SurfaceSpace> anchorRect,
	Anchor anchor,
	Gravity gravity,
	Adjustment adjustment = Adjustment::None,
	PixelOffset<SurfaceSpace> offset = {}
)
{
	PopupPlacement rules;

	rules.Extent = extent;
	rules.AnchorRect = anchorRect;
	rules.AnchorStated = true;
	rules.Anchor = anchor;
	rules.Gravity = gravity;
	rules.Adjustment = adjustment;
	rules.Offset = offset;

	return rules;
}
} // namespace

GYRO_TEST(Positioner, AMenuHangsBelowTheButtonItCameFrom)
{
	const PopupPlacement rules = Rules({ 200, 300 }, Button, Anchor::BottomLeft, Gravity::BottomRight);

	// The anchor is the button's bottom-left corner and the gravity says *grow down and to the right*,
	// so the menu's own top-left corner is exactly that point. This is what every menu bar in existence
	// asks for.
	GYRO_CHECK_EQ(PlacePopup(rules), (PixelRect<SurfaceSpace>{ { 50, 30 }, { 200, 300 } }));
}

GYRO_TEST(Positioner, ACentredAnchorHalvesTheAnchorAndTheExtent)
{
	const PopupPlacement rules = Rules({ 40, 20 }, Button, Anchor::Bottom, Gravity::None);

	// Anchor `bottom` centres on x, and gravity `none` centres the popup on the anchor point: 50 + 100/2
	// is the middle of the button, less half the popup's own width. A tooltip under a control is this and
	// nothing else, and it is where an off-by-one in either halving shows up.
	GYRO_CHECK_EQ(PlacePopup(rules), (PixelRect<SurfaceSpace>{ { 80, 20 }, { 40, 20 } }));
}

GYRO_TEST(Positioner, TheOffsetIsAppliedAfterTheGravity)
{
	const PopupPlacement rules =
		Rules({ 200, 300 }, Button, Anchor::BottomLeft, Gravity::BottomRight, Adjustment::None, { -8, 4 });

	// A toolkit's shadow margin, which is what an offset almost always is.
	GYRO_CHECK_EQ(PlacePopup(rules), (PixelRect<SurfaceSpace>{ { 42, 34 }, { 200, 300 } }));
}

GYRO_TEST(Positioner, NothingIsAdjustedWhereNothingIsConstrained)
{
	const PopupPlacement rules = Rules(
		{ 200, 300 },
		Button,
		Anchor::BottomLeft,
		Gravity::BottomRight,
		Adjustment::FlipY | Adjustment::SlideX | Adjustment::ResizeY
	);

	// Permission to adjust is not a reason to. A compositor that slid or flipped a menu that already fit
	// would move every menu on the machine, because toolkits ask for all three every time.
	GYRO_CHECK_EQ(PlacePopup(rules, Screen), PlacePopup(rules));
}

GYRO_TEST(Positioner, AMenuAtTheBottomOfTheScreenOpensUpwards)
{
	const PopupPlacement rules = Rules({ 200, 300 }, Low, Anchor::BottomLeft, Gravity::BottomRight, Adjustment::FlipY);

	// Unconstrained it runs from 530 to 830 on a screen that ends at 600.
	GYRO_CHECK_EQ(PlacePopup(rules), (PixelRect<SurfaceSpace>{ { 50, 530 }, { 200, 300 } }));

	// Flipped: the anchor becomes the button's *top* edge and the gravity becomes upward, so the menu's
	// bottom edge sits on 500 and its top on 200. Nothing about x moved, which is the other half of the
	// claim — a flip is per axis, and a menu that jumped sideways when it opened upward would be wrong.
	GYRO_CHECK_EQ(PlacePopup(rules, Screen), (PixelRect<SurfaceSpace>{ { 50, 200 }, { 200, 300 } }));
}

GYRO_TEST(Positioner, AFlipIsTakenWhereItMerelyHelps)
{
	// A button in the middle of the screen with a menu too tall to fit either way: 300 above it and 270
	// below, for a menu wanting 400.
	constexpr PixelRect<SurfaceSpace> middle{ { 50, 300 }, { 100, 30 } };

	const PopupPlacement rules =
		Rules({ 200, 400 }, middle, Anchor::BottomLeft, Gravity::BottomRight, Adjustment::FlipY);

	// **Flipping is taken where it is merely better, not only where it fits.** Downward shows 270 of the
	// menu and upward shows 300, so upward wins — and a person sees a menu whose first item is on screen
	// instead of one hanging off the bottom edge. Refusing every imperfect flip is the reading that looks
	// safer and is worse on exactly the screens where it matters.
	GYRO_CHECK_EQ(PlacePopup(rules, Screen), (PixelRect<SurfaceSpace>{ { 50, -100 }, { 200, 400 } }));
}

GYRO_TEST(Positioner, AMenuAtTheRightEdgeSlidesBackOn)
{
	const PopupPlacement rules =
		Rules({ 200, 150 }, Caret, Anchor::BottomLeft, Gravity::BottomRight, Adjustment::SlideX);

	// Slid until its right edge is on the screen's, and not one pixel further: 800 - 200.
	GYRO_CHECK_EQ(PlacePopup(rules, Screen), (PixelRect<SurfaceSpace>{ { 600, 120 }, { 200, 150 } }));
}

GYRO_TEST(Positioner, APopupWiderThanTheScreenKeepsItsNearEdge)
{
	const PopupPlacement rules =
		Rules({ 1000, 150 }, Caret, Anchor::BottomLeft, Gravity::BottomRight, Adjustment::SlideX);

	// **It has to lose one end and the end it loses is the far one.** Sliding left far enough to put the
	// right edge on the screen would push the left edge to -200, so the left edge wins the clamp — which
	// is the end a person reads a menu from. This is the one case where the protocol's prose and what
	// every compositor ships disagree, and shipping behaviour is what toolkits are written against.
	GYRO_CHECK_EQ(PlacePopup(rules, Screen), (PixelRect<SurfaceSpace>{ { 0, 120 }, { 1000, 150 } }));
}

GYRO_TEST(Positioner, ResizeIsTheLastResortAndOnlyOnItsOwnAxis)
{
	const PopupPlacement rules =
		Rules({ 200, 300 }, Low, Anchor::BottomLeft, Gravity::BottomRight, Adjustment::ResizeY);

	// With no permission to flip or slide, the only way to fit is to be shorter: the menu keeps its top
	// at 530 and loses everything past the bottom of the screen. Its width is untouched, because the x
	// axis was never constrained and a compositor that shrank both would be a menu that changed shape for
	// no reason a person could see.
	GYRO_CHECK_EQ(PlacePopup(rules, Screen), (PixelRect<SurfaceSpace>{ { 50, 530 }, { 200, 70 } }));
}

GYRO_TEST(Positioner, SlideIsPreferredToResizeWhereBothArePermitted)
{
	const PopupPlacement rules =
		Rules({ 200, 300 }, Low, Anchor::BottomLeft, Gravity::BottomRight, Adjustment::SlideY | Adjustment::ResizeY);

	// The precedence is the protocol's — flip, then slide, then resize — and it is what keeps a menu
	// whole. Sliding it up to 300 shows every row; resizing first would cut the last rows off a menu that
	// had room to be moved instead.
	GYRO_CHECK_EQ(PlacePopup(rules, Screen), (PixelRect<SurfaceSpace>{ { 50, 300 }, { 200, 300 } }));
}

GYRO_TEST(Positioner, WithoutPermissionAPopupIsLeftWhereTheClientPutIt)
{
	const PopupPlacement rules = Rules({ 200, 300 }, Low, Anchor::BottomLeft, Gravity::BottomRight);

	// `none` means *do not move it*, and it has to be honoured: a client that positions a popup itself
	// and gets it silently corrected is one whose own arrow no longer points at its own control.
	GYRO_CHECK_EQ(PlacePopup(rules, Screen), (PixelRect<SurfaceSpace>{ { 50, 530 }, { 200, 300 } }));
}

GYRO_TEST(Positioner, NoScreenMeansNoConstraining)
{
	const PopupPlacement rules =
		Rules({ 200, 300 }, Low, Anchor::BottomLeft, Gravity::BottomRight, Adjustment::FlipY | Adjustment::SlideY);

	// A window on no output has no edges to be pushed off. Answering with an empty rectangle rather than
	// inventing a screen is what keeps a headless run — and the moment between a monitor going away and
	// the next one arriving — from collapsing every open menu onto the origin.
	GYRO_CHECK_EQ(PlacePopup(rules, {}), PlacePopup(rules));
}

GYRO_TEST(Positioner, APositionerIsIncompleteUntilItHasASizeAndAnAnchorRectangle)
{
	PopupPlacement rules;

	GYRO_CHECK(!rules.IsComplete());

	rules.Extent = { 200, 300 };

	GYRO_CHECK(!rules.IsComplete());

	// A caret is an anchor rectangle with no width, and it is complete. What makes one incomplete is
	// never having been stated, which is why the bit exists at all: every value a stated rectangle can
	// hold is also the value an unstated one has.
	rules.AnchorRect = Caret;
	rules.AnchorStated = true;

	GYRO_CHECK(rules.IsComplete());
}
