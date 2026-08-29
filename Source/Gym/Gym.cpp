#include "Gym/Gym.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/NodeTransform.h"
#include "Gym/Card.h"
#include "Gym/Cards.h"
#include "Gym/Lanes.h"
#include "Gym/Pointer.h"
#include "Scene/Commit.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"

namespace
{
// The tick each lane retargets on. Four periods with no common factor worth speaking of, so the lanes
// do not flip together — a scene whose four channels all restart on one instant is one in which a
// channel driving the wrong lane is invisible, because everything moved anyway.
constexpr Duration SlidePeriod = std::chrono::milliseconds{ 900 };
constexpr Duration GrowPeriod = std::chrono::milliseconds{ 1300 };
constexpr Duration FadePeriod = std::chrono::milliseconds{ 700 };
constexpr Duration TurnPeriod = std::chrono::milliseconds{ 1100 };

// How often the card gym swaps the buffer underneath its scene. Slower than every motion above,
// because a swap is the one event here a person reads by *recognising* a picture rather than by seeing
// something move — and out of step with all of them, so the swap does not habitually land on a
// retarget and become invisible inside it.
constexpr Duration SwapPeriod = std::chrono::milliseconds{ 2300 };

// How often the pointer gym reverses its sliding specimens. Slower than every lane above, because what
// is being looked at is an edge rather than a motion: a glyph that crawls does it while crossing
// pixels, and a marker that has already turned round is a glyph nobody had time to look at.
constexpr Duration GlidePeriod = std::chrono::milliseconds{ 2600 };

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

// The pointer specimens glide rather than snap, and the choice is the instrument's: a slow crossing is
// what puts the staircase at every sub-pixel phase in turn, which is the only way a shimmer that
// happens at one phase in twenty is seen at all.
constexpr Motion GlideMotion = Motion::Gentle;

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

// The rotation lane's tick, which is shared because two gyms drive it and one of them drives the other
// three as well.
//
// **A step count rather than an accumulated angle, because six steps close the circle exactly**: an
// angle that only ever grew would drift as the process ran, and a marker whose resting orientations
// wander is one nobody can read a rotation bug off.
class TurnDriver
{
public:
	void Open(Instant now) noexcept { m_Edge = Advanced(now, TurnPeriod); }

	// Retargets the lane if its edge has passed, and answers whether the write was accepted. A gym that
	// is handed `false` latches off, the same way its own lanes' refusals do.
	[[nodiscard]] bool Drive(SceneStore& scene, EntityId marker, Instant now)
	{
		if (m_Edge > now)
		{
			return true;
		}

		m_Step = (m_Step + 1) % TurnSteps;

		const Quaternion orientation =
			Quaternion::FromAxisAngle({ 0.0F, 0.0F, 1.0F }, static_cast<float>(m_Step) * TurnStepRadians);

		const bool written = Tick(scene, m_Edge, [&](SceneCommit& commit) {
			return commit.Turn(marker, orientation, Animate(TurnMotion));
		});

		m_Edge = NextEdge(m_Edge, TurnPeriod, now);

		return written;
	}

	[[nodiscard]] Instant NextDue() const noexcept { return m_Edge; }

private:
	Instant m_Edge{};
	int m_Step = 0;
};

// What every gym here has: the instrument, and the authoring of it.
class LaneGym : public ISceneAuthor
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

	[[nodiscard]] Result<void> Open(SceneStore& scene, ITextures&) override
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

	[[nodiscard]] Wake Advance(SceneStore& scene, ITextures&, Instant now) override
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

protected:
	// Whether the lanes are still being written to, which a gym built on top of this one has to be able
	// to read: a refusal here latches the whole instrument off, and a subclass still driving its own
	// lane afterwards would keep a frozen scene answering wakes.
	bool m_Driving = true;

private:
	Instant m_Slide{};
	Instant m_Grow{};
	Instant m_Fade{};

	bool m_SlideFar = false;
	bool m_GrowFull = false;
	bool m_FadeFull = false;
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

	[[nodiscard]] Result<void> Open(SceneStore& scene, ITextures&) override
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

	[[nodiscard]] Wake Advance(SceneStore&, ITextures&, Instant) override { return Wake::Never(); }
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

	[[nodiscard]] Result<void> Open(SceneStore& scene, ITextures&) override
	{
		const Result<void> authored = AuthorScene(scene);

		if (!authored)
		{
			return authored;
		}

		m_Turn.Open(scene.Now());

		return {};
	}

	[[nodiscard]] Wake Advance(SceneStore& scene, ITextures&, Instant now) override
	{
		if (!m_Driving)
		{
			return Wake::Never();
		}

		m_Driving = m_Turn.Drive(scene, m_Lanes.Turn.Marker, now);

		if (!m_Driving)
		{
			return Wake::Never();
		}

		return Wake::At(m_Turn.NextDue());
	}

private:
	TurnDriver m_Turn;
	bool m_Driving = true;
};

// The lanes, with a `Glass` panel and a `Smoke` panel laid over them.
//
// It drives the lanes exactly as `LanesGym` does, and that is the point rather than a saving: what a
// gathering material is worth looking at over is content that is *moving*, since a blur over a still
// picture is indistinguishable from a still picture somebody blurred once. So the instrument underneath
// is the same instrument, and what is added is the two panels and the shadow they cast.
//
// **The rotation lane is driven here and not in `LanesGym`**, which is the one place the two scenes
// differ. A turning quad is refused by the CPU renderer and one refusal loses the whole frame, so the
// lane can only be driven by a gym that has already given the CPU renderer up — and this one has,
// twice over, on the material and on the elevation. Turning it on in `LanesGym` would take the default
// scene away from `--backend=dump` as well, which is the backend it is most useful on.
class MaterialsGym final : public LanesGym
{
public:
	[[nodiscard]] std::string_view Name() const noexcept override { return ::Name(GymKind::Materials); }

	[[nodiscard]] Result<void> Open(SceneStore& scene, ITextures& textures) override
	{
		const Result<void> lanes = LanesGym::Open(scene, textures);

		if (!lanes)
		{
			return lanes;
		}

		const Result<MaterialOverlay> overlay = AuthorMaterialOverlay(scene, m_Lanes);

		if (!overlay)
		{
			return std::unexpected{ overlay.error() };
		}

		m_Turn.Open(scene.Now());

		return {};
	}

	[[nodiscard]] Wake Advance(SceneStore& scene, ITextures& textures, Instant now) override
	{
		const Wake lanes = LanesGym::Advance(scene, textures, now);

		if (!m_Driving)
		{
			return Wake::Never();
		}

		m_Driving = m_Turn.Drive(scene, m_Lanes.Turn.Marker, now);

		if (!m_Driving)
		{
			return Wake::Never();
		}

		return Sooner(lanes, Wake::At(m_Turn.NextDue()));
	}

private:
	TurnDriver m_Turn;
};
// An imported image, drawn four times, with the buffer swapped underneath it forever.
//
// **The swap is the half of this gym that no other instrument reaches.** Three copies moving under
// springs exercise the sampler; what they do not exercise is the lifetime, and the lifetime is the part
// Seam/Importer.h cannot check for itself — a texture is safe to forget only once the frame thread has
// moved past every snapshot that named it, and nothing in a running gyro asked that question until
// this gym did. So every swap here is a client's next buffer: mint, adopt, attach, give up the one
// that was there, and let the registry hold it until the watermark says the frame thread is past it.
//
// **A scene that stops swapping is a picture that stops alternating**, which is why the card carries a
// phase band at all. A swap that is dropped, that lands on the wrong buffer, or that frees pixels a
// frame is still sampling are three different things to see rather than one crash to bisect — and
// under a sanitiser the third is not a picture at all, which is the point of doing it here rather than
// discovering it under a client's window.
class CardGym final : public ISceneAuthor
{
public:
	[[nodiscard]] std::string_view Name() const noexcept override { return ::Name(GymKind::Card); }

	[[nodiscard]] Result<void> Open(SceneStore& scene, ITextures& textures) override
	{
		// Both buffers are drawn once and kept, because a swap should cost what a client's swap costs —
		// an adopt and a retire — rather than the drawing of a picture the gym could have had in hand.
		// The registry copies what it adopts, so these are the gym's own and outlive every id minted
		// from them.
		Result<Card> first = Card::Draw(CardTexels, AlphaMode::Premultiplied, CardPhase::First);

		if (!first)
		{
			return std::unexpected{ first.error() };
		}

		Result<Card> second = Card::Draw(CardTexels, AlphaMode::Premultiplied, CardPhase::Second);

		if (!second)
		{
			return std::unexpected{ second.error() };
		}

		m_Buffers[0].emplace(std::move(*first));
		m_Buffers[1].emplace(std::move(*second));

		// **Adopted before anything is authored**, because a node naming an id no renderer holds draws
		// nothing and says nothing — Core/Texture.h makes that silence deliberate, and a scene authored
		// around a failed import would spend the whole run being the one failure that reports itself as
		// a blank rectangle.
		const Result<TextureId> adopted = AdoptPhase(textures);

		if (!adopted)
		{
			return std::unexpected{ adopted.error() };
		}

		m_Texture = *adopted;

		const Result<CardScene> cards = AuthorCards(scene, m_Texture);

		if (!cards)
		{
			textures.Retire(m_Texture);

			return std::unexpected{ cards.error() };
		}

		m_Scene = *cards;

		const Instant now = scene.Now();

		m_Slide = Advanced(now, SlidePeriod);
		m_Grow = Advanced(now, GrowPeriod);
		m_Fade = Advanced(now, FadePeriod);
		m_Swap = Advanced(now, SwapPeriod);

		return {};
	}

	[[nodiscard]] Wake Advance(SceneStore& scene, ITextures& textures, Instant now) override
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
					m_Scene.Sliding, m_SlideFar ? m_Scene.SlideFar : m_Scene.SlideNear, Animate(SlideMotion)
				);
			});

			m_Slide = NextEdge(m_Slide, SlidePeriod, now);
		}

		if (m_Grow <= now)
		{
			m_Small = !m_Small;
			m_Driving =
				m_Driving && Tick(scene, m_Grow, [&](SceneCommit& commit) {
					return commit.Scale(m_Scene.Scaled, m_Small ? CardScaleSmall : CardScaleFull, Animate(GrowMotion));
				});

			m_Grow = NextEdge(m_Grow, GrowPeriod, now);
		}

		if (m_Fade <= now)
		{
			m_Dim = !m_Dim;
			m_Driving = m_Driving && Tick(scene, m_Fade, [&](SceneCommit& commit) {
							return commit.Fade(m_Scene.Faded, m_Dim ? CardFadeDim : CardFadeFull, Animate(FadeMotion));
						});

			m_Fade = NextEdge(m_Fade, FadePeriod, now);
		}

		if (m_Swap <= now && m_Driving)
		{
			m_Driving = Swap(scene, textures);
			m_Swap = NextEdge(m_Swap, SwapPeriod, now);
		}

		// A refused write latches the gym off and the freeze is the report, per `LanesGym`. A refused
		// *import* latches it off the same way, and is the one refusal here that is not this file being
		// wrong: a renderer's image table is bounded, so a gym that swapped faster than the watermark
		// moved would exhaust it — which is a real answer about pacing rather than a bug, and stopping
		// dead is how it gets read.
		if (!m_Driving)
		{
			return Wake::Never();
		}

		return Wake::At(std::min({ m_Slide, m_Grow, m_Fade, m_Swap }));
	}

private:
	// The client's whole buffer cycle, in the order that makes each step safe.
	//
	// **Adopt before attach, retire after.** The new id has to exist before a node can name it, or the
	// scene spends a frame drawing nothing; the old one has to stay adopted until nothing names it, or
	// the frame thread is sampling pixels the registry has been told to forget. Between those two the
	// order is forced, and it is the same order `wl_surface.attach` imposes on a client.
	[[nodiscard]] bool Swap(SceneStore& scene, ITextures& textures)
	{
		m_Phase = 1 - m_Phase;

		const Result<TextureId> next = AdoptPhase(textures);

		if (!next)
		{
			return false;
		}

		const TextureId previous = m_Texture;

		m_Texture = *next;

		// The same texel count every phase, because the two buffers are the same card drawn twice.
		const Rect<BufferSpace> source{ {}, { static_cast<float>(CardTexels), static_cast<float>(CardTexels) } };

		const bool attached = Tick(scene, m_Swap, [&](SceneCommit& commit) {
			return commit.Attach(m_Scene.Still, m_Texture, source) &&
			       commit.Attach(m_Scene.Scaled, m_Texture, source) &&
			       commit.Attach(m_Scene.Faded, m_Texture, source) && commit.Attach(m_Scene.Sliding, m_Texture, source);
		});

		// Only where every copy took it. A partial attach leaves nodes naming the old id, and retiring
		// it then is exactly the use-after-free this gym exists to make loud rather than to commit.
		if (attached)
		{
			textures.Retire(previous);
		}

		return attached;
	}

	[[nodiscard]] Result<TextureId> AdoptPhase(ITextures& textures)
	{
		const Card& buffer = *m_Buffers[static_cast<std::size_t>(m_Phase)];

		return textures.Adopt(buffer.Size(), buffer.Stride(), buffer.Bytes(), TextureAlpha::Premultiplied);
	}

	std::array<std::optional<Card>, 2> m_Buffers{};

	CardScene m_Scene{};
	TextureId m_Texture{};

	Instant m_Slide{};
	Instant m_Grow{};
	Instant m_Fade{};
	Instant m_Swap{};

	int m_Phase = 0;

	bool m_SlideFar = false;
	bool m_Small = true;
	bool m_Dim = true;

	bool m_Driving = true;
};

// The pointer glyph, specimened.
//
// **It authors a grid and drives one thing, and the asymmetry is the point.** Every other gym here is
// a motion with a picture attached; this one is a picture with a motion attached, because the question
// decision 152 left open is what the glyph should be and the only part of that a still frame cannot
// answer is whether the staircase crawls when it moves. So the grid is authored once and never
// touched, and one carriage slides across the bottom of it forever.
class PointerGym final : public ISceneAuthor
{
public:
	[[nodiscard]] std::string_view Name() const noexcept override { return ::Name(GymKind::Pointer); }

	[[nodiscard]] Result<void> Open(SceneStore& scene, ITextures& textures) override
	{
		const Result<PointerScene> pointers = AuthorPointers(scene, textures);

		if (!pointers)
		{
			return std::unexpected{ pointers.error() };
		}

		m_Scene = *pointers;

		// One period out rather than immediately, for `LanesGym`'s reason: the scene as authored is what
		// the first frame shows, and here that frame is the whole still half of the instrument.
		m_Glide = Advanced(scene.Now(), GlidePeriod);

		return {};
	}

	[[nodiscard]] Wake Advance(SceneStore& scene, ITextures&, Instant now) override
	{
		if (!m_Driving)
		{
			return Wake::Never();
		}

		if (m_Glide <= now)
		{
			m_Far = !m_Far;
			m_Driving = Tick(scene, m_Glide, [&](SceneCommit& commit) {
				return commit.Move(
					m_Scene.MovingArrow, m_Far ? m_Scene.SlideFar : m_Scene.SlideNear, Animate(GlideMotion)
				);
			});

			m_Glide = NextEdge(m_Glide, GlidePeriod, now);
		}

		// A refused write latches the gym off and the freeze is the report, which is `LanesGym`'s rule
		// for its reason: the write is against an id this gym created, so a refusal is this file being
		// wrong rather than a state to recover from.
		if (!m_Driving)
		{
			return Wake::Never();
		}

		return Wake::At(m_Glide);
	}

private:
	PointerScene m_Scene{};

	Instant m_Glide{};

	bool m_Far = false;
	bool m_Driving = true;
};
} // namespace

Result<std::unique_ptr<ISceneAuthor>> MakeGym(std::string_view name)
{
	const std::optional<GymKind> kind = GymNamed(name);

	if (!kind)
	{
		return Failure(EINVAL, "--gym is not one of the scenes --help lists");
	}

	// A switch with no default label, so a sixth enumerator is a build failure here rather than a name
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
		case GymKind::Card:
			return std::make_unique<CardGym>();
		case GymKind::Pointer:
			return std::make_unique<PointerGym>();
	}

	return Failure(EINVAL, "--gym is not one of the scenes --help lists");
}

std::span<const std::string_view> GymNames()
{
	return AllGymNames;
}
