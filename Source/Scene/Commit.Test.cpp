#include "Scene/Commit.h"

#include <cmath>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Animation/Solve/Spring.h"
#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Core/Time.h"
#include "Geometry/NodeTransform.h"
#include "Scene/Entity.h"
#include "Scene/Store.h"
#include "Testing/Test.h"

// Docs/Decisions.md decision 112's scope, and decision 89's phase one through it.
//
// What a commit *means* — that a retarget picks up the position and velocity of a motion in progress —
// is asserted in Source/Integration/SceneCommit.Test.cpp, on published bytes, because the claim is
// about a spring that crossed. What is checked here is the scope itself: who may open one, what a write
// with no origin behind it does, and the one channel whose retarget is not the retarget every other
// channel gets.

namespace
{
constexpr Instant Origin = Monotonic::FromNanoseconds(1'000'000'000);

[[nodiscard]] EntityId Only(SceneStore& store)
{
	return store.CreateContainer({}, {}).value();
}

// Two quaternions naming the same rotation, to within what a float chart round trip costs. The sign is
// free — decision 55's transform reads a rotation and not a quaternion — so the comparison is on the
// magnitude of the dot product.
[[nodiscard]] bool Same(Quaternion left, Quaternion right) noexcept
{
	return std::abs(Dot(left, right)) > 1.0F - 1e-5F;
}
} // namespace

GYRO_TEST(SceneCommit, AWriteWithNoOriginBehindItIsRefusedRatherThanStampedWithNow)
{
	ManualClock clock{ Origin };
	SceneStore store{ clock };

	const EntityId node = Only(store);

	// The client shape: `wl_surface.commit` carries no timestamp, and nothing a client authors is a
	// sprung channel. An extent and an immediate placement are exactly what it does write.
	SceneCommit commit{ store, CommitAuthor::Client };

	GYRO_REQUIRE(commit.IsOpen());
	GYRO_CHECK(!commit.Origin().has_value());
	GYRO_CHECK(commit.Author() == CommitAuthor::Client);

	GYRO_CHECK(commit.Resize(node, { 640.0F, 480.0F }));
	GYRO_CHECK(commit.Move(node, { 40.0, 0.0, 0.0 }, Immediate()));

	// And a motion is refused rather than given the dispatch thread's own now, which would add a frame
	// of lag to whatever it started, invisibly, on the axis a person judges most harshly.
	GYRO_CHECK(!commit.Fade(node, 0.0F, Animate(Motion::Standard)));

	const Entity& entity = *store.Find(node);

	GYRO_CHECK_EQ(entity.Extent, Size<SurfaceSpace, float>{ 640.0F, 480.0F });
	GYRO_CHECK_EQ(entity.Translation.Model(), Vector3<double>(40.0, 0.0, 0.0));
	GYRO_CHECK(entity.Translation.IsAtRest());
	GYRO_CHECK(entity.Opacity.IsAtRest());
	GYRO_CHECK_EQ(entity.Opacity.Model(), 1.0F);
}

GYRO_TEST(SceneCommit, AnOriginFromTheFutureIsClampedToDispatchsOwnNow)
{
	ManualClock clock{ Origin };
	SceneStore store{ clock };

	const EntityId node = Only(store);

	// A stamp from the future is not a late start, it is a motion that stands still until the clock
	// catches up: the closed form evaluated before its origin is the state it began in, so a window
	// dragged under a bad timestamp would sit still for as long as the stamp was wrong.
	{
		SceneCommit ahead{ store, CommitAuthor::Shell, Advanced(Origin, Duration{ 10'000'000'000 }) };

		GYRO_CHECK(ahead.Origin() == Origin);
		GYRO_CHECK(ahead.Fade(node, 0.0F, Animate(Motion::Standard)));
	}

	GYRO_CHECK(store.Find(node)->Opacity.Coefficients().Origin == Origin);

	// The backward direction is self-limiting and is left alone: a stale origin reads as a motion that
	// has already finished, and a decaying exponential evaluated far along is settled rather than wrong.
	const Instant stale = Monotonic::FromNanoseconds(1'000'000);

	{
		SceneCommit late{ store, CommitAuthor::Shell, stale };

		GYRO_CHECK(late.Origin() == stale);
		GYRO_CHECK(late.Fade(node, 1.0F, Animate(Motion::Standard)));
	}

	GYRO_CHECK(store.Find(node)->Opacity.Coefficients().Origin == stale);
}

GYRO_TEST(SceneCommit, ACommitOpenedInsideAnotherRefusesEveryWrite)
{
	ManualClock clock{ Origin };
	SceneStore store{ clock };

	const EntityId node = Only(store);

	SceneCommit outer{ store, CommitAuthor::Shell, Origin };

	GYRO_REQUIRE(outer.IsOpen());

	{
		// Commits do not nest: the double buffering Wayland requires resolves in `Protocol` before
		// anything reaches the store, so a second scope inside this one is a bug at a call site. It
		// refuses rather than borrowing an origin that belongs to a different event.
		SceneCommit inner{ store, CommitAuthor::Shell, Advanced(Origin, Duration{ 8'000'000 }) };

		GYRO_CHECK(!inner.IsOpen());
		GYRO_CHECK(!inner.Fade(node, 0.0F, Animate(Motion::Standard)));
		GYRO_CHECK(!inner.Resize(node, { 10.0F, 10.0F }));
	}

	// And closing it did not close the scope around it, which is the failure that would make the next
	// write in the outer body a motion with no transaction around it.
	GYRO_CHECK(outer.IsOpen());
	GYRO_CHECK(outer.Fade(node, 0.0F, Animate(Motion::Standard)));
	GYRO_CHECK(store.Find(node)->Opacity.Coefficients().Origin == Origin);
}

GYRO_TEST(SceneCommit, AStaleIdIsARefusalAndAnAbsentChannelIsNot)
{
	ManualClock clock{ Origin };
	SceneStore store{ clock };

	const EntityId node = Only(store);

	SceneCommit commit{ store, CommitAuthor::Shell, Origin };

	GYRO_CHECK(!commit.Move(EntityId{ 7, 2 }, { 1.0, 0.0, 0.0 }, Animate(Motion::Standard)));
	GYRO_CHECK(!commit.Resize(EntityId{ 7, 2 }, { 10.0F, 10.0F }));

	// A default-constructed disposition is decision 89's *absent*: not part of this transition, so
	// whatever the channel was doing continues. It is a write that says nothing rather than one that
	// failed, which is what lets a caller hand a whole `ChannelTable` through without filtering it.
	GYRO_CHECK(commit.Fade(node, 0.0F, ChannelMotion{}));
	GYRO_CHECK(commit.Move(node, { 99.0, 0.0, 0.0 }, ChannelMotion{}));

	GYRO_CHECK_EQ(store.Find(node)->Opacity.Model(), 1.0F);
	GYRO_CHECK_EQ(store.Find(node)->Translation.Model(), Vector3<double>(0.0, 0.0, 0.0));
}

GYRO_TEST(SceneCommit, AnImmediateWriteStopsTheMotionRatherThanRetargetingIt)
{
	ManualClock clock{ Origin };
	SceneStore store{ clock };

	const EntityId node = Only(store);

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Origin };

		GYRO_REQUIRE(commit.Move(node, { 400.0, 0.0, 0.0 }, Animate(Motion::Standard)));
	}

	GYRO_REQUIRE(!store.Find(node)->Translation.IsAtRest());

	// Decision 68's subsurface and decision 112's client-driven resize both land here: the value is set
	// and no motion is started, which is what a spring between a video player's controls and the screen
	// would break.
	{
		SceneCommit commit{ store, CommitAuthor::Client };

		GYRO_REQUIRE(commit.Move(node, { 400.0, 0.0, 0.0 }, Immediate()));
	}

	GYRO_CHECK(store.Find(node)->Translation.IsAtRest());
	GYRO_CHECK_EQ(store.Find(node)->Translation.Model(), Vector3<double>(400.0, 0.0, 0.0));
}

GYRO_TEST(SceneCommit, TheScenesPacingIsWhatAMotionResolvesThrough)
{
	ManualClock clock{ Origin };
	SceneStore store{ clock };

	const EntityId node = Only(store);

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Origin };

		GYRO_REQUIRE(commit.Fade(node, 0.0F, Animate(Motion::Standard)));
	}

	const float authored = store.Find(node)->Opacity.Coefficients().Parameters.Frequency;

	// A person who has asked for faster motion has asked for all of it, so the table is the scene's and
	// not a call site's. Twice the speed is half the response, which is twice the frequency.
	store.SetMotions(MotionTable{}, { .Speed = 2.0 });

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Origin };

		GYRO_REQUIRE(commit.Fade(node, 1.0F, Animate(Motion::Standard)));
	}

	GYRO_CHECK_EQ(store.Find(node)->Opacity.Coefficients().Parameters.Frequency, authored * 2.0F);
}

GYRO_TEST(SceneCommit, TwoWritesAtOneOriginAreIdenticalInEveryRegime)
{
	ManualClock clock{ Origin };
	SceneStore store{ clock };

	const EntityId node = Only(store);

	// The catalog is critical or underdamped throughout, and the exact idempotence decision 89 claims is
	// asserted on that path in Source/Integration/SceneCommit.Test.cpp. Configuration can reach the third
	// regime, where the closed form recovers its own initial offset from two coefficients and lands a
	// unit in the last place away — so a property written at device rate would drift once per event.
	store.SetMotions(MotionTable{ .Standard = { .Response = 0.4, .Damping = 4.0 } });

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Origin };

		GYRO_REQUIRE(commit.Fade(node, 0.0F, Animate(Motion::Standard)));
	}

	const Instant interrupted = Advanced(Origin, Duration{ 8'000'000 });

	clock.Set(interrupted);

	GYRO_REQUIRE(store.Find(node)->Opacity.Coefficients().Regime() == SpringRegime::Overdamped);

	{
		SceneCommit commit{ store, CommitAuthor::Shell, interrupted };

		GYRO_REQUIRE(commit.Fade(node, 0.4F, Animate(Motion::Standard)));
	}

	const Spring<float> once = store.Find(node)->Opacity.Coefficients();

	{
		SceneCommit commit{ store, CommitAuthor::Shell, interrupted };

		GYRO_REQUIRE(commit.Fade(node, 0.4F, Animate(Motion::Standard)));
	}

	const Spring<float> twice = store.Find(node)->Opacity.Coefficients();

	GYRO_CHECK(once.Origin == twice.Origin);
	GYRO_CHECK_EQ(once.Target, twice.Target);
	GYRO_CHECK_EQ(once.Offset, twice.Offset);
	GYRO_CHECK_EQ(once.Velocity, twice.Velocity);
}

GYRO_TEST(SceneCommit, ARotationRetargetMovesTheChartAndCarriesTheVelocityIntoIt)
{
	ManualClock clock{ Origin };
	SceneStore store{ clock };

	const EntityId node = Only(store);

	const Quaternion quarter = Quaternion::FromAxisAngle({ 0.0F, 0.0F, 1.0F }, 1.5707963F);
	// About a different axis, so that the new deviation is not parallel to the velocity the old chart
	// holds. A retarget along the axis already turning is the one case where the transport is the
	// identity, and testing that case would assert nothing.
	const Quaternion sideways = Quaternion::FromAxisAngle({ 1.0F, 0.0F, 0.0F }, 1.5707963F);

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Origin };

		GYRO_REQUIRE(commit.Turn(node, quarter, Animate(Motion::Standard)));
	}

	// Sixty milliseconds later, with dispatch's clock where it would be: a commit's origin is the event's
	// timestamp, and the event happened at or before the thread got to it.
	const Instant interrupted = Advanced(Origin, Duration{ 60'000'000 });

	clock.Set(interrupted);

	const Entity& entity = *store.Find(node);
	const SpringState<RotationVector> before = entity.Turn.PresentationState(interrupted);
	const Quaternion turned = Quaternion::FromDeviation(before.Position, entity.Orientation);

	GYRO_REQUIRE(!entity.Turn.IsAtRest());

	{
		SceneCommit commit{ store, CommitAuthor::Shell, interrupted };

		GYRO_REQUIRE(commit.Turn(node, sideways, Animate(Motion::Standard)));
	}

	// The orientation the frame side reconstructs does not jump: the deviation is re-expressed against
	// the new base point, so the same turn is under way with a different destination.
	const SpringState<RotationVector> after = entity.Turn.PresentationState(interrupted);

	GYRO_CHECK(entity.Orientation == sideways);
	GYRO_CHECK(Same(Quaternion::FromDeviation(after.Position, entity.Orientation), turned));

	// And the velocity was transported rather than copied. Copying it is the defect
	// `Geometry/NodeTransform.h` names: the chart change is first order in the change of deviation, so a
	// retarget across a right angle sends the angular velocity tens of degrees off course, which reads
	// as a window that changes direction when it is interrupted.
	GYRO_CHECK(after.Velocity != before.Velocity);

	// A retarget that does not move the base point is the flat case, exactly. Two of them at one origin
	// are bit-identical, which is what routing an unchanged chart through the transport would cost.
	{
		SceneCommit commit{ store, CommitAuthor::Shell, interrupted };

		GYRO_REQUIRE(commit.Turn(node, sideways, Animate(Motion::Standard)));
	}

	const SpringState<RotationVector> again = entity.Turn.PresentationState(interrupted);

	GYRO_CHECK_EQ(again.Position, after.Position);
	GYRO_CHECK_EQ(again.Velocity, after.Velocity);
}
