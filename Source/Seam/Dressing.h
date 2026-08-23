#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "World/Material.h"

// What a material *is*, in numbers: the radius, the tint, `Smoke`'s contrast floor, decision 63's
// kind and expansion, and decision 34's ladder.
//
// **It is in `Seam` because two renderers have to produce one picture.** Decision 62's oracle draws
// the same frame twice and asserts the two agree, and decision 40 makes software rendering a device
// rather than a backend — so `Blit` and the Vulkan renderer are not one path and a spare, they are
// two implementations of the same look. A radius living in either of them is a radius the other can
// disagree with, and the disagreement is a panel that is visibly softer on the machine whose driver
// failed. `Seam`'s charter is every interface with more than one implementation *and the data
// crossing it*; this is the second half of that sentence.
//
// **It is not in `World/Material.h`, and the distance is the point.** Decision 33 forbids the call
// site naming a radius, a pass count, or a chain resolution. `World/Material.h` is the header the
// authoring side includes in order to say `Material::Glass`, so a radius in it is decision 33 lost by
// proximity rather than by argument — one `#include` away from the thing it exists to prevent, in a
// file `Scene` and `Protocol` both already have open. The enum stays where a shell can reach it and
// the numbers sit on the far side of a waist neither may name.
//
// **It is not in `Render`.** That is where the numbers would go by default and it is where the floor
// tier stops being real: `Blit` draws when there is no GPU driver loaded at all, and the frame it
// draws has to be the same frame.

// Decision 34's quality tier: which *structure* the pass chain has, never what it looks like.
//
// **The ladder is chain internal resolution, then pass count, and radius is not on it.** Decision 34
// admits the first rung precisely because a result about to be blurred tolerates a downscale, and
// decision 47 is what makes that true — a downscale performed in an encoded space darkens its
// result, so under sRGB-space compositing the cheapest lever would be the most conspicuous one.
//
// **The third rung is not here.** Decision 34 ends the ladder at *the material is not rendered — an
// opaque or simply tinted fill*, and that is `RenderMode::Floor`: decision 35's record-time check
// already picks it per frame, and a second enumerator here would be a second way to say one thing.
// So a tier says how the chain is built and a mode says whether there is one.
//
// Nothing chooses a tier yet. Decision 34 wants a startup capability probe that runs the real chain
// at two or three sizes, and until that exists `High` is what a caller passes and the probe is owed.
enum class Tier : std::uint8_t
{
	High,
	Medium,
	Low,
};

[[nodiscard]] constexpr std::string_view Name(Tier tier) noexcept
{
	switch (tier)
	{
		case Tier::High:
			return "High";
		case Tier::Medium:
			return "Medium";
		case Tier::Low:
			return "Low";
	}

	return "?";
}

// What one tier's chain is built out of. Two numbers, and neither of them is a radius.
struct TierFacts
{
	// Rung one. The chain runs at the target's resolution divided by this, so the extract pass is
	// also the downscale and the whole chain costs area over the square of it.
	std::uint32_t Divisor = 4;

	// Rung two. How many box passes run at that resolution, each of them separable, so the pass count
	// in flight is twice this.
	std::uint32_t Passes = 3;
};

// **Three box passes approximate a Gaussian to within a few percent**, which is the classical result
// and the reason the pass count starts there rather than at one large kernel.
//
// **Equal sigma is not an identical kernel, and the difference is why the ladder is in this order.**
// `SolveHalfWidth` holds the sigma exactly at every tier, so nothing here changes how far the blur
// spreads — but three boxes make a near-Gaussian and two make a triangle, so a tier that spends a
// pass has the same spread with a harder falloff. Dressing.Test.cpp measures both rungs against that
// shape: dropping the resolution moves it a few percent and dropping a pass halves it. Decision 34
// asserts that order; this is where it comes out of the arithmetic.
//
// **The resolution rung stops at eight rather than continuing, which is what makes the second rung
// necessary at all.** Past a divisor of eight the chain's own kernel is under two texels wide for the
// radii in the table below, and the bilinear upscale's footprint is then comparable to the blur's —
// so a panel gains a slow wobble as it moves, and the rung decision 34 admits *because* it is
// invisible stops being the invisible one. Cheaper is available; cheaper without a visible change is
// not, which is where the ladder has to change lever.
//
// **The chain also bottoms out here, and it is worth knowing before the probe is written.** The
// extract reads the region once at full resolution and the dressing writes it once, so about two
// passes over the panel's area are owed whatever the tier is; at a divisor of eight the blur itself
// costs a fraction of one. So the ladder's first rung buys most of what there is, its second buys a
// little, and what buys the rest is `RenderMode::Floor` — which is decision 34's own third rung, and
// this is the arithmetic that says why the ladder ends there rather than continuing.
[[nodiscard]] constexpr TierFacts Facts(Tier tier) noexcept
{
	switch (tier)
	{
		case Tier::High:
			return { .Divisor = 4, .Passes = 3 };
		case Tier::Medium:
			return { .Divisor = 8, .Passes = 3 };
		case Tier::Low:
			return { .Divisor = 8, .Passes = 2 };
	}

	return {};
}

// A premultiplied colour in linear light, relative to the composite's own reference white. Four
// named floats rather than an array because both a table row and a push constant read it by name,
// and a `[3]` that means alpha is one index typo from a colour nobody can explain.
struct MaterialTint
{
	float Red = 0.0F;
	float Green = 0.0F;
	float Blue = 0.0F;
	float Alpha = 0.0F;

	friend constexpr bool operator==(MaterialTint, MaterialTint) noexcept = default;
};

// What one material does to light. Decision 103's set, with the numbers that entry left open.
struct MaterialFacts
{
	// Decision 63's kind, and the first of the three jobs one declaration does. Every material in
	// decision 103's set is gathering and the pointwise column ships empty; the field is here because
	// decision 62 branches on it and a renderer must not infer it from whether the radius is zero.
	bool Gathering = false;

	// The standard deviation of the blur, in device pixels at a scale of one. **A sigma rather than a
	// kernel width**, because it is the only figure that survives decision 34's ladder — a width is a
	// property of one pass at one resolution and a sigma is a property of the look, which is the
	// distinction that entry rests on.
	float Sigma = 0.0F;

	// Applied over the blurred backdrop, so the alpha is how much of the backdrop does *not* survive.
	MaterialTint Tint{};

	// `Smoke`'s obligation and nothing else's: the least the tint's alpha may fall to, whatever the
	// tint says, so that the material stays legible over the worst backdrop it can be given.
	//
	// **A constant rather than a measurement of what is behind it**, which decision 103 rejects by
	// name: a tint that adapts to backdrop luminance breathes, and a panel over a playing film
	// changing tone at every cut is *quality does not visibly fluctuate* failing where a person is
	// looking. So the worst case is assumed once instead of sampled per frame.
	float ContrastFloor = 0.0F;
};

// SPEC: the placeholder numbers, and they are unmeasured.
//
// Decision 103 says in as many words that the tint values, the radii, and `Smoke`'s contrast floor
// are numbers and want a screen — with GTK, Qt, and an Xwayland application on it at once, which is
// the same screen decision 105 owes the floor radius to. What is settled by that entry and built
// here is which materials exist, what each is for, and the mechanism that renders them. These four
// rows are defensible and they are not decided.
//
// `Glass` is thin because it sits over content the user arranged. `Smoke` is dark and heavier
// because it sits over content gyro did not choose, and its floor is what makes that an arithmetic
// difference rather than a stylistic one.
//
// **`Smoke`'s stated alpha is deliberately below its floor**, which is decision 103's sentence made
// mechanical: its opacity comes from a worst-case contrast requirement *rather than from taste*, so
// the taste number is what the row says and the floor is what actually decides. A row whose stated
// alpha already cleared its floor would leave the floor decorative and nobody would notice until the
// day somebody tuned the tint down.
[[nodiscard]] constexpr MaterialFacts Facts(Material material) noexcept
{
	switch (material)
	{
		case Material::None:
			return {};
		case Material::Glass:
			return { .Gathering = true,
				     .Sigma = 24.0F,
				     .Tint = { .Red = 0.18F, .Green = 0.18F, .Blue = 0.19F, .Alpha = 0.28F },
				     .ContrastFloor = 0.0F };
		case Material::Smoke:
			return { .Gathering = true,
				     .Sigma = 32.0F,
				     .Tint = { .Red = 0.03F, .Green = 0.03F, .Blue = 0.04F, .Alpha = 0.45F },
				     .ContrastFloor = 0.62F };
	}

	return {};
}

// The tint a material composites with: its own, except where a contrast floor raises the alpha.
// Premultiplied, so raising the alpha scales the colour with it rather than lightening the result.
[[nodiscard]] constexpr MaterialTint ResolvedTint(MaterialFacts facts) noexcept
{
	const float alpha = facts.Tint.Alpha < facts.ContrastFloor ? facts.ContrastFloor : facts.Tint.Alpha;
	const float scale = facts.Tint.Alpha > 0.0F ? alpha / facts.Tint.Alpha : 0.0F;

	return { .Red = facts.Tint.Red * scale,
		     .Green = facts.Tint.Green * scale,
		     .Blue = facts.Tint.Blue * scale,
		     .Alpha = alpha };
}

// The variance of one separable box pass of half-width `halfWidth`, sampled on the integer grid.
//
// **Fractional, because an integer half-width would put the radius back on decision 34's ladder.**
// The kernel is one at every whole offset out to `floor(w)` and the fractional remainder at the two
// offsets past it, normalised by its own sum — so a tier that halves the chain's resolution can
// halve the half-width exactly rather than to the nearest texel, and the sigma the chain produces is
// the same number at every tier instead of the nearest number the grid allowed.
//
// This is the *exact* variance of that discrete kernel rather than the continuous box's `((2w+1)²−1)
// /12`, which is what lets `SolveHalfWidth` be an inverse rather than an approximation.
[[nodiscard]] inline float BoxVariance(float halfWidth) noexcept
{
	if (halfWidth <= 0.0F)
	{
		return 0.0F;
	}

	const auto whole = static_cast<std::int32_t>(std::floor(halfWidth));
	const float fraction = halfWidth - static_cast<float>(whole);

	float sum = 1.0F;
	float second = 0.0F;

	for (std::int32_t offset = 1; offset <= whole; ++offset)
	{
		const auto position = static_cast<float>(offset);

		sum += 2.0F;
		second += 2.0F * position * position;
	}

	const auto edge = static_cast<float>(whole + 1);

	sum += 2.0F * fraction;
	second += 2.0F * fraction * edge * edge;

	return second / sum;
}

// The half-width one pass needs so that `passes` of them come to `sigma`, in the chain's own texels.
//
// **This is the whole of decision 34's promise that the tier changes the pass structure and never
// the look.** Independent blurs add variances, so `passes` boxes of variance `v` give `sigma² =
// passes · v` — and the half-width falls out by inversion rather than being a number per tier. Rung
// one moves the chain's resolution and rung two moves the count; this absorbs both, and
// Dressing.Test.cpp asserts that the sigma the three tiers produce is one number.
//
// **Bisection rather than the continuous box's closed form**, because the closed form inverts a
// kernel this does not use: the discrete kernel's variance is a piecewise-quadratic staircase in the
// half-width, and solving the continuous approximation and rounding is exactly the error the
// fractional width exists to avoid. Thirty halvings of a bracket that starts at the sigma itself is
// well past float precision and costs microseconds at a table lookup nobody runs per frame.
[[nodiscard]] inline float SolveHalfWidth(float sigma, std::uint32_t passes) noexcept
{
	if (sigma <= 0.0F || passes == 0)
	{
		return 0.0F;
	}

	const float wanted = (sigma * sigma) / static_cast<float>(passes);

	// A single box of half-width `w` has variance below `w²/3`, so a bracket of `3·sqrt(wanted)`
	// contains the answer for every sigma. Cheap to overshoot and fatal to undershoot.
	float low = 0.0F;
	float high = 3.0F * std::sqrt(wanted) + 1.0F;

	for (int step = 0; step < 40; ++step)
	{
		const float middle = 0.5F * (low + high);

		if (BoxVariance(middle) < wanted)
		{
			low = middle;
		}
		else
		{
			high = middle;
		}
	}

	return 0.5F * (low + high);
}

// The chain one material runs at one tier, in the chain's texels — everything a renderer needs to
// build the passes, and nothing a caller may name.
struct ChainPlan
{
	std::uint32_t Divisor = 1;
	std::uint32_t Passes = 0;
	float HalfWidth = 0.0F;

	// **`For` rather than a free `Plan`, which is the idiom `QuadVariant::For` already uses** — and
	// the free spelling collides with Compositor/Schedule.h's `Plan`, which is a real type in the
	// same global namespace and a better claim on the word.
	[[nodiscard]] static ChainPlan For(Material material, Tier tier) noexcept
	{
		const MaterialFacts facts = Facts(material);

		if (!facts.Gathering || facts.Sigma <= 0.0F)
		{
			return {};
		}

		const TierFacts structure = Facts(tier);
		const float sigma = facts.Sigma / static_cast<float>(structure.Divisor);

		return { .Divisor = structure.Divisor,
			     .Passes = structure.Passes,
			     .HalfWidth = SolveHalfWidth(sigma, structure.Passes) };
	}
};

// Decision 63's declared expansion: how far past its own extent a material's output reaches, in
// device pixels, so that a change *behind* it dirties the right region and no more.
//
// **It is exact rather than a truncation, and that is an argument for boxes beyond their cost.** A
// Gaussian has infinite support, so declaring a bound for one means picking a number of standard
// deviations and accepting a fringe below it — under-declared, which decision 63 says leaves a trail
// at the edge of something that moved. A box chain is compactly supported: `passes` boxes of
// half-width `w` reach exactly `passes · w`, and the bilinear taps the downscale and the upscale
// each use reach one chain texel further. Nothing here is rounded in the unsafe direction.
//
// **Nothing on the damage path consumes this yet, and that is deliberate.** Frame/Evaluator.h reports
// the whole output or nothing under decision 101, because per-node damage needs an identity the node
// record does not carry — so a whole-output region already contains every neighbourhood a chain can
// read and the expansion is satisfied vacuously. The number is declared anyway, here, because
// decision 63 is explicit that the declaration exists *in order to be contradicted by the verifier*,
// and because the alternative to declaring it now is a renderer that clamps its own sampling to hide
// an expansion nobody wrote — a correctness rule in the one place nobody would look for one.
[[nodiscard]] inline float Expansion(Material material, Tier tier) noexcept
{
	const ChainPlan plan = ChainPlan::For(material, tier);

	if (plan.Passes == 0)
	{
		return 0.0F;
	}

	const float reach = static_cast<float>(plan.Passes) * plan.HalfWidth + 2.0F;

	return reach * static_cast<float>(plan.Divisor);
}
