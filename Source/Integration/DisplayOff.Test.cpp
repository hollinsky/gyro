#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

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
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Scene/Commit.h"
#include "Scene/Idle.h"
#include "Scene/Output.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Node.h"

// **Nobody touches the machine, the screen goes dark, nothing is drawn while it is, and the first touch
// brings it back with the whole picture.** That sentence is what this file asserts, end to end: the
// ladder asks through the store, the request crosses as bytes, the frame thread hands it to a presenter
// that takes time to answer, and the loop stays out of the presenter's way until it has.
//
// **It is here because it names `Scene`, `Frame` and `Headless` at once**, which no module may, and
// because each half is only right in combination. A ladder that asked and a loop that went on
// submitting is a panel refusing frames for as long as it is dark; a loop that stopped and a request
// that never crossed is a desktop that froze instead of going dark. The harness is
// Source/Integration/SceneIdle.Test.cpp's, cut down to what this needs.

namespace
{
using namespace std::chrono_literals;

constexpr PixelSize<DeviceSpace> Screen{ 320, 180 };
constexpr Duration Period = PeriodFromHertz(60.0);

// A number the clock is moved past and nothing more; the ladder's own arithmetic is asserted in
// Source/Scene/Idle.Test.cpp.
constexpr Duration Timeout = 600s;

// Architecture.md's figure for a DPMS transition. A presenter that answered instantly would let a loop
// that drew straight through the transition pass.
constexpr Duration Transition = 100ms;

constexpr std::size_t IterationLimit = 4000;

class Desk
{
public:
	Desk()
	{
		OutputConfiguration configuration;
		configuration.Generation = 1;
		configuration.Resolution = Screen;
		configuration.Period = Period;

		m_Panel = m_Device.Add(configuration, HeadlessOutputPolicy{ .ModesetLatency = Transition });
		GYRO_REQUIRE(m_Panel != nullptr);
		GYRO_REQUIRE(m_Renderer.BindTargets(m_Panel->Targets(), ColorState::Srgb()));

		m_Output.Bind(*m_Panel, m_Renderer, 0, configuration);
		m_Loop.Bind({ &m_Output, 1 });
		m_Loop.Listen(m_Sources);

		m_OnPresented.ConnectTo<&Desk::OnPresented>(m_Panel->Presented, *this);

		// The composition root's half of a completed reconfiguration, which is `BoundOutput::Adopt`: a
		// panel that came back with new images has them bound before anything records into them.
		m_OnReconfigured.ConnectTo<&Desk::OnReconfigured>(m_Panel->Reconfigured, *this);

		const SceneOutput outputs[] = { SceneOutput{
			.Id = OutputId{ 1, 1 }, .Generation = 1, .Density = Scale::FromInteger(1), .Grid = Screen } };
		m_Store.SetOutputs(outputs);

		m_Ladder.DisplayOffAfter(Timeout, m_Clock.Now());
	}

	[[nodiscard]] SceneStore& Store() noexcept { return m_Store; }

	[[nodiscard]] Instant Now() const noexcept { return m_Clock.Now(); }

	void WaitUntil(Instant instant) { m_Clock.Set(std::max(instant, m_Clock.Now())); }

	// The dispatch thread's step, as much of it as this file needs: the ladder, and then the scene it wrote,
	// published.
	void Step()
	{
		m_Idle = m_Ladder.Step(m_Store, m_Clock.Now());
		Publish();
	}

	// Somebody touched the machine, and the step that follows.
	[[nodiscard]] bool Touch()
	{
		const bool lit = m_Ladder.Touch(m_Store, m_Clock.Now());

		Step();

		return lit;
	}

	// Run the frame side until it would block indefinitely, republishing where the serializer said a
	// channel comes to rest — SceneIdle.Test.cpp's loop, for its reasons.
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

			if (m_Republish.Which != Wake::Kind::Settled)
			{
				next = std::min(next, m_Republish.When);
			}

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

			if (m_Republish.IsDue(m_Clock.Now()))
			{
				Publish();
			}
		}

		return false;
	}

	[[nodiscard]] Wake Idle() const noexcept { return m_Idle; }

	[[nodiscard]] bool Powered() const noexcept { return m_Panel->Configuration().Powered; }

	[[nodiscard]] bool Reconfiguring() const noexcept { return m_Output.IsReconfiguring(); }

	[[nodiscard]] std::uint64_t Frames() const noexcept { return m_Frames; }

	[[nodiscard]] std::uint64_t Commits() const noexcept { return m_Panel->Commits; }

	[[nodiscard]] std::uint64_t Refused() const noexcept { return m_Output.Refused(); }

private:
	void Publish()
	{
		const SnapshotPublisher& publisher = m_Serializer.Serialize(m_Store);

		publisher.Build(m_Snapshot, m_Ring.NextSequence());

		// The watermark is what the frame loop holds, which is what its report would carry — this file
		// publishes more often than the ring is deep, and a watermark of zero would reclaim nothing.
		GYRO_CHECK(m_Ring.Publish(m_Snapshot.Bytes(), m_Loop.Held()));

		m_Republish = m_Serializer.Republish();
	}

	void OnPresented(const PresentationInfo&) noexcept { ++m_Frames; }

	void OnReconfigured(const OutputConfiguration&) noexcept
	{
		GYRO_CHECK(m_Renderer.BindTargets(m_Panel->Targets(), ColorState::Srgb()));
	}

	ManualClock m_Clock;

	SceneStore m_Store{ m_Clock };
	SceneSerializer m_Serializer;
	IdleLadder m_Ladder;
	SnapshotBuffer m_Snapshot;
	SnapshotRing m_Ring;
	ReturnChannel m_Returns;

	HeadlessDevice m_Device{ m_Clock };
	HeadlessOutput* m_Panel = nullptr;
	SimulatedRenderer m_Renderer;
	SceneEvaluator m_Evaluator{ m_Clock };

	FrameOutput m_Output;
	FrameLoop m_Loop{ m_Clock, m_Ring, m_Returns, m_Evaluator };

	IEventSource* m_Sources[1] = { &m_Device };

	Wake m_Idle = Wake::Never();
	Wake m_Republish = Wake::Never();
	Connection<const PresentationInfo&> m_OnPresented;
	Connection<const OutputConfiguration&> m_OnReconfigured;
	std::uint64_t m_Frames = 0;
};

[[nodiscard]] EntityId Window(SceneStore& store)
{
	return store
	    .CreateImage({}, NodeProperties{ .Position = { 20.0, 20.0, 0.0 }, .Extent = { 120.0F, 90.0F } }, ImageContent{})
	    .value();
}
} // namespace

GYRO_TEST(DisplayOff, ThePanelGoesDarkDrawsNothingWhileItIsAndComesBackWithTheWholePicture)
{
	Desk desk;
	const Instant start = desk.Now();
	const EntityId window = Window(desk.Store());

	GYRO_REQUIRE(window != EntityId{});

	desk.Step();

	GYRO_REQUIRE(desk.RunUntilIdle());

	// A window on a lit desktop, drawn once, and the only instant anything is waiting for is the one the
	// panel would go dark at.
	GYRO_CHECK_EQ(desk.Frames(), std::uint64_t{ 1 });
	GYRO_CHECK(desk.Idle() == Wake::At(Advanced(start, Timeout)));

	desk.WaitUntil(Advanced(start, Timeout));
	desk.Step();

	GYRO_CHECK(desk.Idle() == Wake::Never());
	GYRO_REQUIRE(desk.RunUntilIdle());

	// Dark, answered, and nothing drawn to get there: the new scene that carried the request is one the
	// loop had never drawn, and it did not draw it into a panel that was going off. Nothing was refused
	// either — the loop stayed out of the way rather than finding out once a frame.
	GYRO_CHECK(!desk.Powered());
	GYRO_CHECK(!desk.Reconfiguring());
	GYRO_CHECK_EQ(desk.Frames(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(desk.Commits(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(desk.Refused(), std::uint64_t{ 0 });

	// **Something animating on a dark panel costs nothing**: a clock in a shell's bar ticking over, a
	// window sliding somewhere. The scene crosses and the channels retire on the dispatch side, and the
	// frame side arms nothing through all of it.
	{
		SceneCommit commit{ desk.Store(),
			                CommitAuthor::Shell,
			                desk.Now(),
			                SceneCommit::Uncatalogued{ { .Translation = Animate(Motion::Standard) } } };

		GYRO_REQUIRE(commit.Move(window, { 180.0, 20.0, 0.0 }));
	}

	desk.Step();

	GYRO_REQUIRE(desk.RunUntilIdle());
	GYRO_CHECK_EQ(desk.Frames(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(desk.Commits(), std::uint64_t{ 1 });

	// And the first touch lights it, says it did, and the picture that comes back is the whole of where
	// the world got to while nobody was looking — one frame, because what moved has finished moving.
	const Instant touched = desk.Now();

	GYRO_CHECK(desk.Touch());
	GYRO_REQUIRE(desk.RunUntilIdle());

	GYRO_CHECK(desk.Powered());
	GYRO_CHECK_EQ(desk.Frames(), std::uint64_t{ 2 });
	GYRO_CHECK_EQ(desk.Refused(), std::uint64_t{ 0 });

	// And the count starts again from the touch.
	GYRO_CHECK(desk.Idle() == Wake::At(Advanced(touched, Timeout)));
}
