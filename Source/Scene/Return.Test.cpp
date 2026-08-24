#include "Scene/Return.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Buffer.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Publication/Return.h"
#include "Testing/Test.h"

// What the dispatch side promises about a frame that reached the glass, before there is a protocol to
// tell anybody about it.
//
// Each claim below fails silently if it is wrong, and each fails as a *client* symptom rather than as
// a compositor one — which is why they are worth pinning here, on the leg where the client does not
// exist yet. A flip that is never announced is a `wl_surface.frame` callback that never arrives, and a
// client waiting on one never draws again; an announcement that repeats is a client running at twice
// the panel rate and then stalling; and an announcement attributed to the wrong index is presentation
// feedback from a monitor the surface is not on, which is a client pacing itself to a refresh rate
// nothing near it runs at.

namespace
{
constexpr Instant Vblank = Monotonic::FromNanoseconds(9'000'000'000);

// The observer `Protocol` will be, reduced to what it can already do: remember what it was told.
struct Watcher
{
	struct Flip
	{
		std::size_t Output = 0;
		std::uint64_t Sequence = 0;
		Instant At{};
	};

	std::vector<Flip> Flips;
	std::vector<BufferId> Releases;

	void OnPresented(std::size_t output, std::uint64_t sequence, Instant at)
	{
		Flips.push_back({ .Output = output, .Sequence = sequence, .At = at });
	}

	void OnReleased(BufferId id) { Releases.push_back(id); }

	// Connections are members of the observer, which is Core/Signal.h's whole shape: connecting links
	// two objects that already exist, and a watcher going out of scope unlinks itself.
	Connection<std::size_t, std::uint64_t, Instant> Presented;
	Connection<BufferId> Released;

	void Watch(SceneReturn& drain)
	{
		Presented.ConnectTo<&Watcher::OnPresented>(drain.Presented, *this);
		Released.ConnectTo<&Watcher::OnReleased>(drain.Released, *this);
	}
};

// A report for `outputs` outputs where output zero flipped and the rest did not, which is the ordinary
// shape: the frame thread posts one every iteration whatever happened.
[[nodiscard]] FrameReport Flip(std::uint64_t sequence, Instant at, std::uint32_t outputs = 1)
{
	FrameReport report{};
	report.Watermark = sequence;
	report.OutputCount = outputs;
	report.Presentations[0] = { .Sequence = sequence, .At = at };

	return report;
}
} // namespace

GYRO_TEST(SceneReturn, APresentedFrameReachesTheObserverWithTheSequenceItWasDrawnFrom)
{
	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	drain.Drain(Flip(7, Vblank));

	GYRO_REQUIRE_EQ(watcher.Flips.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(watcher.Flips[0].Output, std::size_t{ 0 });
	GYRO_CHECK_EQ(watcher.Flips[0].Sequence, std::uint64_t{ 7 });
	GYRO_CHECK(watcher.Flips[0].At == Vblank);

	// And the state behind the event, which is what decision 113's damage clear reads and what an
	// observer that connected afterwards has to be able to ask for.
	GYRO_CHECK(drain.Presentation(0).HasPresented());
	GYRO_CHECK_EQ(drain.Presentation(0).Sequence, std::uint64_t{ 7 });
}

GYRO_TEST(SceneReturn, AnOutputThatDidNotFlipSaysNothing)
{
	SceneReturn drain;
	drain.SetOutputs(2);

	Watcher watcher;
	watcher.Watch(drain);

	// Two outputs, one flip. The second entry is the report's *no news* — zero rather than a sequence —
	// and it is the common case rather than an edge: a report crosses every frame, and an output at
	// 60 Hz beside one at 144 is quiet on most of them.
	drain.Drain(Flip(3, Vblank, 2));

	GYRO_REQUIRE_EQ(watcher.Flips.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(watcher.Flips[0].Output, std::size_t{ 0 });
	GYRO_CHECK(!drain.Presentation(1).HasPresented());
}

GYRO_TEST(SceneReturn, ASequenceThatDidNotAdvanceIsNotASecondFrame)
{
	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	drain.Drain(Flip(4, Vblank));
	drain.Drain(Flip(4, Advanced(Vblank, Duration{ 16'666'666 })));

	// One frame, one announcement. The second report is a merge the frame side already superseded — or
	// a duplicate a host spoke twice about — and announcing it again would hand a client two frame
	// callbacks for one frame, which is a client that draws ahead and then waits on a callback that
	// never comes.
	GYRO_CHECK_EQ(watcher.Flips.size(), std::size_t{ 1 });
	GYRO_CHECK(drain.Presentation(0).At == Vblank);
}

GYRO_TEST(SceneReturn, ARunFromAnotherOutputSetIsIgnoredWholesale)
{
	SceneReturn drain;
	drain.SetOutputs(2);

	Watcher watcher;
	watcher.Watch(drain);

	// A report staged before a monitor was unplugged, arriving after the world was rebound. Decision
	// 84's rule: a run whose length is not this set's is no information rather than partial
	// information, and taking the prefix would report the surviving panel's flip against a timestamp
	// from the one that left.
	drain.Drain(Flip(9, Vblank, 3));

	GYRO_CHECK(watcher.Flips.empty());
	GYRO_CHECK(!drain.Presentation(0).HasPresented());
}

GYRO_TEST(SceneReturn, RebindingTheOutputSetForgetsWhatTheOldOneShowed)
{
	SceneReturn drain;
	drain.SetOutputs(1);

	drain.Drain(Flip(5, Vblank));
	GYRO_REQUIRE(drain.Presentation(0).HasPresented());

	drain.SetOutputs(2);

	// Index zero names a different panel now, so the sequence it last showed is not a fact about it.
	// Keeping it would also swallow the new panel's first frames, since `Observe` only announces a
	// sequence that advanced.
	GYRO_CHECK(!drain.Presentation(0).HasPresented());
	GYRO_CHECK_EQ(drain.Outputs(), std::size_t{ 2 });
}

GYRO_TEST(SceneReturn, AReleasedBufferCrossesAsItsIdentityAndNothingElse)
{
	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	FrameReport report = Flip(2, Vblank);
	report.ReleaseCount = 2;
	report.Releases[0] = BufferId{ 4, 1 };
	report.Releases[1] = BufferId{ 4, 2 };

	drain.Drain(report);

	// Two ids that share a slot and differ by generation are two buffers, not one — which is the whole
	// reason the identity is generational. A release that resolved to whatever occupies the slot now is
	// a client handed back a buffer gyro is still sampling.
	GYRO_REQUIRE_EQ(watcher.Releases.size(), std::size_t{ 2 });
	GYRO_CHECK(watcher.Releases[0] != watcher.Releases[1]);
	GYRO_CHECK(watcher.Releases[0] == BufferId{ 4, 1 });
}

GYRO_TEST(SceneReturn, ANullReleaseIsNotAnnounced)
{
	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	// The record is fixed-capacity and zero-initialised, so a count that outran what was written is the
	// way a null reaches here. `Protocol` would resolve it to no buffer and send nothing; catching it
	// at the drain keeps the signal a statement about a real buffer.
	FrameReport report = Flip(1, Vblank);
	report.ReleaseCount = 3;
	report.Releases[0] = BufferId{ 1, 1 };

	drain.Drain(report);

	GYRO_CHECK_EQ(watcher.Releases.size(), std::size_t{ 1 });
}
