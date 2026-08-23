#include "Dispatch/Loop.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Gym/Gym.h"
#include "Publication/Reader/Reader.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Scene/Output.h"
#include "Testing/Test.h"
#include "World/Node.h"

// What the dispatch step promises the composition root: it publishes what the author wrote, it says
// when to come back, and it never loses a scene to a frame thread that has fallen behind.
//
// The three claims below are the three the design turns on, and each of them fails silently if it is
// wrong. A scene that does not cross leaves a compositor drawing nothing while every counter says it
// is working. A fold that never reaches `Never()` is a machine that cannot sleep. And a publish
// dropped on a full ring is the one Publication/Ring.h refuses to accept — harmless on every frame
// except the last one before a scene quiesces, which is the frame that then stays on screen forever.
//
// **The frame thread is played by hand here**, and that is the right vantage rather than a shortcut:
// what this file is testing is the producer's behaviour against a consumer that is *late*, and a real
// frame loop cannot be asked to fall four snapshots behind on demand. Source/Integration is where the
// two halves meet for real.

namespace
{
constexpr Instant Start = Monotonic::FromNanoseconds(1'000'000'000);

// Reaching this means the loop kept asking to be called back with nothing left to settle — a livelock
// to report rather than a run to wait out. Far past the handful of edges a settling gym schedules.
constexpr std::size_t StepLimit = 4096;

[[nodiscard]] SceneOutput Panel()
{
	return SceneOutput{ .Bounds = { {}, { 1920.0, 1080.0 } },
		                .Density = Scale::FromInteger(1),
		                .Grid = PixelSize<DeviceSpace>{ 1920, 1080 } };
}

// The whole dispatch side, with the two channels it writes through held beside it. The channels are
// members rather than locals because the loop holds them by reference for its life, which is the same
// arrangement the composition root has.
struct Fixture
{
	ManualClock Clock{ Start };
	SnapshotRing Ring;
	ReturnChannel Returns;
	DispatchLoop Loop{ Clock, Ring, Returns };

	[[nodiscard]] Result<void> Open(std::string_view gym)
	{
		Result<std::unique_ptr<IGym>> author = MakeGym(gym);

		if (!author)
		{
			return std::unexpected{ author.error() };
		}

		const SceneOutput outputs[] = { Panel() };

		return Loop.Open(std::move(*author), outputs);
	}

	// The frame thread, played by hand: take whatever is newest and report having consumed it. Nothing
	// is rendered — what the far side does with the bytes is Source/Integration's subject — but the
	// watermark is the real one, read from the ring rather than counted here, which is what makes the
	// reclamation below a reclamation rather than an agreement between two counters.
	void Consume()
	{
		const AcquiredSnapshot acquired = Ring.Acquire(Held);

		if (acquired.IsNewer())
		{
			Held = acquired.Sequence;
		}

		(void)Returns.Post(Held);
	}

	// Move the clock to the instant the loop asked for. A `Settled` wake has no instant, so the caller
	// checks the kind before asking.
	void Reach(Wake wake) noexcept { Clock.Set(wake.When); }

	// The newest bytes on the ring, parsed the way the frame thread parses them.
	[[nodiscard]] SnapshotReader Newest() const noexcept { return SnapshotReader{ Ring.Acquire(0).Bytes }; }

	std::uint64_t Held = 0;
};
} // namespace

GYRO_TEST(DispatchLoop, AnUnopenedLoopAsksForNothing)
{
	Fixture fixture;

	// Not a state to serve so much as one that cannot be mistaken for idle: nothing was authored, so
	// nothing is owed, and the root's wiring mistake shows up as a compositor that never draws rather
	// than as a crash.
	GYRO_CHECK_EQ(fixture.Loop.Step(), Wake::Never());
	GYRO_CHECK_EQ(fixture.Loop.Publications(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(fixture.Ring.Published(), std::uint64_t{ 0 });
}

GYRO_TEST(DispatchLoop, OpenRefusesAnAuthorlessOrOutputlessScene)
{
	Fixture fixture;

	const SceneOutput outputs[] = { Panel() };

	GYRO_CHECK_EQ(fixture.Loop.Open(nullptr, outputs).error().Code(), EINVAL);

	// The one that would otherwise pass every test in the process: a scene laid out against no outputs
	// publishes a wake schedule of length zero, which decision 84 makes *no* information rather than
	// partial information — so the frame thread folds to idle with a scene nobody ever sees.
	{
		Result<std::unique_ptr<IGym>> author = MakeGym("lanes");

		GYRO_REQUIRE(author);
		GYRO_CHECK_EQ(fixture.Loop.Open(std::move(*author), {}).error().Code(), EINVAL);
	}

	GYRO_REQUIRE(fixture.Open("lanes"));

	// And a second author is refused rather than replacing the first, because the store it would author
	// into already holds the first one's tree.
	{
		Result<std::unique_ptr<IGym>> second = MakeGym("settle");

		GYRO_REQUIRE(second);
		GYRO_CHECK_EQ(fixture.Loop.Open(std::move(*second), outputs).error().Code(), EEXIST);
	}
}

GYRO_TEST(DispatchLoop, WhatTheAuthorWroteCrossesAndTheSequenceAdvances)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Open("lanes"));

	const Wake first = fixture.Loop.Step();

	GYRO_CHECK_EQ(fixture.Loop.Publications(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(fixture.Loop.Deferrals(), std::uint64_t{ 0 });

	// Parsed the way the frame thread parses it, because bytes that validate on the producer's own terms
	// are the failure this crossing is most likely to have: the reader is what decides whether a
	// snapshot is a snapshot.
	const SnapshotReader reader = fixture.Newest();

	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK_EQ(reader.Sequence(), std::uint64_t{ 1 });
	GYRO_CHECK(!reader.Nodes<Node>().empty());

	// One wake per output, in output order. A run of any other length is what decision 84 has the frame
	// side reject wholesale, so publishing one is publishing nothing.
	GYRO_CHECK_EQ(reader.Wakes().size(), std::size_t{ 1 });

	// The lanes gym retargets forever, so it always wants to be called again — and it is the only way a
	// scene keeps moving once settling is exact.
	GYRO_REQUIRE(first.Which != Wake::Kind::Settled);

	// Four more edges, with the frame thread keeping up. Each one is a fresh scene on the ring under a
	// sequence nobody has skipped.
	for (std::uint64_t expected = 2; expected <= 5; ++expected)
	{
		fixture.Consume();
		fixture.Reach(fixture.Loop.Step());

		GYRO_CHECK_EQ(fixture.Ring.Published(), expected);
	}

	GYRO_CHECK_EQ(fixture.Loop.Publications(), std::uint64_t{ 5 });
	GYRO_CHECK_EQ(fixture.Loop.Deferrals(), std::uint64_t{ 0 });

	// **The zero above is the proof that reclamation happened**, and it is a better one than counting
	// buffers. The ring is four deep, so a fifth publish is only possible if the watermark moved and the
	// oldest snapshot went back to the pool — and it went back to the pool only to be taken straight out
	// of it again for the publish that followed, which is why `Pooled` is a poor witness and `Retained`
	// staying bounded is the honest one.
	GYRO_CHECK(fixture.Loop.Reports() > 0);
	GYRO_CHECK(fixture.Loop.Outbox().Watermark() > 0);
	GYRO_CHECK(fixture.Loop.Outbox().Retained() <= SnapshotRingDepth);
}

GYRO_TEST(DispatchLoop, ASceneThatFinishesStopsAskingToBeCalledBack)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Open("settle"));

	Wake wake = Wake::EveryFrame();
	std::size_t steps = 0;

	while (wake.Which != Wake::Kind::Settled && steps < StepLimit)
	{
		wake = fixture.Loop.Step();
		++steps;

		if (wake.Which != Wake::Kind::Settled)
		{
			fixture.Consume();
			fixture.Reach(wake);
		}
	}

	// **This is the only instrument in the tree for *doing nothing costs nothing* on the authoring
	// side.** The settle gym writes once and answers `Never()` forever after, so anything still due here
	// is the loop's own — and the last thing due is the analytic instant the final spring comes to rest,
	// at which the scene is re-serialised without it.
	GYRO_CHECK_EQ(wake, Wake::Never());
	GYRO_CHECK(steps < StepLimit);

	// More than one, and the number is not asserted because it is `Gym`'s to tune: each of the four
	// sprung channels comes to rest at its own analytic instant, and each of those is a step whose only
	// work is to re-serialise the scene without the channel that finished. A run of exactly one would
	// mean the scene had settled before it was ever published, which is a gym that does nothing rather
	// than the control this one is meant to be.
	GYRO_CHECK(fixture.Loop.Publications() >= 2);

	// At rest and stepped again — which the root would never do, but a spurious wake makes possible —
	// the answer does not change and no new demand appears from nowhere.
	const std::uint64_t published = fixture.Loop.Publications();

	GYRO_CHECK_EQ(fixture.Loop.Step(), Wake::Never());
	GYRO_CHECK_EQ(fixture.Loop.Publications(), published + 1);
}

GYRO_TEST(DispatchLoop, AFullRingDefersRatherThanLosingAScene)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Open("lanes"));

	// The frame thread never consumes, so the ring fills after `SnapshotRingDepth` publishes and every
	// step past that is refused.
	Wake wake = Wake::Never();

	for (std::size_t step = 0; step < SnapshotRingDepth + 2; ++step)
	{
		wake = fixture.Loop.Step();
		fixture.Reach(wake);
	}

	GYRO_REQUIRE(fixture.Loop.Deferrals() > 0);
	GYRO_CHECK(fixture.Loop.Outbox().HasPending());

	// Nothing was published beyond what the ring can hold, and the sequence the pending snapshot carries
	// was never consumed — so the run stays contiguous when it finally goes out.
	GYRO_CHECK_EQ(fixture.Ring.Published(), SnapshotRingDepth);

	// **The retry is armed, and nothing else would arm it.** A refused publish is unblocked by the frame
	// thread posting a report, and the return channel carries no descriptor — so a loop that folded to
	// `Never()` here would sit on the last scene before a quiescing world and never send it.
	GYRO_REQUIRE(wake.Which != Wake::Kind::Settled);
	GYRO_CHECK(wake.When <= Advanced(fixture.Clock.Now(), PublishRetryInterval));

	// The frame thread catches up. One report is enough to move the watermark past the oldest sequence,
	// which frees the slot the deferred snapshot has been waiting for.
	fixture.Consume();

	const std::uint64_t deferred = fixture.Loop.Deferrals();

	fixture.Reach(fixture.Loop.Step());

	GYRO_CHECK(!fixture.Loop.Outbox().HasPending());
	GYRO_CHECK_EQ(fixture.Loop.Deferrals(), deferred);
	GYRO_CHECK_EQ(fixture.Ring.Published(), SnapshotRingDepth + 1);

	// And what came out the far side is still a snapshot, under the sequence the ring assigned rather
	// than one the deferral skipped.
	const SnapshotReader reader = fixture.Newest();

	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK_EQ(reader.Sequence(), SnapshotRingDepth + 1);
}
