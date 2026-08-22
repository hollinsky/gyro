#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// What a node is dressed in.
//
// **It is in `World` because both halves of the world name it, and in `World` rather than `Core`
// because it is authored vocabulary rather than a primitive.** A material is declared by a client or
// a shell, so the authoring side produces one and the frame thread consumes it — Docs/Structure.md's
// rule, from decision 87, is that such a type lives below both waists rather than in `Seam`, which
// neither `Protocol` nor `Scene` may name. Core/Texture.h makes the same argument for `TextureId` and
// lands in `Core`; the difference is decision 91's line between the two: `Core` holds primitives that
// happen to be world-visible, and `World` holds what a node *is* and what it is dressed in. Decision
// 95 is what let this move — the set is a field on every node rather than a node kind, so it is one of
// World/Node.h's own fields, and a record's field does not live in another module.
//
// Seam/Renderer.h still names it, because a draw item carries the dressing it was recorded with. That
// edge points down, which is the direction that stays legal as the vocabulary grows.

// How an item is dressed. Decision 33's closed set, named rather than parameterized, and populated by
// decision 103.
//
// **Named by what it does to light, never by the role it is put to and never by degree.** That rule
// is decision 103's and it decides most of what is and is not below. A role name — `Sidebar`,
// `Titlebar`, `Menu` — is decision 51's fisheye-dock failure one field over from where decision 95
// refused it for node kinds: the moment the vocabulary knows what a sidebar is, the arrangements that
// are not sidebars stop being sayable about. A degree name — `Thin`, `Regular`, `Thick` — is decision
// 33's rejected parameterized call with the number spelled in English, which is what `UIBlurEffect`
// became, and it hands back at the vocabulary the radius the call site was refused.
//
// **No parameters, and their absence is the same decision.** Decision 33 is that the shell names a
// material and gyro decides what it costs; a radius or an amount beside this field would be the
// parameterized filter call that decision refuses, arriving by the back door. The tint values, the
// radii, and `Smoke`'s contrast floor are all gyro's, and they live with the pass chain rather than
// here — this file says which materials exist, the way Animation/Author/Catalog.h says which
// transitions do.
enum class Material : std::uint8_t
{
	None,

	// A blurred, tinted backdrop, over content the user arranged. The workhorse: shell panels, the
	// dock, popovers, notifications, the launcher, an overview's wallpaper. It can be thin because
	// the person looking at it chose what is behind it.
	Glass,

	// Dark, heavier, and legible over anything. The difference from `Glass` is not weight, it is
	// obligation: this sits over content gyro did not choose — a film, a white page, a photograph —
	// so its opacity comes from a worst-case contrast floor rather than from taste. A volume overlay
	// during a film is the case, and it is the one a lighter material cannot be substituted into.
	Smoke,
};

inline constexpr std::size_t MaterialCount = 3;

// **Five, and tighter than Animation/Author/Motion.h's seven**, which inverts the usual intuition and
// is decision 103's first reason. A blur difference is visible in a still screenshot; a spring
// difference needs motion to see. So the incohesion decision 13 describes — two nearly-identical
// entries, each defensible, the system subtly wrong forever after — is *cheaper* to perceive on this
// axis than on the one that argument was written for. A dock and a top bar whose blurs differ by ten
// percent is a screen anyone can see is not one machine.
//
// The second reason is that growth is not free the way a transition's is: every gathering material is
// a pass boundary decision 62 can never fuse away, plus a row in decision 34's cost table and a row in
// decision 63's expansion table. Asserted rather than described, so a fourth is a deliberate act with
// a diff that says so.
static_assert(static_cast<std::size_t>(Material::Smoke) + 1 == MaterialCount);
static_assert(MaterialCount <= 5, "Decision 103's cap. A fifth is the last one that is not a symptom");

// The vocabulary in one place a pass over it can name, which decision 63's damage verifier is: it
// runs headless over the material vocabulary, so the vocabulary has to be a thing that can be swept.
inline constexpr std::array<Material, MaterialCount> AllMaterials{
	Material::None,
	Material::Glass,
	Material::Smoke,
};

// The list is the enumeration in order, so a sweep over it is a sweep over the enum. The size is
// fixed by MaterialCount already; what is left to go wrong is a duplicate standing in for the entry
// somebody meant to add, which reads correctly and silently exempts a material from everything above.
static_assert([] {
	for (std::size_t index = 0; index < AllMaterials.size(); ++index)
	{
		if (static_cast<std::size_t>(AllMaterials[index]) != index)
		{
			return false;
		}
	}

	return true;
}());

[[nodiscard]] constexpr std::string_view Name(Material material) noexcept
{
	switch (material)
	{
		case Material::None:
			return "none";
		case Material::Glass:
			return "glass";
		case Material::Smoke:
			return "smoke";
	}

	return "unknown";
}

// Whether the material reads a neighbourhood, which is decision 63's one declaration doing three jobs
// — fusibility under decision 62, damage expansion, and whether a result can be cached across frames.
//
// **It is fixed per material and does not vary with tier**, which forecloses the cute answer: a
// material could gather at high tiers and go pointwise at low ones, and it may not, because
// fusibility depends on this. A tier-dependent classification makes the *variant set* tier-dependent
// and couples the two axes decision 62 spends a paragraph holding apart — fusion is invisible and
// buys performance, tier is visible and is chosen once and held.
//
// **Everything in the set gathers, and the empty pointwise column is decision 103's answer rather
// than an omission.** What Open.md wanted from the question was a size for decision 62's variant
// lattice, and empty gives a much better one: the pointwise run per item is the colour-state
// conversion, the corner-radius mask, per-node opacity, and the dim on an output with no backlight —
// four, in one fixed order, so contiguous runs number ten and a gather splits a chain into at most
// two of them. Tens of variants precompiled at startup rather than 2ⁿ over the vocabulary.
//
// The rule for the first pointwise material that arrives: it has to justify why it is not a `Solid`
// node or a field on the node, since those already do every pointwise thing the scene can express.
// Exactly one justification survives — decision 95's, that the treatment must track the node's extent
// and radius exactly, so a second node carrying it is a second transform that agrees only while
// nothing moves. A backdrop desaturation and a multiplicative tint are the two candidates that clear
// it, and neither is needed by anything on a screen today.
//
// A switch with no default label, so a new enumerator is a build failure rather than inheriting a
// classification nobody chose — which is the direction that matters, since gathering is the expensive
// answer and a silent default of pointwise would be a fusion variant that reads its own unwritten
// output.
[[nodiscard]] constexpr bool IsGathering(Material material) noexcept
{
	switch (material)
	{
		case Material::None:
			return false;
		case Material::Glass:
		case Material::Smoke:
			return true;
	}

	return false;
}

// The contract everything downstream assumes.

// Two materials are two dressings, which is the assertion that fails first if the enum is ever
// widened into a bitfield.
static_assert(Name(Material::None) == "none");
static_assert(Name(Material::Glass) == "glass");
static_assert(Name(Material::Smoke) == "smoke");

// `None` is not an effect at all, so it cannot be the thing that segments a fusible run. Stated as an
// assertion because it is the one entry whose classification is structural rather than a design
// choice: a node with no dressing has no pass to place.
static_assert(!IsGathering(Material::None));

// Checked over the whole vocabulary rather than about the entries named, which is the direction that
// catches the change a default would make and a diff would not show as one: a material arriving in
// the pointwise column without the argument above having been made for it.
static_assert(
	[] {
		for (const Material material : AllMaterials)
		{
			if (material != Material::None && !IsGathering(material))
			{
				return false;
			}
		}

		return true;
	}(),
	"A pointwise material has to argue it is not a Solid node or a node field first — decision 103"
);
