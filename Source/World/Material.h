#pragma once

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

// How an item is dressed. Decision 33's closed set, named rather than parameterized.
//
// **It has one member and that is the honest state of it.** The vocabulary is open — Docs/Open.md's
// *material vocabulary* entry wants designing with the motion catalog, because a material and the
// transitions that reveal it are one problem — and inventing entries here would be inventing the
// storage for a decision nobody has taken. What is fixed now is the shape decision 78 says cannot be
// widened later: a material is a *name on an item*, so adding one is an enumerator and reaches no
// call site that does not switch on it.
//
// **No parameters, and their absence is the same decision.** Decision 33 is that the shell names a
// material and gyro decides what it costs; a radius or an amount beside this field would be the
// parameterized filter call that decision refuses, arriving by the back door. The snapshot does carry
// an animated blur channel, and what that channel drives is a property of whichever material claims
// it — which is a question for the catalog rather than for this field.
enum class Material : std::uint8_t
{
	None,
};

[[nodiscard]] constexpr std::string_view Name(Material material) noexcept
{
	switch (material)
	{
		case Material::None:
			return "none";
	}

	return "unknown";
}

// Two materials are two dressings even where neither is populated, which is the assertion that fails
// first if the enum is ever widened into a bitfield.
static_assert(Name(Material::None) == "none");
