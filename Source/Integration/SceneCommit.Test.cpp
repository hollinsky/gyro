#include <cstddef>
#include <cstdint>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Catalog.h"
#include "Animation/Author/Motion.h"
#include "Animation/Solve/Spring.h"
#include "Core/Clock.h"
#include "Core/Time.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Scale.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Reader/Reader.h"
#include "Publication/Snapshot.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Node.h"

// Docs/Decisions.md decision 89's own motivating case, run through the bytes.
//
// A window opens at `t₀₁`; eight milliseconds later, inside the same frame, a second commit focuses and
// moves it. That is ordinary against 1000 Hz input and a 60 Hz panel, and it is the case that decides
// *when* a mutation becomes motion. Resolved once per frame, the second target wins and the opening
// never existed — the window appears already half open and jumps. Resolved at the write, the second is
// a retarget from the true position and velocity of an open eight milliseconds in progress, and what a
// person sees is one continuous motion that changed its mind.
//
// **It is here rather than in Source/Scene because the claim is about a channel that crossed.** The
// spring the store retargeted is only interesting if it is the spring the frame side will evaluate, so
// every assertion below reads a published snapshot and evaluates it with `Animation/Solve`, which is
// the arithmetic the frame thread does and no part of the authoring side. Naming `Publication` and
// `Animation` in one file is what no module may do, so it lives where the composition root's tests do.
//
// **Publication is naive here on purpose**, which is Docs/Open.md's pacing entry asking for the first
// cut to be the eager one: each commit is followed by a full re-serialisation. The one case that would
// make that visible is asserted rather than assumed — coalescing the two commits into a single
// publication loses nothing, because the first commit's motion is already inside the position and
// velocity the second's retarget sampled.

namespace
{
constexpr Instant Opening = Monotonic::FromNanoseconds(1'000'000'000);
constexpr Duration Gap = Duration{ 8'000'000 };
constexpr Instant Focus = Advanced(Opening, Gap);

// The scene's clock, parked after both origins so that neither is clamped. A commit's `t₀` is never
// later than dispatch's own now — decision 112's guard against a stamp from the future, which is
// exercised where it belongs, in Source/Scene/Commit.Test.cpp — and an event is always something that
// already happened by the time the thread reaches it.
ManualClock Clock{ Monotonic::FromNanoseconds(2'000'000'000) };

// What the window is when it is summoned and before anything animates it: off-centre, small, and
// invisible. `Scene/Entity.h` constructs these at rest, so the open below has something to move from.
[[nodiscard]] NodeProperties Summoned()
{
	return {
		.Position = { 400.0, 300.0, 0.0 },
		.Scale = { 0.6F, 0.6F, 1.0F },
		.Extent = { 800.0F, 600.0F },
		.Opacity = 0.0F,
	};
}

struct Window
{
	SceneStore Store{ Clock };
	EntityId Node{};
};

[[nodiscard]] Window Summon()
{
	Window window;

	window.Node = window.Store.CreateContainer({}, Summoned()).value();

	const SceneOutput outputs[] = { SceneOutput{ .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	window.Store.SetOutputs(outputs);

	return window;
}

// The open, as the catalog states it: scale and opacity, under two different motions.
void Open(SceneStore& store, EntityId node, Instant origin)
{
	const ChannelTable channels = Channels(Definition(Transition::WindowOpen), MotionPolicy::Ordinary);

	SceneCommit commit{ store, CommitAuthor::Shell, origin };

	GYRO_REQUIRE(commit.Scale(node, { 1.0F, 1.0F, 1.0F }, channels.Scale));
	GYRO_REQUIRE(commit.Fade(node, 1.0F, channels.Opacity));
}

// Focus moving away from this window and the window moving with it: the second event, one commit.
//
// The scale channel is written by neither, which is decision 89's third case and the one that is easy
// to lose — a channel no transition names keeps doing what it was doing, so the open's scale spring has
// to come through this commit untouched rather than restarted at the new origin.
void FocusElsewhere(SceneStore& store, EntityId node, Instant origin)
{
	const ChannelTable dim = Channels(Definition(Transition::FocusChange), MotionPolicy::Ordinary);
	const ChannelTable move = Channels(Definition(Transition::MatchedMove), MotionPolicy::Ordinary);

	SceneCommit commit{ store, CommitAuthor::Shell, origin };

	GYRO_REQUIRE(commit.Fade(node, 0.7F, dim.Opacity));
	GYRO_REQUIRE(commit.Move(node, { 900.0, 300.0, 0.0 }, move.Translation));
}

// What crossed, as the frame side would find it: the one node, and the coefficient each channel names.
struct Published
{
	SnapshotBuffer Buffer;
	SnapshotReader Reader;

	[[nodiscard]] const Node& Record() const noexcept { return Reader.Nodes<Node>()[0]; }

	template<typename T>
	[[nodiscard]] T Coefficients(SnapshotRun which, std::uint32_t at) const noexcept
	{
		return Reader.Run<T>(which)[at];
	}
};

[[nodiscard]] Published Publish(SceneSerializer& serializer, const SceneStore& store, std::uint64_t sequence)
{
	Published published{ .Buffer = serializer.Serialize(store).Build(sequence), .Reader = {} };

	published.Reader = SnapshotReader{ published.Buffer.Bytes() };

	return published;
}

// Field by field rather than through an operator, because what is being asserted is that two springs
// are the *same* spring — decision 89 claims exact idempotence, and a comparison with any tolerance in
// it would pass on a spring that had quietly drifted.
template<typename V>
[[nodiscard]] bool Same(const Spring<V>& left, const Spring<V>& right) noexcept
{
	return left.Origin == right.Origin && left.Parameters == right.Parameters && left.Target == right.Target &&
	       left.Offset == right.Offset && left.Velocity == right.Velocity;
}
} // namespace

GYRO_TEST(SceneCommit, TheSecondCommitRetargetsTheOpenRatherThanReplacingIt)
{
	Window window = Summon();
	SceneSerializer serializer;

	Open(window.Store, window.Node, Opening);

	const Published opening = Publish(serializer, window.Store, 1);

	GYRO_REQUIRE(opening.Reader.IsValid());
	GYRO_REQUIRE_EQ(opening.Reader.Nodes<Node>().size(), std::size_t{ 1 });

	// The active set is derived at publication and never maintained (decision 16): the two channels the
	// open moves name a coefficient, and the two it does not carry their value inline and nothing else.
	GYRO_REQUIRE(opening.Record().IsScaling());
	GYRO_REQUIRE(opening.Record().IsFading());
	GYRO_CHECK(!opening.Record().IsTranslating());
	GYRO_CHECK_EQ(opening.Record().Transform.Translation, Vector3<double>(400.0, 300.0, 0.0));

	const Spring<float> opacityAtOpen = opening.Coefficients<Spring<float>>(SnapshotRun::Opacity, 0);
	const Spring<Vector3<float>> scaleAtOpen = opening.Coefficients<Spring<Vector3<float>>>(SnapshotRun::Scale, 0);

	GYRO_CHECK(opacityAtOpen.Origin == Opening);

	// Eight milliseconds in, the open is genuinely in flight: partway there and still moving. Both halves
	// are what the retarget below has to inherit, and asserting them first is what stops the continuity
	// check from being satisfied by a channel that had already settled.
	const SpringState<float> inFlight = opacityAtOpen.Evaluate(Focus);

	GYRO_REQUIRE(inFlight.Position > 0.0F && inFlight.Position < 1.0F);
	GYRO_REQUIRE(inFlight.Velocity > 0.0F);

	FocusElsewhere(window.Store, window.Node, Focus);

	const Published focused = Publish(serializer, window.Store, 2);

	GYRO_REQUIRE(focused.Reader.IsValid());
	GYRO_REQUIRE(focused.Record().IsFading());
	GYRO_REQUIRE(focused.Record().IsTranslating());
	GYRO_REQUIRE(focused.Record().IsScaling());

	const Spring<float> opacityAtFocus = focused.Coefficients<Spring<float>>(SnapshotRun::Opacity, 0);

	// The retarget, stated as the two things that make it one: it starts at the second event's instant,
	// and at that instant it is exactly where the first motion had got to, going exactly as fast. A
	// replacement would start from the model value the open was heading for, or from where the open
	// began; either one is a jump on screen and neither is expressible as a difference of targets.
	GYRO_CHECK(opacityAtFocus.Origin == Focus);
	GYRO_CHECK_EQ(opacityAtFocus.Target, 0.7F);

	const SpringState<float> retargeted = opacityAtFocus.Evaluate(Focus);

	GYRO_CHECK_EQ(retargeted.Position, inFlight.Position);
	GYRO_CHECK_EQ(retargeted.Velocity, inFlight.Velocity);

	// The commit's origin is shared rather than read per write, so the channel this commit started and
	// the channel it interrupted have one instant between them whatever order the body wrote them in.
	GYRO_CHECK(focused.Coefficients<Spring<Vector3<double>>>(SnapshotRun::Translation, 0).Origin == Focus);

	// And the channel neither transition named came through untouched. The window is still growing under
	// the open's own spring, which is what *whatever the channel was doing continues* has to mean once
	// the scene has been serialised twice.
	GYRO_CHECK(Same(focused.Coefficients<Spring<Vector3<float>>>(SnapshotRun::Scale, 0), scaleAtOpen));
}

GYRO_TEST(SceneCommit, ResolvingOncePerFrameWouldHaveThrownTheOpeningAway)
{
	Window eager = Summon();
	Window summarised = Summon();

	Open(eager.Store, eager.Node, Opening);
	FocusElsewhere(eager.Store, eager.Node, Focus);

	// The rejected shape, which is decision 14 read literally: the frame's dirty set resolved once, at
	// the last origin it saw. Both mutations touch opacity, so the second target is the only one that
	// survives and the open starts where the focus starts.
	{
		SceneCommit commit{ summarised.Store, CommitAuthor::Shell, Focus };

		GYRO_REQUIRE(commit.Scale(summarised.Node, { 1.0F, 1.0F, 1.0F }, Animate(Motion::Standard)));
		GYRO_REQUIRE(commit.Fade(summarised.Node, 0.7F, Animate(Motion::Gentle)));
		GYRO_REQUIRE(commit.Move(summarised.Node, { 900.0, 300.0, 0.0 }, Animate(Motion::Snappy)));
	}

	SceneSerializer serializer;

	const Published fromEager = Publish(serializer, eager.Store, 1);
	const Spring<float> eagerOpacity = fromEager.Coefficients<Spring<float>>(SnapshotRun::Opacity, 0);

	const Published fromSummary = Publish(serializer, summarised.Store, 2);
	const Spring<float> summarisedOpacity = fromSummary.Coefficients<Spring<float>>(SnapshotRun::Opacity, 0);

	// At the instant the second event lands, the eager window is part of the way open and moving; the
	// summarised one is exactly as invisible and as still as it was when it was summoned. The eight
	// milliseconds of opening did not happen slightly differently — they did not happen.
	GYRO_CHECK_EQ(summarisedOpacity.Evaluate(Focus).Position, 0.0F);
	GYRO_CHECK_EQ(summarisedOpacity.Evaluate(Focus).Velocity, 0.0F);
	GYRO_CHECK(eagerOpacity.Evaluate(Focus).Position > summarisedOpacity.Evaluate(Focus).Position);
	GYRO_CHECK(eagerOpacity.Evaluate(Focus).Velocity > 0.0F);

	// And the difference outlives the instant it was created at, which is what makes it something a
	// person sees rather than a discontinuity of a single frame.
	const Instant later = Advanced(Focus, Duration{ 16'000'000 });

	GYRO_CHECK(eagerOpacity.Evaluate(later).Position > summarisedOpacity.Evaluate(later).Position);
}

GYRO_TEST(SceneCommit, TwoRetargetsAtOneOriginAreExactlyIdempotent)
{
	Window window = Summon();
	SceneSerializer serializer;

	Open(window.Store, window.Node, Opening);
	FocusElsewhere(window.Store, window.Node, Focus);

	const Published once = Publish(serializer, window.Store, 1);
	const Spring<float> after = once.Coefficients<Spring<float>>(SnapshotRun::Opacity, 0);
	const Spring<Vector3<double>> moved = once.Coefficients<Spring<Vector3<double>>>(SnapshotRun::Translation, 0);

	// The same write again, at the same origin. The second samples the first's spring at the instant that
	// spring started, which reads back the position and velocity it was constructed from — so a repeated
	// write inside one commit is arithmetic rather than motion, and this is the claim decision 89 makes
	// that a rounding error would falsify.
	{
		SceneCommit commit{ window.Store, CommitAuthor::Shell, Focus };

		GYRO_REQUIRE(commit.Fade(window.Node, 0.7F, Animate(Motion::Gentle)));
		GYRO_REQUIRE(commit.Move(window.Node, { 900.0, 300.0, 0.0 }, Animate(Motion::Snappy)));
	}

	const Published twice = Publish(serializer, window.Store, 2);

	GYRO_CHECK(Same(twice.Coefficients<Spring<float>>(SnapshotRun::Opacity, 0), after));
	GYRO_CHECK(Same(twice.Coefficients<Spring<Vector3<double>>>(SnapshotRun::Translation, 0), moved));

	// The same property, written twice to different targets inside one commit. The last target wins and
	// nothing of the ones before it survives — which is what lets a commit body be written in whatever
	// order a caller finds natural, and is the same arithmetic seen from the other side.
	Window overwritten = Summon();

	Open(overwritten.Store, overwritten.Node, Opening);

	{
		SceneCommit commit{ overwritten.Store, CommitAuthor::Shell, Focus };

		GYRO_REQUIRE(commit.Fade(overwritten.Node, 0.2F, Animate(Motion::Gentle)));
		GYRO_REQUIRE(commit.Fade(overwritten.Node, 0.5F, Animate(Motion::Gentle)));
		GYRO_REQUIRE(commit.Fade(overwritten.Node, 0.7F, Animate(Motion::Gentle)));
		GYRO_REQUIRE(commit.Move(overwritten.Node, { 900.0, 300.0, 0.0 }, Animate(Motion::Snappy)));
	}

	const Published last = Publish(serializer, overwritten.Store, 3);

	GYRO_CHECK(Same(last.Coefficients<Spring<float>>(SnapshotRun::Opacity, 0), after));
	GYRO_CHECK(Same(last.Coefficients<Spring<Vector3<double>>>(SnapshotRun::Translation, 0), moved));
}
