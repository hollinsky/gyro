#include "Scene/Reach.h"

#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Geometry/Scale.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Node.h"

// Which outputs a window is on, which is decision 32's question and the one that decides when its
// client is told it may draw again.
//
// Every claim here fails as a client symptom rather than as a compositor one. Too narrow is a window
// that stops repainting — a mask that misses the panel a window is actually on never clears, so the
// callback never goes out and the application freezes with its last frame on screen. Too wide is a
// window paced by a monitor it is not on, which on a mixed-rate desk is a client drawing at 144 Hz
// into a 60 Hz panel and dropping two frames in three.

namespace
{
ManualClock Clock;

// Two side-by-side 1920x1080 panels at 1x, which is the arrangement every multi-output question in
// this file is asked against: the seam is at x = 1920.
void TwoPanels(SceneStore& store)
{
	const SceneOutput left{ .Id = OutputId{ 1, 1 },
		                    .Bounds = { .Origin = { 0.0, 0.0 }, .Extent = { 1920.0, 1080.0 } },
		                    .Density = Scale::FromInteger(1),
		                    .Grid = { 1920, 1080 } };

	const SceneOutput right{ .Id = OutputId{ 2, 1 },
		                     .Bounds = { .Origin = { 1920.0, 0.0 }, .Extent = { 1920.0, 1080.0 } },
		                     .Density = Scale::FromInteger(1),
		                     .Grid = { 1920, 1080 } };

	const SceneOutput set[2]{ left, right };

	store.SetOutputs(set);
}

// A window of `width` x `height` at `x`, as the shape decision 141's floorplanner produces: a
// container holding an image, because that is what `Protocol/Shell.cpp` maps and therefore what is
// ever asked about.
[[nodiscard]] EntityId Window(SceneStore& store, double x, float width, float height)
{
	const EntityId frame =
		store.CreateContainer({}, { .Position = { x, 0.0, 0.0 }, .Extent = { width, height } }).value();

	return store.CreateImage(frame, { .Extent = { width, height } }, ImageContent{}).value();
}
} // namespace

GYRO_TEST(Reach, AWindowOnOnePanelNamesThatPanelAndNoOther)
{
	SceneStore store{ Clock };
	TwoPanels(store);

	GYRO_CHECK_EQ(Reach(store, Window(store, 100.0, 400.0F, 300.0F)), OutputReach{ 0b01 });
	GYRO_CHECK_EQ(Reach(store, Window(store, 2100.0, 400.0F, 300.0F)), OutputReach{ 0b10 });
}

GYRO_TEST(Reach, AWindowStraddlingTheSeamNamesBoth)
{
	SceneStore store{ Clock };
	TwoPanels(store);

	// Decision 32's whole subject: one buffer, one callback queue, and two panels that will show it at
	// different moments. What this mask decides is that the *first* of them to flip is the cadence.
	GYRO_CHECK_EQ(Reach(store, Window(store, 1800.0, 400.0F, 300.0F)), OutputReach{ 0b11 });
}

GYRO_TEST(Reach, AnEdgeExactlyOnTheSeamIsNotOnTheNextPanel)
{
	SceneStore store{ Clock };
	TwoPanels(store);

	// Half-open, as a scissor rectangle is: a window whose right edge is the second panel's left edge
	// puts no pixel on it. Counting it would pace this client against a monitor showing none of it.
	GYRO_CHECK_EQ(Reach(store, Window(store, 1520.0, 400.0F, 300.0F)), OutputReach{ 0b01 });
}

GYRO_TEST(Reach, AWindowOffEveryPanelIsOwedNothing)
{
	SceneStore store{ Clock };
	TwoPanels(store);

	// Not a defect and not a starvation either: nothing is going to present it, so nothing can honestly
	// tell its client to draw. `SceneReturn::Seal` is what re-asks this question on every later
	// publication, so the frame it comes back into view on is the frame its callback goes out on.
	GYRO_CHECK_EQ(Reach(store, Window(store, 6000.0, 400.0F, 300.0F)), OutputReach{ 0 });
}

GYRO_TEST(Reach, HiddenAnywhereAboveIsNowhere)
{
	SceneStore store{ Clock };
	TwoPanels(store);

	const EntityId workspace =
		store.CreateContainer({}, { .Extent = { 1920.0F, 1080.0F }, .Flags = Node::Hidden }).value();
	const EntityId window =
		store.CreateContainer(workspace, { .Position = { 100.0, 0.0, 0.0 }, .Extent = { 400.0F, 300.0F } }).value();
	const EntityId content = store.CreateImage(window, { .Extent = { 400.0F, 300.0F } }, ImageContent{}).value();

	// The frame walk skips a hidden subtree whole, so a window on another workspace is on no panel
	// whatever its own coordinates say. Asking only the node's own flag is how the overview
	// decision 95 describes — nine workspaces of windows under one hidden container — would have every
	// window on it repainting for nothing.
	GYRO_CHECK_EQ(Reach(store, content), OutputReach{ 0 });
}

GYRO_TEST(Reach, TheAnswerFollowsTheParentTheWindowWasMovedBy)
{
	SceneStore store{ Clock };
	TwoPanels(store);

	const EntityId content = Window(store, 100.0, 400.0F, 300.0F);
	const EntityId frame = store.Find(content)->Parent;

	GYRO_REQUIRE_EQ(Reach(store, content), OutputReach{ 0b01 });

	{
		SceneCommit commit{ store, CommitAuthor::Compositor, store.Now(), Transition::None };

		static_cast<void>(commit.Move(frame, { 2100.0, 0.0, 0.0 }));
	}

	// The chain is composed rather than the node read, which is what makes dragging a window across the
	// seam change what paces it. A reading that took the image's own position would have every window
	// answering for the top-left corner of the desk.
	GYRO_CHECK_EQ(Reach(store, content), OutputReach{ 0b10 });
}

GYRO_TEST(Reach, AWorldWithNoOutputsReachesNothing)
{
	SceneStore store{ Clock };

	GYRO_CHECK_EQ(Reach(store, Window(store, 0.0, 400.0F, 300.0F)), OutputReach{ 0 });
}

GYRO_TEST(Reach, AStaleIdNamesNoOutput)
{
	SceneStore store{ Clock };
	TwoPanels(store);

	GYRO_CHECK_EQ(Reach(store, EntityId{ 4, 9 }), OutputReach{ 0 });
}
