#include "Gym/Gym.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/NodeTransform.h"
#include "Gym/Lanes.h"
#include "Scene/Commit.h"
#include "Scene/Store.h"

namespace
{
// The tick each lane retargets on. Four periods with no common factor worth speaking of, so the lanes
// do not flip together — a scene whose four channels all restart on one instant is one in which a
// channel driving the wrong lane is invisible, because everything moved anyway.
constexpr Duration SlidePeriod = std::chrono::milliseconds{ 900 };
constexpr Duration GrowPeriod = std::chrono::milliseconds{ 1300 };
constexpr Duration FadePeriod = std::chrono::milliseconds{ 700 };
constexpr Duration TurnPeriod = std::chrono::milliseconds{ 1100 };

// The motion each lane is driven under.
//
// **Named motions rather than a catalog transition, and the asymmetry with a shell is deliberate.** A
// shell names a `Transition` because it is making a window open, and the bundle is the four channels
// of that one event designed together (decision 13). A lane is not an event: it drives exactly one
// channel and leaves the other three to their own lanes, so reaching for `Definition(WindowOpen)` in
// order to read its `Scale` entry would put a transition's name on a scene that is not that
// transition, and would silently inherit whatever else that entry animates the day somebody retunes
// it. What the rule actually forbids is a call site naming a *number*, and none of these does.
//
// **None of the four overshoots**, which is what makes the tracks mean something: a marker past the
// end of its track is a bug rather than `Motion::Expressive` doing its job. Three different motions
// across the three drawing lanes, so their pacing is legible against each other in one frame.
constexpr Motion SlideMotion = Motion::Snappy;
constexpr Motion GrowMotion = Motion::Standard;
constexpr Motion FadeMotion = Motion::Gentle;
constexpr Motion TurnMotion = Motion::Standard;

static_assert(SlidePeriod > Duration::zero() && GrowPeriod > Duration::zero());
static_assert(FadePeriod > Duration::zero() && TurnPeriod > Duration::zero());

// The next edge strictly after `now`, advanced from the edge that just fired by whole periods.
//
// **From its own origin rather than from the wake it was served at**, which is Core/Wake.h's standing
// obligation on a periodic contributor: a wake is served at the following vblank, so adding a period
// to a value that has already been rounded up accumulates drift for as long as the process runs. The
// loop is what makes a late wake catch up rather than fall behind by every period it slept through,
// and *strictly* after is what stops the scheduler spinning at full rate on an edge already served.
[[nodiscard]] Instant NextEdge(Instant edge, Duration period, Instant now) noexcept
{
	do
	{
		edge = Advanced(edge, period);
	} while (edge <= now);

	return edge;
}

// One lane's tick, as its own transaction.
//
// **A commit per lane rather than one per wake**, because decision 112 makes a commit a scope with one
// origin and each lane's tick is its own event: two lanes falling due at the same wake are two things
// that happened at two instants, and sharing a scope would stamp both with whichever origin was
// written first. It also keeps every write in this file inside a scope that closes at the end of the
// statement that opened it, which is what the guard is.
template<typename Write>
[[nodiscard]] bool Tick(SceneStore& scene, Instant origin, Write write)
{
	SceneCommit commit{ scene, CommitAuthor::Compositor, origin };

	return commit.IsOpen() && write(commit);
}

// What every gym here has: the instrument, and the authoring of it.
class LaneGym : public IGym
{
protected:
	[[nodiscard]] Result<void> AuthorScene(SceneStore& scene)
	{
		const Result<LaneScene> lanes = AuthorLanes(scene);

		if (!lanes)
		{
			return std::unexpected{ lanes.error() };
		}

		m_Lanes = *lanes;

		return {};
	}

	LaneScene m_Lanes{};
};

// The four channels, one lane each, retargeting forever.
//
// **Perpetual by retargeting and not by any other means, because there is no other means.** A spring
// settles exactly — `IsAtRest` is zero offset and zero velocity, and the settle instant is analytic —
// so a scene that keeps moving is a scene something keeps writing to. That makes this the instrument
// for the eager path at rate: every tick below is a `SceneCommit` with an origin, a retarget sampled
// from wherever the previous motion had got to, and a publication behind it.
//
// The rotation lane is authored and not driven here. See Gym/Lanes.h: a turning quad is refused by the
// CPU renderer outright and one refusal loses the whole frame, so the lane that would take this gym's
// picture away from the backend it is most useful on has a gym of its own.
class LanesGym : public LaneGym
{
public:
	[[nodiscard]] std::string_view Name() const noexcept override { return ::Name(GymKind::Lanes); }

	[[nodiscard]] Result<void> Open(SceneStore& scene) override
	{
		const Result<void> authored = AuthorScene(scene);

		if (!authored)
		{
			return authored;
		}

		// The first edge of each lane is one period out rather than immediately, so the scene as
		// authored is what the first frame shows. A gym that retargeted at its first `Advance` would
		// make the resting state something nobody ever sees.
		const Instant now = scene.Now();

		m_Slide = Advanced(now, SlidePeriod);
		m_Grow = Advanced(now, GrowPeriod);
		m_Fade = Advanced(now, FadePeriod);

		return {};
	}

	[[nodiscard]] Wake Advance(SceneStore& scene, Instant now) override
	{
		if (!m_Driving)
		{
			return Wake::Never();
		}

		if (m_Slide <= now)
		{
			m_SlideFar = !m_SlideFar;
			m_Driving = Tick(scene, m_Slide, [&](SceneCommit& commit) {
				return commit.Move(
					m_Lanes.Slide.Marker, m_SlideFar ? m_Lanes.SlideFar : m_Lanes.SlideNear, Animate(SlideMotion)
				);
			});

			m_Slide = NextEdge(m_Slide, SlidePeriod, now);
		}

		if (m_Grow <= now)
		{
			m_GrowFull = !m_GrowFull;
			m_Driving =
				m_Driving && Tick(scene, m_Grow, [&](SceneCommit& commit) {
					return commit.Scale(m_Lanes.Grow.Marker, m_GrowFull ? GrowFull : GrowSmall, Animate(GrowMotion));
				});

			m_Grow = NextEdge(m_Grow, GrowPeriod, now);
		}

		if (m_Fade <= now)
		{
			m_FadeFull = !m_FadeFull;
			m_Driving =
				m_Driving && Tick(scene, m_Fade, [&](SceneCommit& commit) {
					return commit.Fade(m_Lanes.Fade.Marker, m_FadeFull ? FadeFull : FadeDim, Animate(FadeMotion));
				});

			m_Fade = NextEdge(m_Fade, FadePeriod, now);
		}

		// **A refused write latches the gym off, and the freeze is the report.** Every write above is
		// against an id this gym created, inside the only commit it has open, under an origin it
		// supplied — so a refusal is this file being wrong rather than a state to recover from, and
		// there is nowhere for an `Advance` to return one to. Answering `Never()` afterwards stops the
		// scene dead and makes `--frames=N` fall out early, which is loud in exactly the instrument
		// whose other job is to say whether an idle system is idle.
		if (!m_Driving)
		{
			return Wake::Never();
		}

		return Wake::At(std::min({ m_Slide, m_Grow, m_Fade }));
	}

private:
	Instant m_Slide{};
	Instant m_Grow{};
	Instant m_Fade{};

	bool m_SlideFar = false;
	bool m_GrowFull = false;
	bool m_FadeFull = false;

	bool m_Driving = true;
};

// The same instrument, authored once and left to finish.
//
// **The only scene here that folds to idle, and that is the whole of what it is for.** Doing nothing
// must cost nothing: after this gym's one commit there is no timer to arm, no publication to make, and
// nothing for the frame thread to do once the three springs have settled — so the frame count under
// `--frames=N` stops climbing on its own, at an instant the springs' own settling decides. Any wake
// after that is a contributor nobody accounted for, which is the failure this exists to catch and
// which no perpetual scene can see.
class SettleGym final : public LaneGym
{
public:
	[[nodiscard]] std::string_view Name() const noexcept override { return ::Name(GymKind::Settle); }

	[[nodiscard]] Result<void> Open(SceneStore& scene) override
	{
		const Result<void> authored = AuthorScene(scene);

		if (!authored)
		{
			return authored;
		}

		// One commit and one origin for all three lanes, which is the opposite of the perpetual gym's
		// arrangement and is right for the same reason: this is one event. The three channels start
		// together and finish apart, because they are paced differently and not because they were
		// written in some order.
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		if (!commit.IsOpen())
		{
			return Failure(EBUSY, "a gym opens a commit of its own, and one was already open");
		}

		const bool written = commit.Move(m_Lanes.Slide.Marker, m_Lanes.SlideFar, Animate(SlideMotion)) &&
		                     commit.Scale(m_Lanes.Grow.Marker, GrowFull, Animate(GrowMotion)) &&
		                     commit.Fade(m_Lanes.Fade.Marker, FadeFull, Animate(FadeMotion));

		if (!written)
		{
			return Failure(EINVAL, "a gym wrote a channel of a node it had just created, and was refused");
		}

		return {};
	}

	[[nodiscard]] Wake Advance(SceneStore&, Instant) override { return Wake::Never(); }
};

// The rotation lane, driven.
//
// **A gym of its own because of what draws rather than because rotation is a different kind of
// motion.** `Blit::Classify` refuses a quad that is not axis-aligned, and a refusal fails the whole
// `Record`, so under `--backend=dump` this writes no frames for as long as the marker is turning — not
// frames with the marker missing. `DrawsOnCpu` is where a caller is told that before it happens.
//
// The other three lanes are left still, as the reference the turning bar is read against.
class TurnGym final : public LaneGym
{
public:
	[[nodiscard]] std::string_view Name() const noexcept override { return ::Name(GymKind::Turn); }

	[[nodiscard]] Result<void> Open(SceneStore& scene) override
	{
		const Result<void> authored = AuthorScene(scene);

		if (!authored)
		{
			return authored;
		}

		m_Edge = Advanced(scene.Now(), TurnPeriod);

		return {};
	}

	[[nodiscard]] Wake Advance(SceneStore& scene, Instant now) override
	{
		if (!m_Driving)
		{
			return Wake::Never();
		}

		if (m_Edge <= now)
		{
			// A step count rather than an accumulated angle, because six steps close the circle exactly:
			// an angle that only ever grew would drift as the process ran, and a marker whose resting
			// orientations wander is one nobody can read a rotation bug off.
			m_Step = (m_Step + 1) % TurnSteps;

			const Quaternion orientation =
				Quaternion::FromAxisAngle({ 0.0F, 0.0F, 1.0F }, static_cast<float>(m_Step) * TurnStepRadians);

			m_Driving = Tick(scene, m_Edge, [&](SceneCommit& commit) {
				return commit.Turn(m_Lanes.Turn.Marker, orientation, Animate(TurnMotion));
			});

			m_Edge = NextEdge(m_Edge, TurnPeriod, now);
		}

		if (!m_Driving)
		{
			return Wake::Never();
		}

		return Wake::At(m_Edge);
	}

private:
	Instant m_Edge{};
	int m_Step = 0;
	bool m_Driving = true;
};

// The lanes, with a `Glass` panel and a `Smoke` panel laid over them.
//
// It drives the lanes exactly as `LanesGym` does, and that is the point rather than a saving: what a
// gathering material is worth looking at over is content that is *moving*, since a blur over a still
// picture is indistinguishable from a still picture somebody blurred once. So the instrument underneath
// is the same instrument, and what is added is the two panels and the shadow they cast.
//
// Refused by the CPU renderer twice over, on the material and on the elevation.
class MaterialsGym final : public LanesGym
{
public:
	[[nodiscard]] std::string_view Name() const noexcept override { return ::Name(GymKind::Materials); }

	[[nodiscard]] Result<void> Open(SceneStore& scene) override
	{
		const Result<void> lanes = LanesGym::Open(scene);

		if (!lanes)
		{
			return lanes;
		}

		const Result<MaterialOverlay> overlay = AuthorMaterialOverlay(scene, m_Lanes);

		if (!overlay)
		{
			return std::unexpected{ overlay.error() };
		}

		return {};
	}
};
} // namespace

Result<std::unique_ptr<IGym>> MakeGym(std::string_view name)
{
	const std::optional<GymKind> kind = GymNamed(name);

	if (!kind)
	{
		return Failure(EINVAL, "--gym is one of lanes, settle, turn, materials");
	}

	// A switch with no default label, so a fifth enumerator is a build failure here rather than a name
	// that parses and constructs nothing.
	switch (*kind)
	{
		case GymKind::Lanes:
			return std::make_unique<LanesGym>();
		case GymKind::Settle:
			return std::make_unique<SettleGym>();
		case GymKind::Turn:
			return std::make_unique<TurnGym>();
		case GymKind::Materials:
			return std::make_unique<MaterialsGym>();
	}

	return Failure(EINVAL, "--gym is one of lanes, settle, turn, materials");
}

std::span<const std::string_view> GymNames()
{
	return AllGymNames;
}
