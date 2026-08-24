#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Frame/Evaluator.h"
#include "Frame/Loop.h"
#include "Geometry/Scale.h"
#include "Headless/Device.h"
#include "Headless/Output.h"
#include "Headless/Renderer.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Reader/Reader.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Node.h"

// **A window opens, moves, stops, and the machine goes back to sleep.** That sentence is the whole of
// what this file asserts, and until the wake schedule was published it was false at both ends.
//
// The two failures it stands between are opposite and equally bad, which is why the test is one run
// rather than two assertions. Publish no schedule at all and `FrameLoop::SceneWake` answers `Never()`,
// `Never()` is `Settled`, and `Wants` gates *evaluation* — so an animating scene draws the single frame
// that binding an output buys with whole-output damage and then sits perfectly still while the springs
// it published run to completion unseen. Publish a schedule and never retire what settled, and the
// opposite happens: the snapshot says *every frame, forever*, no later publication contradicts it, and
// the compositor never sleeps again. Docs/Architecture.md#doing-nothing-must-cost-nothing is broken by
// both, and only a run that watches a real animation start *and* finish can tell them apart.
//
// **It is here because it names `Scene`, `Frame`, and `Headless` at once**, which no module may: the
// wake is folded by an author, crosses as bytes, and is read by a walk, and only the composition root
// ever wires those three together. Source/Integration/ScenePublication.Test.cpp is the same crossing
// asked about *pixels*; this one asks about *time*.
//
// **Nothing about the schedule is written by hand.** Source/Integration/Schedulability.Test.cpp drives
// the same loop against a `Wake` a test chose, which is right for what it measures — the schedule's own
// arithmetic — and cannot see this, because a hand-written wake is exactly the thing that was missing.
// Every wake below comes out of `SceneSerializer` walking a store a commit wrote.

namespace
{
using namespace std::chrono_literals;

// A 60 Hz panel, small on purpose: nothing here draws pixels that anybody looks at, and a headless
// target is a real mapping, so a panel-sized one would spend the test's wall clock zeroing memory. The
// resolution reaches the projection and the damage rectangles and nothing else.
constexpr PixelSize<DeviceSpace> Screen{ 320, 180 };
constexpr Duration Period = PeriodFromHertz(60.0);

// Reaching this means the machine stopped being able to move with something still owed — a livelock to
// report rather than a run to wait out. Far past the two hundred or so iterations a settling animation
// actually takes.
constexpr std::size_t IterationLimit = 4000;

// The composition root, in virtual time, with the *authoring* side wired in — which is the one thing
// Source/Integration/Schedulability.Test.cpp's machine deliberately does not have.
class Desktop
{
public:
	Desktop()
	{
		OutputConfiguration configuration;
		configuration.Resolution = Screen;
		configuration.Period = Period;

		m_Panel = m_Device.Add(configuration);
		GYRO_REQUIRE(m_Panel != nullptr);
		GYRO_REQUIRE(m_Renderer.BindTargets(m_Panel->Targets(), ColorState::Srgb()));

		m_Output.Bind(*m_Panel, m_Renderer, 0, configuration);
		m_Loop.Bind({ &m_Output, 1 });
		m_Loop.Listen(m_Sources);

		m_OnPresented.ConnectTo<&Desktop::OnPresented>(m_Panel->Presented, *this);

		// The world's view of the same panel. Decision 87 has `Scene` declare its own output record and
		// the composition root fill it in from the two sides it is the only thing that knows — which is
		// this constructor, doing exactly that job at the size a test needs it at.
		const SceneOutput outputs[] = { SceneOutput{ .Density = Scale::FromInteger(1), .Grid = Screen } };
		m_Store.SetOutputs(outputs);
	}

	[[nodiscard]] SceneStore& Store() noexcept { return m_Store; }

	[[nodiscard]] Instant Now() const noexcept { return m_Clock.Now(); }

	// Serialise and publish, exactly as a dispatch thread would after resolving a commit. Answers what
	// the serializer says dispatch owes itself: the instant some channel comes to rest, or `Never()`.
	Wake Publish()
	{
		const SnapshotPublisher& publisher = m_Serializer.Serialize(m_Store);

		publisher.Build(m_Snapshot, m_Ring.NextSequence());
		GYRO_CHECK(m_Ring.Publish(m_Snapshot.Bytes(), 0));

		++Publications;

		return m_Serializer.Republish();
	}

	[[nodiscard]] Wake SceneWake() const noexcept { return m_Serializer.SceneWake(); }

	[[nodiscard]] std::size_t ActiveCoefficients() const noexcept
	{
		return m_Serializer.ActiveTranslations() + m_Serializer.ActiveScales() + m_Serializer.ActiveRotations() +
		       m_Serializer.ActiveOpacities();
	}

	// What the frame thread would find in the schedule, read back out of the bytes rather than off the
	// serializer — which is the half of the crossing that could be wrong on its own.
	[[nodiscard]] Wake PublishedWake() const noexcept
	{
		const SnapshotReader reader{ m_Snapshot.Bytes() };
		const std::span<const Wake> schedule = reader.Wakes();

		return schedule.size() == 1 ? schedule[0] : Wake::Never();
	}

	// Run until the composition root would block indefinitely, republishing whenever dispatch's own wake
	// says a channel has come to rest.
	//
	// **The republication is the dispatch thread, spelled as the two lines it is.** There is no dispatch
	// event loop in the tree yet, so this stands in for one — and it stands in for it honestly, because
	// what it arms is `SceneSerializer::Republish` and not a poll: the driver never re-serialises on a
	// frame boundary, only at the analytic instant the serializer named. A scene that owes nothing
	// republishes nothing and the loop below has to reach idle on the schedule already published.
	//
	// Nothing returns until both halves are quiet: a fold of `Never()` while a flip is still outstanding
	// is an iteration that has not happened yet rather than an idle machine.
	[[nodiscard]] bool RunUntilIdle()
	{
		for (std::size_t iteration = 0; iteration < IterationLimit; ++iteration)
		{
			const Wake fold = m_Loop.Step();
			const Instant now = m_Clock.Now();

			Instant next = m_Device.NextEvent();

			if (fold.Which != Wake::Kind::Settled)
			{
				next = std::min(next, fold.When);
			}

			if (m_Dispatch.Which != Wake::Kind::Settled)
			{
				next = std::min(next, m_Dispatch.When);
			}

			// The step asked for an instant already passed, which is every iteration between a commit
			// and the flip it is waiting for. Waking *at or after* is the contract, so this waits for the
			// next thing that can change anything rather than arming a timer that fires immediately.
			if (next <= now)
			{
				next = m_Device.NextEvent();
			}

			if (next == Instant{ Duration::max() })
			{
				return fold == Wake::Never();
			}

			if (next > now)
			{
				m_Clock.Set(next);
			}

			if (m_Dispatch.IsDue(m_Clock.Now()))
			{
				m_Dispatch = Publish();
			}
		}

		return false;
	}

	// Publish, and adopt what dispatch owes itself as a result. The entry point every test uses, so that
	// no test can publish without also arming the republication that publication implies.
	void Commit() { m_Dispatch = Publish(); }

	// What dispatch owes itself after the last publication, which is `SceneSerializer::Republish` as the
	// driver above adopted it.
	[[nodiscard]] Wake Republication() const noexcept { return m_Dispatch; }

	// Damage from outside the scene: a backend that lost its targets, a console that drew over the
	// output, a first frame with nothing behind it. The only thing that asks a settled output for a frame.
	void Damage() noexcept { m_Output.DamageWholeOutput(); }

	[[nodiscard]] std::uint64_t Frames() const noexcept { return m_Frames; }

	[[nodiscard]] std::uint64_t Commits() const noexcept { return m_Panel->Commits; }

	std::size_t Publications = 0;

private:
	void OnPresented(const PresentationInfo&) noexcept { ++m_Frames; }

	ManualClock m_Clock;

	SceneStore m_Store{ m_Clock };
	SceneSerializer m_Serializer;
	SnapshotBuffer m_Snapshot;
	SnapshotRing m_Ring;
	ReturnChannel m_Returns;

	HeadlessDevice m_Device{ m_Clock };
	HeadlessOutput* m_Panel = nullptr;
	SimulatedRenderer m_Renderer;

	// The real walk, not `NullEvaluator`. What is being asserted is that a published *scene* keeps an
	// output awake and then lets it sleep, and an evaluator that reports no damage whatever it was
	// handed would answer the second half for the wrong reason.
	SceneEvaluator m_Evaluator{ m_Clock };

	FrameOutput m_Output;
	FrameLoop m_Loop{ m_Clock, m_Ring, m_Returns, m_Evaluator };

	// One source for the whole device, which is decision 80's granularity rather than an economy: a
	// backend's descriptor is one device's and never one output's.
	IEventSource* m_Sources[1] = { &m_Device };

	Wake m_Dispatch = Wake::Never();
	Connection<const PresentationInfo&> m_OnPresented;
	std::uint64_t m_Frames = 0;
};

// A window on the desktop, at rest.
[[nodiscard]] EntityId Window(SceneStore& store)
{
	return store
	    .CreateImage({}, NodeProperties{ .Position = { 20.0, 20.0, 0.0 }, .Extent = { 120.0F, 90.0F } }, ImageContent{})
	    .value();
}
} // namespace

GYRO_TEST(SceneIdle, AStillSceneIsDrawnOnceAndThenDrawsNothingAndArmsNothing)
{
	Desktop desktop;

	GYRO_REQUIRE(Window(desktop.Store()) != EntityId{});

	desktop.Commit();

	// Nothing is moving, so nothing crosses as a coefficient and the schedule says so — in the bytes,
	// which is where the frame thread reads it. Decision 98's *names no coefficient* and decision 69's
	// `Settled` are the same statement about the same scene, made on either side of the waist.
	GYRO_CHECK_EQ(desktop.ActiveCoefficients(), std::size_t{ 0 });
	GYRO_CHECK(desktop.SceneWake() == Wake::Never());
	GYRO_CHECK(desktop.PublishedWake() == Wake::Never());

	GYRO_REQUIRE(desktop.RunUntilIdle());

	// **One frame, because there is a window here and it has to be on the screen.** The schedule says
	// nothing falls due, which is a statement about the future and not about whether this output has
	// ever drawn what it is holding — and reading it as both is a window that is authored, published,
	// acquired, and never composited. Then no timer is armed, which is decision 58's invariant reached
	// from the authoring side: what it forbids is the *second* frame.
	GYRO_CHECK_EQ(desktop.Frames(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(desktop.Commits(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(desktop.Publications, std::size_t{ 1 });

	// Damage from outside the scene is the other way an output is owed a frame, and it outranks the
	// schedule because pixels that have not reached the glass are not a question about timing. Exactly
	// one more, and then back to sleep.
	desktop.Damage();

	GYRO_REQUIRE(desktop.RunUntilIdle());

	GYRO_CHECK_EQ(desktop.Frames(), std::uint64_t{ 2 });
	GYRO_CHECK_EQ(desktop.Commits(), std::uint64_t{ 2 });
	GYRO_CHECK_EQ(desktop.Publications, std::size_t{ 1 });
}

GYRO_TEST(SceneIdle, AnAnimatingSceneIsDrawnEveryFrameUntilItSettles)
{
	Desktop desktop;

	const EntityId window = Window(desktop.Store());
	const Instant origin = desktop.Now();

	{
		SceneCommit commit{ desktop.Store(), CommitAuthor::Shell, origin };

		GYRO_REQUIRE(commit.Move(window, { 180.0, 20.0, 0.0 }, Animate(Motion::Standard)));
		GYRO_REQUIRE(commit.Fade(window, 0.25F, Animate(Motion::Standard)));
	}

	desktop.Commit();

	// Both channels cross, and the schedule the frame thread will read says a frame is owed every time
	// the output offers one. This is the assertion the whole file exists for: before the fold was
	// staged, this same scene published `Never()` and the run below drew exactly one frame.
	GYRO_REQUIRE_EQ(desktop.ActiveCoefficients(), std::size_t{ 2 });
	GYRO_REQUIRE(desktop.PublishedWake().Which == Wake::Kind::Continuous);
	GYRO_REQUIRE_EQ(desktop.PublishedWake().Interval, Duration::zero());

	GYRO_REQUIRE(desktop.RunUntilIdle());

	// A 0.4 second response settling against a sixteenth of a device pixel finishes somewhere past a
	// second, so a 60 Hz panel owes something like sixty frames. The bound is loose on both sides
	// deliberately: what a tight one would pin is `Scene/Settle.h`'s numbers and the conservatism in
	// `SettlesAt`, both of which are asserted where they live. What is asserted here is that the
	// animation was *drawn* — many frames rather than one — and that it stopped.
	GYRO_CHECK(desktop.Frames() > 30);
	GYRO_CHECK(desktop.Frames() < 300);

	// And it stopped by *retiring*, not by anybody deciding to stop asking. The last publication carries
	// no coefficient at all, so the springs that were running are at rest on the model values a person
	// asked for — which is also what makes the next commit against this window an ordinary retarget
	// rather than an interruption of something invisible.
	GYRO_CHECK_EQ(desktop.ActiveCoefficients(), std::size_t{ 0 });
	GYRO_CHECK(desktop.SceneWake() == Wake::Never());
	GYRO_CHECK(desktop.PublishedWake() == Wake::Never());

	// The window ended where it was sent, exactly, on the model value the record carries inline.
	const Entity* settled = desktop.Store().Find(window);

	GYRO_REQUIRE(settled != nullptr);
	GYRO_CHECK(settled->Translation.IsAtRest());
	GYRO_CHECK_EQ(settled->Translation.Model(), Vector3<double>(180.0, 20.0, 0.0));
	GYRO_CHECK(settled->Opacity.IsAtRest());
	GYRO_CHECK_EQ(settled->Opacity.Model(), 0.25F);
}

GYRO_TEST(SceneIdle, DispatchIsWokenByTheAnalyticSettleAndNotByEveryFrame)
{
	Desktop desktop;

	const EntityId window = Window(desktop.Store());

	{
		SceneCommit commit{ desktop.Store(), CommitAuthor::Shell, desktop.Now() };

		GYRO_REQUIRE(commit.Move(window, { 180.0, 20.0, 0.0 }, Animate(Motion::Standard)));
	}

	desktop.Commit();

	const Wake owed = desktop.Republication();

	// One instant and not a standing commitment. Dispatch has exactly one thing to do while an animation
	// runs — re-serialise at the moment something comes to rest — and a `Continuous` here would be the
	// scene being rebuilt once per frame on the thread Docs/Open.md's pacing entry is about.
	GYRO_REQUIRE_EQ(static_cast<int>(owed.Which), static_cast<int>(Wake::Kind::Timed));
	GYRO_CHECK(owed.When > desktop.Now());

	GYRO_REQUIRE(desktop.RunUntilIdle());

	// Two publications for one animation: the commit's, and the one at the settle instant that retires
	// what finished. Sixty frames were drawn between them and the store was walked twice — which is the
	// whole difference between an analytic settle and observing an evaluation.
	GYRO_CHECK_EQ(desktop.Publications, std::size_t{ 2 });
	GYRO_CHECK(desktop.Frames() > 30);
}
