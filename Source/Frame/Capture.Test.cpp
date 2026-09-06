#include "Frame/Capture.h"

#include <array>
#include <cstdint>
#include <span>

#include "Core/Texture.h"
#include "Frame/Evaluator.h"
#include "Geometry/Space.h"
#include "Testing/Test.h"

// Whether a closing window's picture gets drawn, asserted by what the wrong answer looks like.
//
// Drawing it every frame is a window whose exit stutters for exactly as long as somebody is watching
// it, because a snapshot costs what a whole window costs. Never drawing it again after the rectangle
// is recycled is one window leaving wearing the picture of the window that closed before it — on
// screen, for a whole fade, and only on a machine busy enough to be dropping publications. Trusting
// the proposal instead of the renderer's own count is uninitialised device memory on the glass.
namespace
{
[[nodiscard]] ExitCapture Closing(std::uint32_t reservation, std::uint32_t texture = 0)
{
	return { .Into = TextureId{ texture == 0 ? reservation : texture, 1 },
		     .Slot = PixelRect<BufferSpace>{ { 0, 0 }, { 64, 32 } },
		     .Source = Rect<DeviceSpace>{ { 10.0F, 10.0F }, { 64.0F, 32.0F } },
		     .Reservation = reservation,
		     .First = 3,
		     .Count = 2 };
}
} // namespace

GYRO_TEST(ExitCaptures, AWindowsPictureIsDrawnOnceAndNotOnceAFrame)
{
	ExitCaptures captures;
	const std::array closing{ Closing(1) };

	const std::span<const SnapshotCapture> first = captures.Propose(closing, 0);

	GYRO_REQUIRE_EQ(first.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(first[0].Into, TextureId{ 1, 1 });
	GYRO_CHECK_EQ(first[0].First, 3U);
	GYRO_CHECK_EQ(first[0].Count, 2U);

	captures.Landed(1);

	// The window is still leaving and still in the walk's report, and there is nothing left to draw.
	GYRO_CHECK(captures.Propose(closing, 0).empty());
	GYRO_CHECK(captures.Propose(closing, 0).empty());
	GYRO_CHECK(captures.Holds(0, 1));
}

GYRO_TEST(ExitCaptures, ARendererThatTookNoneIsOfferedTheSameWindowAgain)
{
	ExitCaptures captures;
	const std::array closing{ Closing(1) };

	GYRO_REQUIRE_EQ(captures.Propose(closing, 0).size(), std::size_t{ 1 });

	// A screen whose renderer has no snapshot path, or a frame where the atlas image was not there
	// yet. Remembering a picture nobody drew would leave the window fading from whatever that memory
	// happened to hold.
	captures.Landed(0);

	GYRO_CHECK_EQ(captures.Propose(closing, 0).size(), std::size_t{ 1 });
	GYRO_CHECK(!captures.Holds(0, 1));
}

GYRO_TEST(ExitCaptures, OnlyTheOnesTheRendererCountedAreRemembered)
{
	ExitCaptures captures;
	const std::array closing{ Closing(1), Closing(2) };

	GYRO_REQUIRE_EQ(captures.Propose(closing, 0).size(), std::size_t{ 2 });

	// The renderer ran out partway, which `Submission::Captured` reports as a prefix.
	captures.Landed(1);

	const std::span<const SnapshotCapture> again = captures.Propose(closing, 0);

	GYRO_REQUIRE_EQ(again.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(again[0].Into, TextureId{ 2, 1 });
}

GYRO_TEST(ExitCaptures, TheNextOccupantOfARectangleGetsItsOwnPicture)
{
	ExitCaptures captures;
	const std::array first{ Closing(1) };

	GYRO_REQUIRE_EQ(captures.Propose(first, 0).size(), std::size_t{ 1 });
	captures.Landed(1);

	// The first window finished and the shelf handed the same texels to the next one, under a
	// reservation of its own. Remembering places rather than counts is what would put the first
	// window's picture on the second one's fade.
	const std::array second{ Closing(2, 1) };
	const std::span<const SnapshotCapture> next = captures.Propose(second, 0);

	GYRO_REQUIRE_EQ(next.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(next[0].Into, TextureId{ 1, 1 });
	GYRO_CHECK(!captures.Holds(0, 1));
}

GYRO_TEST(ExitCaptures, OneScreenHavingDrawnItSaysNothingAboutTheOther)
{
	ExitCaptures captures;
	const std::array closing{ Closing(1) };

	// Decision 190: a window on the seam is captured in both atlases, and the two are separate
	// rectangles under separate reservations — but the confirmation is filed per output either way.
	GYRO_REQUIRE_EQ(captures.Propose(closing, 0).size(), std::size_t{ 1 });
	captures.Landed(1);

	GYRO_CHECK_EQ(captures.Propose(closing, 1).size(), std::size_t{ 1 });
	GYRO_CHECK(captures.Holds(0, 1));
	GYRO_CHECK(!captures.Holds(1, 1));
}

GYRO_TEST(ExitCaptures, AConfirmationBelongsToTheProposalBeforeIt)
{
	ExitCaptures captures;
	const std::array closing{ Closing(1) };

	GYRO_REQUIRE_EQ(captures.Propose(closing, 0).size(), std::size_t{ 1 });

	// A second output's frame ran before the first one's submission was filed. The confirmation lands
	// against the output the last proposal spoke for and never against the one before it.
	GYRO_REQUIRE_EQ(captures.Propose(closing, 1).size(), std::size_t{ 1 });
	captures.Landed(1);

	GYRO_CHECK(!captures.Holds(0, 1));
	GYRO_CHECK(captures.Holds(1, 1));
}

// The walk reports at most `MaxExitCaptures`, so this input is one it cannot produce — this is the
// bound holding on its own account rather than a property of the two composed. `Frame/Evaluator.h`'s
// `MaxExitCaptures` says what actually happens to a seventeenth window: it waits for one of the
// sixteen to finish, and draws its own pixels while it waits.
GYRO_TEST(ExitCaptures, MoreProposedThanTheFrameHoldsIsBoundedRatherThanOverrunning)
{
	ExitCaptures captures;
	std::array<ExitCapture, MaxExitCaptures + 2> closing{};

	for (std::uint32_t index = 0; index < closing.size(); ++index)
	{
		closing[index] = Closing(index + 1);
	}

	GYRO_CHECK_EQ(captures.Propose(closing, 0).size(), MaxExitCaptures);
	captures.Landed(MaxExitCaptures);

	// The two that did not fit are still owed rather than dropped.
	GYRO_CHECK_EQ(captures.Propose(closing, 0).size(), std::size_t{ 2 });
}
