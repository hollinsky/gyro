#include "Scene/Serializer.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Animation/Author/Retarget.h"
#include "Core/Clock.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/Scale.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Reader/Reader.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Configuration.h"
#include "World/Content.h"
#include "World/Exit.h"
#include "World/Node.h"

// What the store becomes on the wire: Docs/Decisions.md decision 86's preorder run, with the active
// coefficients beside it and the per-kind payloads beside those.
//
// The walk that consumes this is Frame's and is asserted against it in Source/Integration. What is
// checked here is the serialisation itself, which is the half of the crossing that has an answer a
// test can write down.

namespace
{
// The scene's clock, parked at the epoch. A commit clamps its origin forward to dispatch's own now, so
// a running clock here would move every `t₀` these tests write to whenever the suite happened to run.
ManualClock Clock;

[[nodiscard]] NodeProperties Panel(float width, float height)
{
	return { .Extent = { width, height } };
}

[[nodiscard]] SceneOutput Primary()
{
	return { .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } };
}
} // namespace

GYRO_TEST(SceneSerializer, ASubtreeLengthCountsNodesAndNotChildren)
{
	SceneStore store{ Clock };

	// A menu holding its panel and an open submenu, which is holding its own — the same worked example
	// World/Node.Test.cpp and Source/Integration/SceneRoundTrip.Test.cpp use.
	const EntityId menu = store.CreateContainer({}, {}).value();
	GYRO_CHECK(store.CreateImage(menu, Panel(100.0F, 40.0F), ImageContent{}).has_value());
	const EntityId submenu = store.CreateContainer(menu, {}).value();
	GYRO_CHECK(store.CreateImage(submenu, Panel(80.0F, 30.0F), ImageContent{}).has_value());

	SceneSerializer serializer;
	serializer.Serialize(store);

	const std::span<const Node> nodes = serializer.Nodes();

	GYRO_REQUIRE_EQ(nodes.size(), std::size_t{ 4 });

	// Preorder: menu, its panel, the submenu, the submenu's panel. The lengths are run lengths over
	// nodes and the two differ the moment anything nests — the menu holds two children and three nodes.
	GYRO_CHECK_EQ(nodes[0].SubtreeLength, std::uint32_t{ 3 });
	GYRO_CHECK_EQ(nodes[1].SubtreeLength, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(nodes[2].SubtreeLength, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(nodes[3].SubtreeLength, std::uint32_t{ 0 });

	GYRO_CHECK_EQ(nodes[1].Extent, Size<SurfaceSpace, float>{ 100.0F, 40.0F });
	GYRO_CHECK_EQ(nodes[3].Extent, Size<SurfaceSpace, float>{ 80.0F, 30.0F });

	// A skip is `index += 1 + SubtreeLength`, so every subtree has to end inside the run. This is the
	// comparison the frame side makes per node, made once here against what produced it.
	for (std::size_t index = 0; index < nodes.size(); ++index)
	{
		GYRO_CHECK(nodes[index].Past(index) <= nodes.size());
	}
}

GYRO_TEST(SceneSerializer, SiblingsAtTheTopLevelAreARunAndNotAForest)
{
	SceneStore store{ Clock };

	const EntityId first = store.CreateContainer({}, {}).value();
	GYRO_CHECK(store.CreateSolid(first, Panel(4.0F, 4.0F), SolidContent{}).has_value());
	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneSerializer serializer;
	serializer.Serialize(store);

	// Frame/Evaluator.h starts at index zero and runs to the end of the node run treating that span as
	// siblings, which is why the store's top level is a list and not a root: a distinguished root would
	// be a node every walk pays for and no reader needs.
	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(serializer.Nodes()[0].SubtreeLength, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(serializer.Nodes()[2].SubtreeLength, std::uint32_t{ 0 });
}

GYRO_TEST(SceneSerializer, OnlyAMovingChannelCrossesAsACoefficient)
{
	SceneStore store{ Clock };

	const EntityId still = store.CreateContainer({}, {}).value();
	const EntityId moving = store.CreateContainer({}, {}).value();

	SceneSerializer serializer;
	serializer.Serialize(store);

	// A still desktop publishes no coefficients at all, which is the whole of what the active set of
	// decision 86 buys — the alternative makes a scene where nothing is happening cost the same to
	// publish as one where everything is.
	GYRO_CHECK_EQ(serializer.ActiveTranslations(), std::size_t{ 0 });
	GYRO_CHECK(!serializer.Nodes()[0].IsTranslating());

	{
		SceneCommit commit{ store,
			                CommitAuthor::Shell,
			                Instant{},
			                SceneCommit::Uncatalogued{
								{ .Translation = Animate(Motion::Standard), .Opacity = Animate(Motion::Standard) } } };

		GYRO_REQUIRE(commit.Move(moving, { 40.0, 0.0, 0.0 }));
		GYRO_REQUIRE(commit.Fade(moving, 0.0F));
	}

	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.ActiveTranslations(), std::size_t{ 1 });
	GYRO_CHECK_EQ(serializer.ActiveOpacities(), std::size_t{ 1 });
	GYRO_CHECK_EQ(serializer.ActiveScales(), std::size_t{ 0 });

	// The index is a position within its *own* channel's array, which is decision 90's one run per
	// channel: pointing a scale index at a rotation spring stops being expressible rather than caught.
	GYRO_CHECK_EQ(serializer.Nodes()[1].TranslationSpring, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(serializer.Nodes()[1].OpacitySpring, std::uint32_t{ 0 });
	GYRO_CHECK(!serializer.Nodes()[1].IsScaling());

	// And the entity that is not moving still names nothing, which is decision 98's statement that a
	// node at rest is a node naming no coefficient.
	GYRO_CHECK(!serializer.Nodes()[0].IsTranslating());
	GYRO_CHECK(still == store.FirstRoot());
}

GYRO_TEST(SceneSerializer, TheContentIndexIsTranslatedAndNeverForwarded)
{
	SceneStore store{ Clock };

	ImageContent first{};
	first.Frame = { { 1.0F, 0.0F }, { 2.0F, 2.0F } };

	ImageContent second{};
	second.Frame = { { 5.0F, 0.0F }, { 2.0F, 2.0F } };

	const EntityId container = store.CreateContainer({}, {}).value();
	GYRO_CHECK(store.CreateImage(container, Panel(2.0F, 2.0F), first).has_value());
	GYRO_CHECK(store.CreateImage({}, Panel(2.0F, 2.0F), second).has_value());

	SceneSerializer serializer;
	const SnapshotPublisher& publisher = serializer.Serialize(store);

	const SnapshotBuffer buffer = publisher.Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Node> nodes = reader.Nodes<Node>();
	const std::span<const ImageContent> images = reader.Images<ImageContent>();

	GYRO_REQUIRE_EQ(nodes.size(), std::size_t{ 3 });
	GYRO_REQUIRE_EQ(images.size(), std::size_t{ 2 });

	// The published run is built by the walk that emits it, so an entity's own content index and the
	// one that crosses agree only by accident. Here they happen to, which is why the assertion is that
	// each node reaches *its own* record rather than that the numbers match.
	GYRO_CHECK(images[nodes[1].Content] == first);
	GYRO_CHECK(images[nodes[2].Content] == second);
	GYRO_CHECK_EQ(nodes[0].Content, NoContent);
}

GYRO_TEST(SceneSerializer, AReferenceResolvesBackwardsOrNotAtAll)
{
	SceneStore store{ Clock };

	const EntityId window = store.CreateImage({}, Panel(100.0F, 40.0F), ImageContent{}).value();
	const EntityId tile = store.CreateReference({}, Panel(100.0F, 40.0F), window).value();

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 2 });

	// Decision 95 requires the target's index to be lower than the reference's, which makes a cycle
	// unrepresentable rather than something the frame thread has to detect — and decision 90 declines
	// to rest on a detector, since an unbounded walk inside the frame section on a SCHED_FIFO thread
	// ends with RLIMIT_RTTIME taking every session's UI at once.
	GYRO_CHECK(serializer.Nodes()[1].IsReference());
	GYRO_CHECK_EQ(serializer.Nodes()[1].Content, std::uint32_t{ 0 });
	GYRO_CHECK(tile != window);
}

GYRO_TEST(SceneSerializer, AForwardReferenceNamesNothing)
{
	SceneStore store{ Clock };

	// Authored the wrong way round: the tile is emitted before the window it presents, because the top
	// level is a list in the order it was built. What the overview actually does is put the real
	// windows near the top of the run under a hidden container and the thumbnails below them.
	const EntityId tile =
		store.CreateReference({}, Panel(100.0F, 40.0F), EntityId{ .Index = 1, .Generation = 2 }).value();
	const EntityId window = store.CreateImage({}, Panel(100.0F, 40.0F), ImageContent{}).value();

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 2 });

	// The id is live and the node index is not below the reference's, so it names nothing rather than
	// naming a node the walk would have to bound. Frame/Evaluator.h refuses the expansion a second time
	// on the same comparison; this is the side that makes the byte unwritable.
	GYRO_CHECK(store.IsLive(window));
	GYRO_CHECK(store.Find(tile)->Target == window);
	GYRO_CHECK_EQ(serializer.Nodes()[0].Content, NoContent);
}

GYRO_TEST(SceneSerializer, TheViewsRunIsOnePlacementPerOutputInOutputOrder)
{
	SceneStore store{ Clock };

	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneOutput secondary = Primary();
	secondary.Bounds = { { 1920.0, 0.0 }, { 1920.0, 1080.0 } };

	const SceneOutput both[] = { Primary(), secondary };
	store.SetOutputs(both);

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const OutputAdapter> views = reader.Views<OutputAdapter>();

	GYRO_REQUIRE_EQ(views.size(), std::size_t{ 2 });
	GYRO_CHECK(views[0] == OutputAdapter::Identity());
	GYRO_CHECK_EQ(views[1].Translation.X, -1920.0);
}

// Decision 21's partition, writer's half. `Frame/Evaluator.h` walks the root run with a single
// forward cursor and compares the node index it finds against the node it is standing on, so what it
// needs of this file is exactly two properties: every root is named, and they are named in increasing
// node order. A root the run skipped would be shown on every output — the walk fails towards visible —
// which on a machine with two people logged in is one of them looking at the other's windows.
GYRO_TEST(SceneSerializer, TheRootRunNamesEveryTopLevelNodeInNodeOrder)
{
	SceneStore store{ Clock };

	// Two roots with a child each, so that the second root's node index is not its ordinal and a run
	// built by counting roots rather than by recording where they landed would disagree here.
	const EntityId first = store.CreateContainer({}, {}).value();
	GYRO_CHECK(store.CreateSolid(first, Panel(4.0F, 4.0F), SolidContent{}).has_value());

	const EntityId second = store.CreateContainer({}, {}).value();
	GYRO_CHECK(store.CreateSolid(second, Panel(4.0F, 4.0F), SolidContent{}).has_value());

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const SceneRoot> roots = reader.Roots<SceneRoot>();

	GYRO_REQUIRE_EQ(roots.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(roots[0].Node, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(roots[1].Node, std::uint32_t{ 2 });

	// Every root is gyro's until a session has a root container of its own, which is the step this run
	// exists to make cheap. Asserted rather than assumed, because the day it stops being true is the day
	// something has to place a window under the right one.
	GYRO_CHECK(roots[0].Session == SessionId::None);
	GYRO_CHECK(roots[1].Session == SessionId::None);
}

// The other half, which is one output's assignment carried rather than computed. `SceneOutput` holds
// it because the composition root wrote it there; this file only has to put it on the wire in the
// order decision 84 governs every per-output run by.
GYRO_TEST(SceneSerializer, TheAssignmentRunIsOneSessionPerOutputInOutputOrder)
{
	SceneStore store{ Clock };

	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneOutput showing = Primary();
	showing.Session = static_cast<SessionId>(4);

	SceneOutput own = Primary();
	own.Id = OutputId{ 2, 1 };
	own.Bounds = { { 1920.0, 0.0 }, { 1920.0, 1080.0 } };

	const SceneOutput both[] = { showing, own };
	store.SetOutputs(both);

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const SceneAssignment> sessions = reader.Sessions<SceneAssignment>();

	GYRO_REQUIRE_EQ(sessions.size(), std::size_t{ 2 });
	GYRO_CHECK(sessions[0].Shown == static_cast<SessionId>(4));

	// An output nothing has been assigned to is showing gyro's own scene, which crosses as itself.
	GYRO_CHECK(sessions[1].Shown == SessionId::None);
}

// Decision 73's request, which crosses beside the assignment and is carried rather than computed: what
// the store was asked for, with the generation moving for the output whose power changed and for no other.
GYRO_TEST(SceneSerializer, TheConfigurationRunIsWhatEachOutputWasAskedToBe)
{
	SceneStore store{ Clock };

	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneOutput lit = Primary();
	lit.Id = OutputId{ 1, 1 };
	lit.Generation = 1;

	SceneOutput dark = Primary();
	dark.Id = OutputId{ 2, 1 };
	dark.Generation = 1;
	dark.Bounds = { { 1920.0, 0.0 }, { 1920.0, 1080.0 } };

	const SceneOutput both[] = { lit, dark };
	store.SetOutputs(both);
	store.SetOutputPower(OutputId{ 2, 1 }, false);

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const SceneConfiguration> configurations = reader.Configurations<SceneConfiguration>();

	GYRO_REQUIRE_EQ(configurations.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(configurations[0].Generation, std::uint64_t{ 1 });
	GYRO_CHECK(configurations[0].Powered);
	GYRO_CHECK_EQ(configurations[1].Generation, std::uint64_t{ 2 });
	GYRO_CHECK(!configurations[1].Powered);
}

GYRO_TEST(SceneSerializer, ASecondSerialisationKeepsNothingOfTheFirst)
{
	SceneStore store{ Clock };

	const EntityId moving = store.CreateContainer({}, {}).value();
	const EntityId panel = store.CreateImage(moving, Panel(10.0F, 10.0F), ImageContent{}).value();

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::MatchedMove };

		GYRO_REQUIRE(commit.Move(moving, { 40.0, 0.0, 0.0 }));
	}

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.ActiveTranslations(), std::size_t{ 1 });

	// The same channel written again with no motion, which is how a commit spells *land it here*. What
	// the serialisation has to notice is that the run it staged last time is not this scene's.
	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::None };

		GYRO_REQUIRE(commit.Move(moving, { 40.0, 0.0, 0.0 }));
	}

	const SnapshotBuffer buffer = serializer.Serialize(store).Build(2);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	// The publisher is reused across calls, so a run left unstaged would be the previous scene's — a
	// node pointing at a spring that belonged to something else, which draws a plausible wrong picture
	// rather than failing.
	GYRO_CHECK(reader.Run<Spring<Vector3<double>>>(SnapshotRun::Translation).empty());
	GYRO_REQUIRE_EQ(reader.Nodes<Node>().size(), std::size_t{ 2 });
	GYRO_CHECK(!reader.Nodes<Node>()[0].IsTranslating());
	GYRO_CHECK_EQ(reader.Images<ImageContent>().size(), std::size_t{ 1 });
	GYRO_CHECK(panel != moving);
}

// The wake schedule and retirement, which are one mechanism seen from its two ends: a channel that is
// still owed frames crosses as a coefficient *and* as a term in the fold, and one that has settled
// crosses as neither. What the fold is worth end to end — an animation that is actually drawn, and an
// output that actually sleeps afterwards — is Source/Integration/SceneIdle.Test.cpp's, because it takes
// a frame loop to say it.

GYRO_TEST(SceneSerializer, AMovingSceneOwesEveryFrameAndSaysSoOncePerOutput)
{
	ManualClock clock;
	SceneStore store{ clock };

	const EntityId window = store.CreateContainer({}, {}).value();
	const SceneOutput outputs[] = { Primary(), Primary() };

	store.SetOutputs(outputs);

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::MatchedMove };

		GYRO_REQUIRE(commit.Move(window, { 400.0, 0.0, 0.0 }));
	}

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);

	// A spring in flight has no next interesting instant to name, because every instant until it settles
	// is one — so the contribution is a standing commitment to every frame the output offers, which is
	// decision 69's third case and the reason the answer is not an optional instant.
	GYRO_CHECK(serializer.SceneWake() == Wake::EveryFrame(clock.Now()));

	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	// One entry per output, which is decision 84's rule: a schedule of any other length is read by the
	// frame side as no information rather than as partial information, and an output would then be told
	// nothing is owed while something is moving on it.
	const std::span<const Wake> schedule = reader.Wakes();

	GYRO_REQUIRE_EQ(schedule.size(), std::size_t{ 2 });
	GYRO_CHECK(schedule[0] == serializer.SceneWake());
	GYRO_CHECK(schedule[1] == serializer.SceneWake());

	// And dispatch is told when to look again, as one instant rather than as a standing commitment: it
	// has exactly one thing to do while an animation runs, at the moment something comes to rest.
	GYRO_CHECK(serializer.Republish().Which == Wake::Kind::Timed);
	GYRO_CHECK(serializer.Republish().When > clock.Now());
}

GYRO_TEST(SceneSerializer, ASettledChannelIsRetiredRatherThanRepublished)
{
	ManualClock clock;
	SceneStore store{ clock };

	const EntityId window = store.CreateContainer({}, {}).value();
	const SceneOutput outputs[] = { Primary() };

	store.SetOutputs(outputs);

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::MatchedMove };

		GYRO_REQUIRE(commit.Move(window, { 400.0, 0.0, 0.0 }));
	}

	SceneSerializer serializer;

	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.ActiveTranslations(), std::size_t{ 1 });
	GYRO_REQUIRE(!store.Find(window)->Translation.IsAtRest());

	// The instant the serializer itself named. Nothing observed an evaluation to find it — decision 11's
	// analytic settle is what lets the dispatch side know a spring has finished without one.
	const Wake owed = serializer.Republish();

	GYRO_REQUIRE(owed.Which == Wake::Kind::Timed);
	clock.Set(owed.When);

	serializer.Serialize(store);

	// **Retired, which is three statements at once and the reason they cannot come apart.** No
	// coefficient crosses, so the frame walk reads the inline value and counts the node as still. The
	// schedule says nothing further is owed, so the output folds to idle. And the property is genuinely
	// at rest on the model value the commit set, so the next write against it is an ordinary retarget
	// rather than an interruption of a motion nobody could see.
	GYRO_CHECK_EQ(serializer.ActiveTranslations(), std::size_t{ 0 });
	GYRO_CHECK(!serializer.Nodes()[0].IsTranslating());
	GYRO_CHECK(serializer.SceneWake() == Wake::Never());
	GYRO_CHECK(serializer.Republish() == Wake::Never());

	GYRO_CHECK(store.Find(window)->Translation.IsAtRest());
	GYRO_CHECK_EQ(store.Find(window)->Translation.Model(), Vector3<double>(400.0, 0.0, 0.0));

	// The published value is the model value, which is what makes the retirement invisible: the spring
	// was already inside the position threshold, so what a person sees move at this instant is less than
	// decision 54's snap to the device grid moves it a moment later.
	GYRO_CHECK_EQ(serializer.Nodes()[0].Transform.Translation, Vector3<double>(400.0, 0.0, 0.0));
}

GYRO_TEST(SceneSerializer, AnOutputlessSceneStillSettlesAndStagesNoSchedule)
{
	ManualClock clock;
	SceneStore store{ clock };

	const EntityId window = store.CreateContainer({}, {}).value();

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::FocusChange };

		GYRO_REQUIRE(commit.Fade(window, 0.0F));
	}

	SceneSerializer serializer;

	// A scene with nowhere to be drawn — a session whose outputs have not been assigned, a machine
	// between modesets. The schedule is empty because there is no output to owe anything to, and the
	// thresholds still have to be finite: an empty output set that produced a threshold of zero would
	// leave every channel unsettleable, which is the one direction that is not recoverable.
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.ActiveOpacities(), std::size_t{ 1 });
	GYRO_REQUIRE(serializer.Republish().Which == Wake::Kind::Timed);

	clock.Set(serializer.Republish().When);
	serializer.Serialize(store);

	GYRO_CHECK_EQ(serializer.ActiveOpacities(), std::size_t{ 0 });
	GYRO_CHECK(serializer.SceneWake() == Wake::Never());
}

// Docs/Decisions.md decision 114's two steps, seen from the side that performs the second one.

GYRO_TEST(SceneSerializer, ARetiringSubtreeIsPublishedUntilItHasFinishedAndThenIsGone)
{
	ManualClock clock;
	SceneStore store{ clock };

	const EntityId behind = store.CreateContainer({}, {}).value();
	const EntityId closing = store.CreateContainer({}, {}).value();
	const EntityId child = store.CreateImage(closing, Panel(100.0F, 40.0F), ImageContent{}).value();
	const SceneOutput outputs[] = { Primary() };

	store.SetOutputs(outputs);

	// The exit: something on the subtree is still moving when the author goes away, which is the case the
	// whole two-step shape exists for.
	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::FocusChange };

		GYRO_REQUIRE(commit.Fade(child, 0.0F));
		GYRO_REQUIRE(commit.Retire(closing));
	}

	GYRO_REQUIRE(store.Find(closing)->Retiring);
	GYRO_REQUIRE(store.Find(child)->Retiring);

	SceneSerializer serializer;

	// Still on screen, still drawn, still at the position it had — decision 114's retiring entity is in
	// the tree and not in a container somewhere else.
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(serializer.ActiveOpacities(), std::size_t{ 1 });

	const Wake owed = serializer.Republish();

	GYRO_REQUIRE(owed.Which == Wake::Kind::Timed);
	clock.Set(owed.When);

	// The pass the exit finishes on is the pass that publishes the scene without it. Swept before the
	// walk rather than after it, which is the difference between a closing window's last frame being the
	// end of its animation and being a still that stays up until something unrelated republishes.
	serializer.Serialize(store);

	GYRO_CHECK_EQ(serializer.Nodes().size(), std::size_t{ 1 });
	GYRO_CHECK(!store.IsLive(closing));
	GYRO_CHECK(!store.IsLive(child));

	// And the window behind it is still there, which is what the unlink is for: a chain whose links no
	// longer name live entities reads as a chain that ended.
	GYRO_CHECK(store.IsLive(behind));
	GYRO_CHECK(store.FirstRoot() == behind);
	GYRO_CHECK(store.Find(behind)->NextSibling.IsNull());
	GYRO_CHECK(serializer.SceneWake() == Wake::Never());
}

GYRO_TEST(SceneSerializer, ARetiringSubtreeWithNothingMovingLeavesOnTheNextPass)
{
	ManualClock clock;
	SceneStore store{ clock };

	const EntityId window = store.CreateContainer({}, {}).value();
	const SceneOutput outputs[] = { Primary() };

	store.SetOutputs(outputs);

	{
		SceneCommit commit{ store, CommitAuthor::Client };

		GYRO_REQUIRE(commit.Retire(window));
	}

	SceneSerializer serializer;

	// No exit catalog yet, so nothing is moving and the subtree has finished the instant it retired. The
	// rule is the same one the animating case obeys — free when every channel has settled — and it needs
	// no special case to mean *disappear now* before there is an animation to wait for.
	serializer.Serialize(store);

	GYRO_CHECK_EQ(serializer.Nodes().size(), std::size_t{ 0 });
	GYRO_CHECK(!store.IsLive(window));
	GYRO_CHECK_EQ(store.Count(), std::uint32_t{ 0 });
}

GYRO_TEST(SceneSerializer, OneChannelStillMovingKeepsTheWholeRetiringSubtreeAlive)
{
	ManualClock clock;
	SceneStore store{ clock };

	const EntityId window = store.CreateContainer({}, {}).value();
	const EntityId sliding = store.CreateImage(window, Panel(100.0F, 40.0F), ImageContent{}).value();
	const SceneOutput outputs[] = { Primary() };

	store.SetOutputs(outputs);

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::MatchedMove };

		GYRO_REQUIRE(commit.Move(sliding, { 900.0, 0.0, 0.0 }));
		GYRO_REQUIRE(commit.Retire(window));
	}

	SceneSerializer serializer;

	// The parent's own channels are at rest and it does not leave without the child: a window whose
	// opacity has finished must not vanish out from under a subsurface still sliding away.
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 2 });
	GYRO_CHECK(store.IsLive(window));

	clock.Set(serializer.Republish().When);
	serializer.Serialize(store);

	GYRO_CHECK_EQ(serializer.Nodes().size(), std::size_t{ 0 });
	GYRO_CHECK(!store.IsLive(window));
	GYRO_CHECK(!store.IsLive(sliding));
}

GYRO_TEST(SceneSerializer, AFreedPayloadIsReclaimedAndTheEntityThatMovedStillDrawsItsOwn)
{
	ManualClock clock;
	SceneStore store{ clock };

	const SceneOutput outputs[] = { Primary() };

	store.SetOutputs(outputs);

	// Three images, so that freeing the first moves the last into its hole rather than simply popping.
	const auto pixels = [](std::uint32_t texture) {
		ImageContent content{};
		content.Texture = TextureId{ texture };

		return content;
	};

	const EntityId first = store.CreateImage({}, Panel(10.0F, 10.0F), pixels(1)).value();
	const EntityId second = store.CreateImage({}, Panel(20.0F, 20.0F), pixels(2)).value();
	const EntityId third = store.CreateImage({}, Panel(30.0F, 30.0F), pixels(3)).value();

	GYRO_REQUIRE_EQ(store.Images().size(), std::size_t{ 3 });

	{
		SceneCommit commit{ store, CommitAuthor::Client };

		GYRO_REQUIRE(commit.Retire(first));
	}

	SceneSerializer serializer;

	serializer.Serialize(store);

	// The run shrank rather than keeping a hole — a session is windows opening and closing all day, and
	// a per-kind array that only grows is a leak with a slow clock on it.
	GYRO_CHECK(!store.IsLive(first));
	GYRO_CHECK_EQ(store.Images().size(), std::size_t{ 2 });

	// And the entity whose payload was moved to fill the hole still names its own pixels, which is the
	// one index the swap has to repair.
	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 2 });
	GYRO_CHECK(store.Images()[store.Find(second)->Content].Texture == TextureId{ 2 });
	GYRO_CHECK(store.Images()[store.Find(third)->Content].Texture == TextureId{ 3 });

	GYRO_CHECK(serializer.Nodes()[0].Content != serializer.Nodes()[1].Content);
}

// The transition, which is decision 188 on the authoring side: an output leaving one session for
// another carries the one it is leaving and a coefficient for how far through it is, and the pair is
// retired together when the fade lands. Every claim fails as somebody's desktop staying on a screen
// it was supposed to leave.
GYRO_TEST(SceneSerializer, AFadedReassignmentCrossesAsThePairAndACoefficient)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore store{ clock };

	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneOutput panel = Primary();
	panel.Session = static_cast<SessionId>(4);

	const SceneOutput one[] = { panel };
	store.SetOutputs(one);

	store.FadeOutputSession(panel.Id, static_cast<SessionId>(7), Motion::Gentle, clock.Now());

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const SceneAssignment> sessions = reader.Sessions<SceneAssignment>();

	GYRO_REQUIRE_EQ(sessions.size(), std::size_t{ 1 });

	// The session being moved to is what the output is showing from the first frame, which is what
	// makes input follow the transition rather than trail it.
	GYRO_CHECK(sessions[0].Shown == static_cast<SessionId>(7));
	GYRO_CHECK(sessions[0].Fading == static_cast<SessionId>(4));

	// **The fade is in the opacity run rather than a run of its own**, and it is the one coefficient in
	// a snapshot no node points at.
	GYRO_REQUIRE(sessions[0].Fade != NoCoefficient);

	const std::span<const Spring<float>> opacities = reader.Run<Spring<float>>(SnapshotRun::Opacity);

	GYRO_REQUIRE(sessions[0].Fade < opacities.size());
	GYRO_CHECK_EQ(opacities[sessions[0].Fade].Target, 0.0F);

	// And the loop is told to come back, because nothing else in this scene is moving.
	GYRO_CHECK(serializer.SceneWake() != Wake::Never());
}

// A transition is a contributor to the schedule like any other, and it is the *only* one on a screen
// with no animating window on it. Published without it, the frame thread comes back when something
// else happens to ask it to and the dissolve advances in jumps rather than smoothly.
GYRO_TEST(SceneSerializer, AFadeIsInEveryOutputsPublishedSchedule)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore store{ clock };

	// Nothing in the scene is moving: no window, no commit, no channel. The transition is the whole of
	// what the schedule can have in it, which is what makes this the test that fails on an ordering
	// mistake rather than one masked by a node happening to be animating at the same time.
	SceneOutput first = Primary();
	first.Session = static_cast<SessionId>(4);

	SceneOutput second = Primary();
	second.Id = OutputId{ 2, 1 };
	second.Bounds = { { 1920.0, 0.0 }, { 1920.0, 1080.0 } };

	const SceneOutput both[] = { first, second };
	store.SetOutputs(both);

	store.FadeOutputSession(first.Id, SessionId::None, Motion::Gentle, clock.Now());

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK(serializer.SceneWake().Which != Wake::Kind::Settled);

	const std::span<const Wake> wakes = reader.Wakes();

	GYRO_REQUIRE_EQ(wakes.size(), std::size_t{ 2 });

	// **Both of them, including the one the fade is not on.** The schedule is replicated rather than
	// partitioned (69) — dispatch has no screen-space bound to say which panels a contributor reaches
	// — so an idle monitor waking for its neighbour's animation is the known cost, and an output whose
	// own transition is missing from its own entry is the bug.
	GYRO_CHECK(wakes[0].Which != Wake::Kind::Settled);
	GYRO_CHECK(wakes[1].Which != Wake::Kind::Settled);
}

GYRO_TEST(SceneSerializer, AFadeThatHasLandedRetiresTheFadedSessionWithIt)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore store{ clock };

	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneOutput panel = Primary();
	panel.Session = static_cast<SessionId>(4);

	const SceneOutput one[] = { panel };
	store.SetOutputs(one);

	store.FadeOutputSession(panel.Id, static_cast<SessionId>(7), Motion::Gentle, clock.Now());

	SceneSerializer serializer;
	static_cast<void>(serializer.Serialize(store));

	// Well past anything the catalog runs for. The channel settles, and what has to go with it is the
	// outgoing session: left set, it would be a whole desktop composited at zero for ever — invisible,
	// paid for every frame, and still counted as being on the screen by everything that asks.
	clock.Advance(DurationFromSeconds(10.0));

	const SnapshotBuffer buffer = serializer.Serialize(store).Build(2);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const SceneAssignment> sessions = reader.Sessions<SceneAssignment>();

	GYRO_REQUIRE_EQ(sessions.size(), std::size_t{ 1 });
	GYRO_CHECK(sessions[0].Shown == static_cast<SessionId>(7));
	GYRO_CHECK(sessions[0].Fading == SessionId::None);
	GYRO_CHECK(sessions[0].Fade == NoCoefficient);

	// Which is the same statement as the world having stopped: a settled transition owes no frame.
	GYRO_CHECK(serializer.SceneWake() == Wake::Never());
}

// The cut, which is decision 188's base case rather than a second mode: a reassignment with no
// coefficient. Suspend takes it, because decision 59 needs the locked state on the glass at the next
// flip and not one fade later.
GYRO_TEST(SceneSerializer, AnImmediateReassignmentCarriesNoCoefficientAndNothingFading)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore store{ clock };

	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneOutput panel = Primary();
	panel.Session = static_cast<SessionId>(4);

	const SceneOutput one[] = { panel };
	store.SetOutputs(one);

	// Mid fade, and then the cut lands on top of it — which is a laptop lid closing while the lock
	// animation is still running. What is on the glass at the next flip is the locked screen.
	store.FadeOutputSession(panel.Id, static_cast<SessionId>(7), Motion::Gentle, clock.Now());
	store.SetOutputSession(panel.Id, static_cast<SessionId>(7));

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const SceneAssignment> sessions = reader.Sessions<SceneAssignment>();

	GYRO_REQUIRE_EQ(sessions.size(), std::size_t{ 1 });
	GYRO_CHECK(sessions[0].Shown == static_cast<SessionId>(7));
	GYRO_CHECK(sessions[0].Fading == SessionId::None);
	GYRO_CHECK(sessions[0].Fade == NoCoefficient);
	GYRO_CHECK(serializer.SceneWake() == Wake::Never());
}

// Decision 20's exit pixels, and decision 46's atlas, crossing the waist. What the frame thread gets
// is a position rather than a rectangle, because decision 190 puts a window that straddles a seam in
// both screens' atlases and there is no single rectangle to give it.

namespace
{
// A thousand logical square, at a given scale, sitting where it is put along x.
[[nodiscard]] SceneOutput Screen(OutputId id, double left, std::int32_t scale)
{
	return { .Id = id,
		     .Bounds = { { left, 0.0 }, { 1000.0, 1000.0 } },
		     .Density = Scale::FromInteger(scale),
		     .Grid = { 1000 * scale, 1000 * scale } };
}
} // namespace

GYRO_TEST(SceneSerializer, AClosingWindowCarriesOneRectanglePerScreenItIsOn)
{
	ManualClock clock;
	SceneStore store{ clock };

	const OutputId panel{ 1, 1 };
	const OutputId monitor{ 2, 1 };
	const SceneOutput outputs[] = { Screen(panel, 0.0, 1), Screen(monitor, 1000.0, 2) };

	store.SetOutputs(outputs);

	// Four hundred logical wide, across the boundary at x = 1000 — so half of it is on each screen and
	// each screen keeps the whole of it, at its own scale.
	const EntityId window =
		store.CreateContainer({}, { .Position = { 800.0, 100.0, 0.0 }, .Extent = { 400.0F, 300.0F } }).value();

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::WindowClose };

		GYRO_REQUIRE(commit.Fade(window, 0.0F));
		GYRO_REQUIRE(commit.Retire(window));
	}

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 1 });
	GYRO_REQUIRE_EQ(serializer.Exits().size(), std::size_t{ 2 });

	const Node& node = serializer.Nodes()[0];

	GYRO_REQUIRE(node.IsExiting());
	GYRO_CHECK_EQ(node.Exit, std::uint32_t{ 0 });

	// Contiguous and in output order, which is the whole of what a node's slot promises: the run from
	// it up to the first entry naming somebody else is this node's.
	const std::span<const ExitSnapshot> exits = serializer.Exits();

	GYRO_CHECK_EQ(exits[0].Node, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(exits[1].Node, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(exits[0].Output, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(exits[1].Output, std::uint32_t{ 1 });

	// The same window in each screen's own texels — 400x300 logical is 400x300 at scale 1 and 800x600
	// at scale 2. A single rectangle for both would have to be resampled onto one of them, and a window
	// resampled while it fades is a picture that goes soft on the way out.
	GYRO_CHECK(exits[0].Slot.Extent == PixelSize<BufferSpace>{ 400, 300 });
	GYRO_CHECK(exits[1].Slot.Extent == PixelSize<BufferSpace>{ 800, 600 });

	// This store has no texture space attached, so neither screen has an atlas image to be copied
	// into — which is decision 46's exhaustion answer arriving as an absence, and what a person sees on
	// such a machine is a window that cuts instead of fading.
	GYRO_CHECK(exits[0].Texture.IsNull());
	GYRO_CHECK(exits[1].Texture.IsNull());

	// **Two rectangles, two counts.** The frame thread copies into each screen's atlas on that screen's
	// own frame, so a straddling window naming one count on both would be captured on whichever screen
	// composited first and left empty on the other — the artefact decision 190 splits the snapshot to
	// avoid, arriving through the bookkeeping instead of the storage.
	GYRO_CHECK(exits[0].Reservation != 0 && exits[1].Reservation != 0);
	GYRO_CHECK(exits[0].Reservation != exits[1].Reservation);
}

GYRO_TEST(SceneSerializer, AWindowNobodyIsClosingKeepsNoPixelsAnywhere)
{
	ManualClock clock;
	SceneStore store{ clock };

	const OutputId panel{ 1, 1 };
	const SceneOutput outputs[] = { Screen(panel, 0.0, 1) };

	store.SetOutputs(outputs);

	const EntityId window = store.CreateContainer({}, { .Extent = { 400.0F, 300.0F } }).value();

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 1 });
	GYRO_CHECK(!serializer.Nodes()[0].IsExiting());
	GYRO_CHECK(serializer.Exits().empty());
	GYRO_CHECK(store.Atlases().IsEmpty());
	GYRO_CHECK(store.IsLive(window));
}

GYRO_TEST(SceneSerializer, TheRectangleIsGivenBackOnThePassTheExitFinishes)
{
	ManualClock clock;
	SceneStore store{ clock };

	const OutputId panel{ 1, 1 };
	const SceneOutput outputs[] = { Screen(panel, 0.0, 1) };

	store.SetOutputs(outputs);

	const EntityId window = store.CreateContainer({}, { .Extent = { 400.0F, 300.0F } }).value();

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{}, Transition::WindowClose };

		GYRO_REQUIRE(commit.Fade(window, 0.0F));
		GYRO_REQUIRE(commit.Retire(window));
	}

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Exits().size(), std::size_t{ 1 });

	const Wake owed = serializer.Republish();

	GYRO_REQUIRE(owed.Which == Wake::Kind::Timed);
	clock.Set(owed.When);

	// The pass that publishes the scene without the window is the pass that gives its rectangle back.
	// Held past that, an atlas sized for what is leaving would fill up with windows that have finished
	// leaving, and the exit that then found no room would cut instead of fading.
	serializer.Serialize(store);

	GYRO_CHECK(!store.IsLive(window));
	GYRO_CHECK(serializer.Exits().empty());
	GYRO_CHECK(store.Atlases().IsEmpty());
}
