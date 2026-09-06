#include "Scene/Return.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Buffer.h"
#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Core/Signal.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Publication/Return.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Content.h"

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

	// What a client is owed, which is the only thing on this leg that names a window.
	struct Frame
	{
		EntityId Entity{};
		std::size_t Output = 0;
		Instant At{};
	};

	std::vector<Flip> Flips;
	std::vector<BufferId> Releases;
	std::vector<Frame> Frames;

	void OnPresented(std::size_t output, std::uint64_t sequence, Instant at)
	{
		Flips.push_back({ .Output = output, .Sequence = sequence, .At = at });
	}

	void OnReleased(BufferId id) { Releases.push_back(id); }

	void OnReached(EntityId entity, std::size_t output, const OutputPresentation& shown)
	{
		Frames.push_back({ .Entity = entity, .Output = output, .At = shown.At });
	}

	// Connections are members of the observer, which is Core/Signal.h's whole shape: connecting links
	// two objects that already exist, and a watcher going out of scope unlinks itself.
	Connection<std::size_t, std::uint64_t, Instant> Presented;
	Connection<BufferId> Released;
	Connection<EntityId, std::size_t, const OutputPresentation&> Reached;

	void Watch(SceneReturn& drain)
	{
		Presented.ConnectTo<&Watcher::OnPresented>(drain.Presented, *this);
		Released.ConnectTo<&Watcher::OnReleased>(drain.Released, *this);
		Reached.ConnectTo<&Watcher::OnReached>(drain.Reached, *this);
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
// The same, on the output at `index`, which is what a desk with two monitors posts when only one of
// them has flipped since the last report.
[[nodiscard]] FrameReport FlipOn(std::size_t index, std::uint64_t sequence, Instant at, std::uint32_t outputs)
{
	FrameReport report{};
	report.Watermark = sequence;
	report.OutputCount = outputs;
	report.Presentations[index] = { .Sequence = sequence, .At = at };

	return report;
}

ManualClock Clock;

// One panel at the origin, or two side by side with the seam at x = 1920.
void Panels(SceneStore& store, std::size_t count)
{
	const SceneOutput set[2]{ { .Id = OutputId{ 1, 1 },
		                        .Bounds = { .Origin = { 0.0, 0.0 }, .Extent = { 1920.0, 1080.0 } },
		                        .Density = Scale::FromInteger(1),
		                        .Grid = { 1920, 1080 } },
		                      { .Id = OutputId{ 2, 1 },
		                        .Bounds = { .Origin = { 1920.0, 0.0 }, .Extent = { 1920.0, 1080.0 } },
		                        .Density = Scale::FromInteger(1),
		                        .Grid = { 1920, 1080 } } };

	store.SetOutputs({ set, count });
}

[[nodiscard]] EntityId Window(SceneStore& store, double x)
{
	const EntityId frame =
		store.CreateContainer({}, { .Position = { x, 0.0, 0.0 }, .Extent = { 400.0F, 300.0F } }).value();

	return store.CreateImage(frame, { .Extent = { 400.0F, 300.0F } }, ImageContent{}).value();
}

// A client's commit: new pixels on an existing window, which is what puts it in the ledger.
void Commit(SceneStore& store, EntityId content, std::uint32_t generation)
{
	SceneCommit commit{ store, CommitAuthor::Client };

	static_cast<void>(commit.Attach(content, TextureId{ 1, generation }, {}));
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

// The ledger: decision 115's derivation, which is what turns *sequence S was shown* into *this
// window's client may draw again*. Every failure below is the same one a person sees — an application
// that draws its first frame and then never repaints — so each is pinned separately rather than
// covered by one round trip.

GYRO_TEST(SceneReturn, AWindowHearsBackWhenTheFrameCarryingItReachesTheGlass)
{
	SceneStore store{ Clock };
	Panels(store, 1);

	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	const EntityId content = Window(store, 100.0);

	Commit(store, content, 1);
	drain.Seal(5, store);

	GYRO_REQUIRE(drain.Owing());

	drain.Drain(Flip(5, Vblank));

	GYRO_REQUIRE_EQ(watcher.Frames.size(), std::size_t{ 1 });
	GYRO_CHECK(watcher.Frames[0].Entity == content);
	GYRO_CHECK(watcher.Frames[0].At == Vblank);

	// And nothing is owed afterwards, which is what stops the doorbell being rung for the rest of the
	// run: the composition root wakes this thread on a presented frame only while something is waiting.
	GYRO_CHECK(!drain.Owing());
}

GYRO_TEST(SceneReturn, ACommitIsShownByAnyLaterSequenceAndNotOnlyItsOwn)
{
	SceneStore store{ Clock };
	Panels(store, 1);

	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	Commit(store, Window(store, 100.0), 1);
	drain.Seal(5, store);

	// The ring takes the newest and skips what it passed (74), so the frame a client's pixels are shown
	// in is very often not the snapshot they were published in. Waiting for equality is a callback that
	// never arrives on a machine that dropped one publication.
	drain.Drain(Flip(9, Vblank));

	GYRO_CHECK_EQ(watcher.Frames.size(), std::size_t{ 1 });
}

GYRO_TEST(SceneReturn, AFrameOlderThanTheCommitIsNotTheOneItIsWaitingFor)
{
	SceneStore store{ Clock };
	Panels(store, 1);

	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	Commit(store, Window(store, 100.0), 1);
	drain.Seal(9, store);

	// A frame already in flight when the commit was made. Answering on it is a client told to draw
	// against pixels the panel has not shown, which is the tearing-adjacent half of decision 75's
	// argument for two numbers rather than one.
	drain.Drain(Flip(7, Vblank));

	GYRO_CHECK(watcher.Frames.empty());
	GYRO_CHECK(drain.Owing());

	drain.Drain(Flip(9, Advanced(Vblank, std::chrono::milliseconds{ 16 })));

	GYRO_CHECK_EQ(watcher.Frames.size(), std::size_t{ 1 });
}

GYRO_TEST(SceneReturn, TheFastestOutputIsTheCadenceAndTheSecondIsNotASecondCallback)
{
	SceneStore store{ Clock };
	Panels(store, 2);

	SceneReturn drain;
	drain.SetOutputs(2);

	Watcher watcher;
	watcher.Watch(drain);

	// Straddling the seam, which is decision 32's case: one buffer and one callback queue, two panels
	// that will show it at different moments.
	Commit(store, Window(store, 1800.0), 1);
	drain.Seal(5, store);

	drain.Drain(FlipOn(1, 5, Vblank, 2));

	GYRO_REQUIRE_EQ(watcher.Frames.size(), std::size_t{ 1 });

	// The slower panel catching up is not a second frame. Sending one would be a client drawing twice
	// for one refresh and then stalling — decision 28's bug, arriving through the return leg.
	drain.Drain(FlipOn(0, 5, Advanced(Vblank, std::chrono::milliseconds{ 9 }), 2));

	GYRO_CHECK_EQ(watcher.Frames.size(), std::size_t{ 1 });
	GYRO_CHECK(!drain.Owing());
}

GYRO_TEST(SceneReturn, ASecondCommitSupersedesTheFirstRatherThanQueueingBehindIt)
{
	SceneStore store{ Clock };
	Panels(store, 1);

	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	const EntityId content = Window(store, 100.0);

	Commit(store, content, 1);
	drain.Seal(5, store);

	Commit(store, content, 2);
	drain.Seal(6, store);

	// A client that committed twice inside one refresh is owed one answer, not two: the second commit is
	// what the panel will show, and `ClientSurface::Apply` has already folded both commits' callbacks
	// into one due list. Two entries here would be two `done` events for one frame.
	drain.Drain(Flip(6, Vblank));

	GYRO_CHECK_EQ(watcher.Frames.size(), std::size_t{ 1 });
}

GYRO_TEST(SceneReturn, AWindowOnNoOutputWaitsAndIsAnsweredWhenItComesIntoView)
{
	SceneStore store{ Clock };
	Panels(store, 1);

	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	const EntityId content = Window(store, 6000.0);
	const EntityId frame = store.Find(content)->Parent;

	Commit(store, content, 1);
	drain.Seal(5, store);

	// Nothing is going to present it, so nothing may tell its client to draw — the protocol's own rule,
	// and the reason a minimised application stops spending a core.
	drain.Drain(Flip(5, Vblank));

	GYRO_CHECK(watcher.Frames.empty());
	GYRO_CHECK(drain.Owing());

	{
		SceneCommit moved{ store, CommitAuthor::Compositor, store.Now(), Transition::None };

		static_cast<void>(moved.Move(frame, { 100.0, 0.0, 0.0 }));
	}

	// Re-armed rather than abandoned, which is the half that makes the rule above survivable: the frame
	// the window comes back into view on is the frame its callback goes out on, without the client
	// having done anything to ask again.
	drain.Seal(6, store);
	drain.Drain(Flip(6, Advanced(Vblank, std::chrono::milliseconds{ 16 })));

	GYRO_CHECK_EQ(watcher.Frames.size(), std::size_t{ 1 });
}

GYRO_TEST(SceneReturn, AWindowThatWentAwayIsOwedNothing)
{
	SceneStore store{ Clock };
	Panels(store, 1);

	SceneReturn drain;
	drain.SetOutputs(1);

	Watcher watcher;
	watcher.Watch(drain);

	const EntityId content = Window(store, 100.0);

	Commit(store, content, 1);
	drain.Seal(5, store);

	{
		SceneCommit gone{ store, CommitAuthor::Client };

		static_cast<void>(gone.Retire(store.Find(content)->Parent));
	}

	// Decision 114 keeps the entity in the tree while its exit runs, and the serializer frees it on the
	// pass it settles — which, before there is an exit catalog, is the very next one. The seal after that
	// is what drops the entry: a client that disconnected is not waiting to hear about the frames its
	// window spends leaving.
	SceneSerializer serializer;

	static_cast<void>(serializer.Serialize(store));

	GYRO_REQUIRE(!store.IsLive(content));

	drain.Seal(6, store);
	drain.Drain(Flip(6, Vblank));

	GYRO_CHECK(watcher.Frames.empty());
	GYRO_CHECK(!drain.Owing());
}

GYRO_TEST(SceneReturn, RebindingTheOutputSetLeavesAWaitingWindowWaitingRatherThanStranded)
{
	SceneStore store{ Clock };
	Panels(store, 2);

	SceneReturn drain;
	drain.SetOutputs(2);

	Watcher watcher;
	watcher.Watch(drain);

	Commit(store, Window(store, 2100.0), 1);
	drain.Seal(5, store);

	// The second monitor is unplugged. A mask is a set of positions in the old set (84), so it names
	// different panels now and is dropped — but the client behind it is still waiting, so the entry is
	// re-armed at the next publication rather than erased.
	Panels(store, 1);
	drain.SetOutputs(1);

	GYRO_REQUIRE(drain.Owing());

	drain.Seal(6, store);
	drain.Drain(Flip(6, Vblank));

	GYRO_CHECK(watcher.Frames.empty());
	GYRO_CHECK(drain.Owing());
}
