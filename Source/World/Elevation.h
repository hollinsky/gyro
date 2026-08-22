#pragma once

#include <cstdint>
#include <string_view>

// How far a node sits off what is behind it.
//
// **It is in `World` for World/Material.h's reason and it is the same kind of field.** Decision 95
// puts an elevation on every node, so the authoring side declares one and the frame thread consumes
// it — decision 87's rule is that such a type lives below both waists rather than in `Seam`, and
// decision 91's line puts what a node *is* here rather than in `Core`.

// Which level a node is lifted to. Named levels, not a blur radius and an offset.
//
// **The shell says which windows are lifted and gyro decides what a level costs**, which is decision
// 33's rule arriving on a third axis after the material set and the motion catalog: a shell can no
// more name a shadow radius than it can name a damping ratio. The alternative is a parameter pair at
// every shadow site, and what that produces is a desktop where each surface was tuned by whoever
// added it — a menu whose shadow is heavier than the dialog above it, for no reason a user could
// state.
//
// **It has one member and that is the honest state of it.** Docs/Open.md's *elevation set* entry
// holds the contents open — how many levels, what each is for, and what a level does at the floor
// tier — and wants a review with a screen beside the material vocabulary rather than an argument
// here. What is fixed now is the shape: an elevation is a *name on a node*, so adding one is an
// enumerator and reaches no call site that does not switch on it.
//
// **`None` is also what a self-decorating client gets, and the vocabulary does not know that.**
// Decision 96: a client that drew a shadow into its own buffer already has one, and two shadows are
// worse than either. Window management holds the negotiated decoration mode and sets this field from
// it; nothing here carries a notion of client-side decoration, so the scene stays a scene. That is
// the same separation that keeps a blur radius out of `Material`.
enum class Elevation : std::uint8_t
{
	None,
};

[[nodiscard]] constexpr std::string_view Name(Elevation elevation) noexcept
{
	switch (elevation)
	{
		case Elevation::None:
			return "none";
	}

	return "unknown";
}

// Two elevations are two levels even where only one is populated, which is the assertion that fails
// first if the enum is ever widened into a depth in millimetres.
static_assert(Name(Elevation::None) == "none");
