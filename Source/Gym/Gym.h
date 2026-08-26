#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "Core/Result.h"
#include "Scene/Author.h"

// The scenes gyro authors for itself: a handful of nodes moving under springs, with no protocol
// behind them.
//
// **This is a module rather than files under `Compositor`, and the invariant is what earns it.** A gym
// authors a scene the way a shell would — a store, a commit, a named motion, a wake — and reaches none
// of `Compositor`'s vocabulary: no ring, no `SCHED_FIFO`, no backend, no output negotiation. It is
// `PORTABLE DISPATCH` and therefore runs and is tested on a machine with no GPU, no seat, and no
// compositor, which is the tier the whole instrument case rests on. Left in the composition root it
// would be portable by accident: the root is the one module that is not, so the first time something
// here reached for a platform header nothing would say so. The boot splash is the second scene with
// exactly this shape — authored by gyro, against no client — and the alternative worth naming is that
// it could have been a directory inside `Scene`, which is refused because `Scene` is the machinery a
// gym is a *caller* of, and a module holding its own call sites stops being the thing under test.
//
// **A gym is two verbs and a wake.** `Open` authors the tree once, against an output set the store
// already carries — the placement of an output in global space is the composition root's, being the
// only thing that knows both sides of both waists. `Advance` retargets whatever is due and answers
// when it next wants to be called; the caller sleeps that long. Nothing here publishes, paces, or
// reads a return channel: a gym is an author and the loop around it is the root's, which is the same
// division `Frame/Loop.h` already has on the other thread.
//
// **A gym that never settles is the point, and one that does is the control.** `Animation`'s settling
// is exact — `IsAtRest` is zero offset and zero velocity, and `WakeAt` computes an analytic settle
// instant — so there is no way to author perpetual motion out of a single spring, and the only honest
// way to keep a scene moving is to keep retargeting it. That turns out to be the better instrument
// anyway: retargeting on a tick through `SceneCommit` under an origin is decision 89's eager path run
// at rate, which is the traffic Docs/Open.md's publication-pacing question wants measured rather than
// argued. `Settle` is the other half of the same measurement — it authors once and answers `Never()`
// forever after, which is the only instrument for *doing nothing costs nothing* and is what
// `--frames=N`'s idle stop is read against.
//
// **Two of the four do not draw under the CPU renderer, and that is stated in the table rather than
// discovered.** `Blit/Blit.cpp` refuses a material, an elevation, a nonzero corner radius, and a quad
// that is not axis-aligned; one refused item fails the whole `Record`, so the frame is lost rather
// than degraded. That makes `turn` and `materials` instruments for the Vulkan renderer specifically,
// and under `--backend=dump` they write nothing at all for as long as they are moving. `DrawsOnCpu`
// below is what lets the composition root say so at startup instead of leaving somebody to bisect an
// empty directory.

// Which gym. A closed vocabulary with a name, a sentence, and a claim about the renderer, in the shape
// World/Material.h and Animation/Author/Catalog.h already use — a new entry is an enumerator, a switch
// case the compiler demands, and a row in the array below.
enum class GymKind : std::uint8_t
{
	// The four sprung channels, one lane each, retargeting forever. The default, and the one the
	// pacing question is measured on.
	Lanes,

	// The same instrument authored once and left to finish. The only scene that folds to idle.
	Settle,

	// The rotation lane, driven. Wants the Vulkan renderer.
	Turn,

	// The lanes with a `Glass` and a `Smoke` panel over them, and the rotation lane driven under
	// them. Wants the Vulkan renderer.
	Materials,

	// An imported image, drawn four times, with the buffer swapped underneath it forever. The only gym
	// that needs a texture to exist, and therefore the only one that can fail for a reason that is not
	// the store.
	Card,
};

inline constexpr std::size_t GymCount = 5;

static_assert(static_cast<std::size_t>(GymKind::Card) + 1 == GymCount);

inline constexpr std::array<GymKind, GymCount> AllGyms{
	GymKind::Lanes, GymKind::Settle, GymKind::Turn, GymKind::Materials, GymKind::Card,
};

// The list is the enumeration in order, so a sweep over it is a sweep over the enum. The size is fixed
// by `GymCount`; what is left to go wrong is a duplicate standing in for the entry somebody meant to
// add, which reads correctly and silently drops a gym from every sweep below.
static_assert([] {
	for (std::size_t index = 0; index < AllGyms.size(); ++index)
	{
		if (static_cast<std::size_t>(AllGyms[index]) != index)
		{
			return false;
		}
	}

	return true;
}());

// A switch with no default label, so a new enumerator is a build failure rather than inheriting a
// name, a description, or — the one that matters — a claim that the CPU renderer can draw it.
[[nodiscard]] constexpr std::string_view Name(GymKind gym) noexcept
{
	switch (gym)
	{
		case GymKind::Lanes:
			return "lanes";
		case GymKind::Settle:
			return "settle";
		case GymKind::Turn:
			return "turn";
		case GymKind::Materials:
			return "materials";
		case GymKind::Card:
			return "card";
	}

	return "unknown";
}

// One line, for whatever prints the list to somebody choosing from it.
[[nodiscard]] constexpr std::string_view Describe(GymKind gym) noexcept
{
	switch (gym)
	{
		case GymKind::Lanes:
			return "translation, scale and opacity retargeting forever, one lane each";
		case GymKind::Settle:
			return "the same lanes authored once, so the system folds to idle when they finish";
		case GymKind::Turn:
			return "the rotation lane, driven; no CPU composite rasterizes a rotated quad";
		case GymKind::Materials:
			return "glass and smoke panels over the lanes; no CPU composite draws a material";
		case GymKind::Card:
			return "a test card imported and drawn four ways, its buffer swapped forever";
	}

	return "unknown";
}

// Whether every item this gym authors is one `Blit` can express.
//
// **False is not a degradation, it is an absence.** One refused item fails the whole `Record`, so a
// gym answering false under `--backend=dump` produces no frames at all rather than frames missing a
// node — which is a directory that stays empty, and is exactly the symptom somebody would otherwise
// file as a bug against the dump backend. A caller that has a CPU renderer bound and a gym that
// answers false owes the person a sentence before the first frame is not written.
[[nodiscard]] constexpr bool DrawsOnCpu(GymKind gym) noexcept
{
	switch (gym)
	{
		case GymKind::Lanes:
		case GymKind::Settle:
			return true;

		// The one gym whose CPU answer is the *interesting* one: `Blit` is the only renderer that
		// samples a texture today, so this is the gym that draws there and nowhere else. The Vulkan arm
		// of Docs/Open.md's minting entry is what makes that temporary.
		case GymKind::Card:
			return true;

		// Refused by `Blit::Classify` on the quad: the corners of a rotated chain are not bit-identical
		// in pairs, and the comparison is exact float equality deliberately — a tolerance there would
		// draw a rotated logo unrotated and be invisible until somebody measured the picture.
		case GymKind::Turn:
			return false;

		// Refused by `Blit::Classify` twice over, on the material and on the elevation.
		case GymKind::Materials:
			return false;
	}

	return false;
}

// The names, in the enumeration's order. A second spelling of the vocabulary, which is a thing this
// codebase otherwise refuses — it is here because the fixed interface below answers in names rather
// than in kinds, and the assertion underneath is what keeps the two from drifting.
inline constexpr std::array<std::string_view, GymCount> AllGymNames{
	"lanes", "settle", "turn", "materials", "card",
};

static_assert([] {
	for (std::size_t index = 0; index < AllGyms.size(); ++index)
	{
		if (AllGymNames[index] != Name(AllGyms[index]))
		{
			return false;
		}
	}

	return true;
}());

// The kind a name spells, or nothing.
[[nodiscard]] constexpr std::optional<GymKind> GymNamed(std::string_view name) noexcept
{
	for (const GymKind gym : AllGyms)
	{
		if (Name(gym) == name)
		{
			return gym;
		}
	}

	return std::nullopt;
}

// The gym a name spells, or an error naming the vocabulary. A gym is one `ISceneAuthor`
// ([Scene/Author.h](../Scene/Author.h)); this is the factory for the ones gyro authors for itself.
[[nodiscard]] Result<std::unique_ptr<ISceneAuthor>> MakeGym(std::string_view name);

[[nodiscard]] std::span<const std::string_view> GymNames();
