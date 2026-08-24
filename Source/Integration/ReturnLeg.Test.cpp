#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Signal.h"
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
#include "Scene/Output.h"
#include "Scene/Return.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Node.h"

// **A scene is authored, drawn, and reaches the glass, and the side that authored it is told so.**
// That round trip is the return leg, and until now only its first half existed: the frame loop posted
// a watermark and nothing on the far side read anything else out of the report.
//
// It is here because it names `Frame` and `Scene` at once, which no module may — `CheckLayering`
// denies that edge, and it is the one edge the whole two-thread split is built around. The forward
// direction of the same crossing is Source/Integration/ScenePublication.Test.cpp; this is the
// direction the client's `wl_surface.frame` callback comes back on.
//
// **What it is standing in for is a client.** There is no protocol layer, so nothing yet turns a
// presented sequence into a set of surfaces — decision 115's retained handle runs are phase two's. The
// number that every one of those derivations is a function of does exist, and it is the thing that is
// wrong or right here: a sequence that is *not the one on the glass* is a frame callback sent for a
// frame nobody saw, and no amount of protocol correctness downstream recovers from it.

namespace
{
constexpr PixelSize<DeviceSpace> Screen{ 320, 180 };
constexpr Duration Period = PeriodFromHertz(60.0);

// Far past the handful of vblanks a frame takes to land. Reaching it means the machine stopped being
// able to move with a flip still owed.
constexpr std::size_t IterationLimit = 512;

// The observer `Protocol` will be, standing in for it with the one thing it could already do.
struct Client
{
	struct Frame
	{
		std::size_t Output = 0;
		std::uint64_t Sequence = 0;
		Instant At{};
	};

	std::vector<Frame> Callbacks;

	void OnPresented(std::size_t output, std::uint64_t sequence, Instant at)
	{
		Callbacks.push_back({ .Output = output, .Sequence = sequence, .At = at });
	}

	Connection<std::size_t, std::uint64_t, Instant> Presented;
};

// Both halves of the boundary in virtual time, with the return leg wired the way the composition root
// wires it: the frame loop posts, the outbox's watermark is taken, and the drain announces the rest.
class Machine
{
public:
	Machine()
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

		const SceneOutput outputs[] = { SceneOutput{ .Density = Scale::FromInteger(1), .Grid = Screen } };
		m_Store.SetOutputs(outputs);
		m_Return.SetOutputs(1);

		m_Watcher.Presented.ConnectTo<&Client::OnPresented>(m_Return.Presented, m_Watcher);

		// What the panel says happened, kept beside what the drain says it was told. The two have to
		// agree on the instant, and they come from opposite ends of the crossing.
		m_OnPresented.ConnectTo<&Machine::OnPresented>(m_Panel->Presented, *this);
	}

	[[nodiscard]] SceneStore& Store() noexcept { return m_Store; }

	[[nodiscard]] const Client& Watcher() const noexcept { return m_Watcher; }

	[[nodiscard]] std::uint64_t Flips() const noexcept { return m_Flips; }

	[[nodiscard]] Instant LastFlip() const noexcept { return m_LastFlip; }

	// Serialise and publish, exactly as the dispatch thread would after resolving a commit. Answers the
	// sequence the ring assigned, which is the number the far side has to hear back.
	std::uint64_t Publish()
	{
		const std::uint64_t sequence = m_Ring.NextSequence();

		m_Serializer.Serialize(m_Store).Build(m_Snapshot, sequence);
		GYRO_CHECK(m_Ring.Publish(m_Snapshot.Bytes(), m_Watermark));

		return sequence;
	}

	// The output is owed a frame from outside the scene, which is what a first frame after a bind is.
	void Damage() noexcept { m_Output.DamageWholeOutput(); }

	// Run the frame loop until this many flips have landed, draining the return channel every iteration
	// the way the dispatch step does. False means the machine stopped moving with a flip still owed.
	[[nodiscard]] bool RunUntil(std::uint64_t flips)
	{
		for (std::size_t iteration = 0; iteration < IterationLimit && m_Flips < flips; ++iteration)
		{
			const Wake fold = m_Loop.Step();
			Collect();

			if (m_Flips >= flips)
			{
				return true;
			}

			const Instant now = m_Clock.Now();
			Instant next = m_Device.NextEvent();

			// The loop wakes a reserve *ahead* of the frame it is serving, so the earlier of the two is
			// what the composition root would sleep to. Taking the vblank alone would step straight past
			// every instant the loop meant to draw at.
			if (fold.Which != Wake::Kind::Settled)
			{
				next = std::min(next, fold.When);
			}

			// An instant already passed is every iteration between a commit and the flip it waits for.
			if (next <= now)
			{
				next = m_Device.NextEvent();
			}

			if (next == Instant{ Duration::max() })
			{
				return false;
			}

			if (next > now)
			{
				m_Clock.Set(next);
			}
		}

		return m_Flips >= flips;
	}

	// One iteration with nothing owed: step, drain, and let a whole refresh of virtual time pass. The
	// loop posts a report every time through whatever happened, because the watermark has to cross.
	void Idle()
	{
		(void)m_Loop.Step();
		Collect();
		m_Clock.Set(Advanced(m_Clock.Now(), Period));
	}

	// The dispatch side's drain, as its two halves: the watermark, and everything derived from what is
	// left. `SnapshotOutbox` owns the first in the real loop and this stands in for exactly that much.
	void Collect()
	{
		FrameReport report{};

		while (m_Returns.Take(report))
		{
			m_Watermark = std::max(m_Watermark, report.Watermark);
			m_Return.Drain(report);
		}
	}

private:
	void OnPresented(const PresentationInfo& info) noexcept
	{
		++m_Flips;
		m_LastFlip = info.PresentedAt;
	}

	ManualClock m_Clock;

	SceneStore m_Store{ m_Clock };
	SceneSerializer m_Serializer;
	SnapshotBuffer m_Snapshot;
	SnapshotRing m_Ring;
	ReturnChannel m_Returns;
	SceneReturn m_Return;
	Client m_Watcher;

	HeadlessDevice m_Device{ m_Clock };
	HeadlessOutput* m_Panel = nullptr;
	SimulatedRenderer m_Renderer;
	SceneEvaluator m_Evaluator{ m_Clock };

	FrameOutput m_Output;
	FrameLoop m_Loop{ m_Clock, m_Ring, m_Returns, m_Evaluator };

	IEventSource* m_Sources[1] = { &m_Device };

	std::uint64_t m_Watermark = 0;
	std::uint64_t m_Flips = 0;
	Instant m_LastFlip{};
	Connection<const PresentationInfo&> m_OnPresented;
};

// A window on the desktop, at rest. Nothing here animates: what is being asserted is the leg, and a
// moving scene would only add publications the assertion has to reason around.
void Window(SceneStore& store)
{
	GYRO_CHECK(store.CreateImage(
		{}, NodeProperties{ .Position = { 20.0, 20.0, 0.0 }, .Extent = { 120.0F, 90.0F } }, ImageContent{}
	));
}
} // namespace

GYRO_TEST(ReturnLeg, TheSceneThatReachedTheGlassIsTheOneTheAuthorHearsBack)
{
	Machine machine;
	Window(machine.Store());

	const std::uint64_t published = machine.Publish();
	machine.Damage();

	GYRO_REQUIRE(machine.RunUntil(1));

	// One flip, one announcement, and the sequence in it is the one the ring assigned to the bytes the
	// composite was walked out of. That is the whole leg: a client's `wl_surface.frame` callback is
	// *this* number turned into surfaces, and a number that named a scene the panel never showed is a
	// callback for a frame nobody saw.
	const std::vector<Client::Frame>& callbacks = machine.Watcher().Callbacks;

	GYRO_REQUIRE_EQ(callbacks.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(callbacks[0].Output, std::size_t{ 0 });
	GYRO_CHECK_EQ(callbacks[0].Sequence, published);

	// And the instant is the panel's own, not the moment the report was read. `wp_presentation_feedback`
	// is what a client paces itself from, so a timestamp taken on this side of the crossing would hand
	// every client a refresh interval with the dispatch thread's latency folded into it.
	GYRO_CHECK(callbacks[0].At == machine.LastFlip());
}

GYRO_TEST(ReturnLeg, EveryFrameIsAnnouncedOnceAndTheSequencesOnlyGoForwards)
{
	Machine machine;
	Window(machine.Store());

	// Four scenes, each published and then drawn. A leg that announced a scene twice would be a client
	// running ahead of the panel; one that skipped a scene would be a client waiting forever on a
	// callback for the frame it did draw.
	for (std::uint64_t frame = 0; frame < 4; ++frame)
	{
		(void)machine.Publish();
		machine.Damage();

		GYRO_REQUIRE(machine.RunUntil(frame + 1));
	}

	const std::vector<Client::Frame>& callbacks = machine.Watcher().Callbacks;

	GYRO_REQUIRE_EQ(callbacks.size(), std::size_t{ 4 });

	for (std::size_t index = 1; index < callbacks.size(); ++index)
	{
		GYRO_CHECK(callbacks[index].Sequence > callbacks[index - 1].Sequence);
		GYRO_CHECK(callbacks[index].At > callbacks[index - 1].At);
	}
}

GYRO_TEST(ReturnLeg, ASettledSceneAnnouncesNothingAndTheChannelStillCarriesTheWatermark)
{
	Machine machine;
	Window(machine.Store());

	(void)machine.Publish();
	machine.Damage();
	GYRO_REQUIRE(machine.RunUntil(1));

	const std::size_t announced = machine.Watcher().Callbacks.size();

	// Nothing is owed now: no damage, no motion, and the scene the panel is holding is the one that was
	// published. The loop keeps posting a report every iteration because the watermark has to cross, and
	// none of those reports may look like a frame.
	for (std::size_t iteration = 0; iteration < 8; ++iteration)
	{
		machine.Idle();
	}

	GYRO_CHECK_EQ(machine.Watcher().Callbacks.size(), announced);
	GYRO_CHECK_EQ(machine.Flips(), std::uint64_t{ 1 });
}
