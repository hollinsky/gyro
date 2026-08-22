#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// How far a node sits off what is behind it.
//
// **It is in `World` for World/Material.h's reason and it is the same kind of field.** Decision 95
// puts an elevation on every node, so the authoring side declares one and the frame thread consumes
// it — decision 87's rule is that such a type lives below both waists rather than in `Seam`, and
// decision 91's line puts what a node *is* here rather than in `Core`.

// Which level a node is lifted to. Named levels, not a blur radius and an offset. Decision 104.
//
// **A level is a height, and there is one light.** From a height gyro derives the offset, the
// softness, and the opacity, by two constants held for the whole system; the light is parallel and
// vertical rather than a point source somewhere on the screen, so two windows at one level cast
// identical shadows wherever they sit. That is Animation/Author/Motion.h's `MotionTable` : `Motion`
// exactly, including why the granularity is structural rather than a rule a parser has to keep:
// configuration retunes the two constants, which moves every level together, and configuration cannot
// reach a level because a level has nowhere to put a radius.
//
// **What a person sees, which is the whole argument for the shape.** A Linux desktop today is every
// toolkit's client-drawn shadow being its own light — a GTK menu's shadow falling one way over a Qt
// window's falling another, at a different softness and a different weight — and the screen reads as
// a collage of applications rather than as one surface with things on it. Unlike a motion, that is
// visible in a screenshot before anything moves.
//
// **The enumeration order is the height order**, which is what makes the assertion at the bottom mean
// something and what a comparison between two levels is entitled to assume. The heights themselves are
// numbers and want a screen (Docs/Open.md); this file fixes which levels exist and what each is for.
//
// **`None` is also what a self-decorating client gets, and the vocabulary does not know that.**
// Decision 96: a client that drew a shadow into its own buffer already has one, and two shadows are
// worse than either. Window management holds the negotiated decoration mode and sets this field from
// it; nothing here carries a notion of client-side decoration, so the scene stays a scene. That is
// the same separation that keeps a blur radius out of `Material`.
enum class Elevation : std::uint8_t
{
	// In the plane. The wallpaper, a tiled or fullscreen window, and decision 96's client-decorated
	// window.
	None,

	// Lifted off the desktop and left there. An ordinary floating window.
	Resting,

	// Lifted off everything; unattached and transient. A menu, a tooltip, a notification, an overlay.
	Floating,
};

inline constexpr std::size_t ElevationCount = 3;

// **Four, and the cap is perceptual rather than economic**, which is where this differs from
// World/Material.h's. Decision 104's shadow is analytic, so a level costs almost nothing to have —
// what a fifth would cost is a level nobody can rank. Shadow ranks depth badly, and past about three
// lifted levels a person reads *floating* and stops counting; Material Design's twenty-four dp steps
// resolve to roughly four distinguishable looks in practice. A level that reads the same as its
// neighbour leaves a shell author choosing between them, and therefore choosing inconsistently.
//
// The obvious fourth is a middle level for a dialog on its window, which macOS distinguishes from a
// menu clearly. Decision 104 leaves it out as the safer read against the cap, and it is recoverable
// as a single enumerator that reaches no call site not already switching on this — which is the shape
// decision 95 fixed for exactly this kind of filling-in.
static_assert(static_cast<std::size_t>(Elevation::Floating) + 1 == ElevationCount);
static_assert(ElevationCount <= 4, "Decision 104's cap, and it is about what an eye can rank");

// The vocabulary in one place a pass over it can name — the light table's own sweep, when it lands,
// and the golden-image sweep that has to render each level once.
inline constexpr std::array<Elevation, ElevationCount> AllElevations{
	Elevation::None,
	Elevation::Resting,
	Elevation::Floating,
};

// The list is the enumeration in order, so a sweep over it is a sweep over the enum. What is left to
// go wrong once the count is fixed is a duplicate standing in for the entry somebody meant to add.
static_assert([] {
	for (std::size_t index = 0; index < AllElevations.size(); ++index)
	{
		if (static_cast<std::size_t>(AllElevations[index]) != index)
		{
			return false;
		}
	}

	return true;
}());

[[nodiscard]] constexpr std::string_view Name(Elevation elevation) noexcept
{
	switch (elevation)
	{
		case Elevation::None:
			return "none";
		case Elevation::Resting:
			return "resting";
		case Elevation::Floating:
			return "floating";
	}

	return "unknown";
}

// The contract everything downstream assumes.

// Two elevations are two levels, which is the assertion that fails first if the enum is ever widened
// into a depth in millimetres.
static_assert(Name(Elevation::None) == "none");
static_assert(Name(Elevation::Resting) == "resting");
static_assert(Name(Elevation::Floating) == "floating");

// The enumeration order is the height order, stated above as a rule and fixed here as a fact, so the
// light table that lands later has a constraint to satisfy rather than a comment to remember. A level
// inserted in the wrong position is the change this catches — it reads correctly at every call site
// and reverses two shadows on screen.
static_assert(Elevation::None < Elevation::Resting && Elevation::Resting < Elevation::Floating);
