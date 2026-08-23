#include "Seam/Dressing.h"

#include <array>
#include <cmath>

#include "Testing/Test.h"

namespace
{

// What a chain of `passes` boxes of this half-width actually produces, in the chain's texels.
[[nodiscard]] float ChainSigma(float halfWidth, std::uint32_t passes) noexcept
{
	return std::sqrt(BoxVariance(halfWidth) * static_cast<float>(passes));
}

// The same figure back in device pixels, which is the number a person perceives.
[[nodiscard]] float DeviceSigma(Material material, Tier tier) noexcept
{
	const ChainPlan plan = ChainPlan::For(material, tier);

	return ChainSigma(plan.HalfWidth, plan.Passes) * static_cast<float>(plan.Divisor);
}

constexpr std::array Tiers{ Tier::High, Tier::Medium, Tier::Low };
constexpr std::array Gathering{ Material::Glass, Material::Smoke };

} // namespace

// Decision 34's whole promise, and the reason `SolveHalfWidth` is an inverse rather than a table.
// The ladder moves the chain's resolution and then its pass count; if either changed the sigma, the
// cheapest lever would be the most conspicuous one and a machine under pressure would look different
// rather than cost less.
GYRO_TEST(Dressing, EveryTierProducesTheSameBlurAsEveryOther)
{
	for (const Material material : Gathering)
	{
		const float wanted = Facts(material).Sigma;

		for (const Tier tier : Tiers)
		{
			const float produced = DeviceSigma(material, tier);

			GYRO_CHECK(std::abs(produced - wanted) < 0.01F);
		}
	}
}

// The two rungs are separable, which is what makes them a ladder rather than one lever with two
// names. Medium keeps High's pass count and halves the resolution; Low keeps Medium's resolution and
// drops a pass.
GYRO_TEST(Dressing, TheLadderMovesResolutionFirstAndPassCountSecond)
{
	GYRO_CHECK(Facts(Tier::Medium).Divisor > Facts(Tier::High).Divisor);
	GYRO_CHECK(Facts(Tier::Medium).Passes == Facts(Tier::High).Passes);

	GYRO_CHECK(Facts(Tier::Low).Divisor == Facts(Tier::Medium).Divisor);
	GYRO_CHECK(Facts(Tier::Low).Passes < Facts(Tier::Medium).Passes);
}

// **Equal sigma is not an identical kernel, and this is what says by how much.** Three boxes
// approximate a Gaussian closely and two make a triangle, so a tier that spends a pass has the same
// spread with a harder falloff. Measured as the reach in sigmas, which is the shape figure a
// compactly supported kernel has: rung one moves it a few percent and rung two halves it.
//
// That is decision 34's ladder order coming out of the arithmetic rather than being asserted by it.
// That entry spends internal resolution first because a result about to be blurred tolerates a
// downscale, and pass count second; the numbers here are why the order is that way round and not the
// other, and why the resolution rung stops at four — at eight the chain kernel is about two texels
// wide, which is where the bilinear upscale's own footprint becomes comparable to the blur's and the
// rung stops being the invisible one.
GYRO_TEST(Dressing, ResolutionChangesTheKernelFarLessThanPassCountDoes)
{
	const auto shape = [](Material material, Tier tier) { return Expansion(material, tier) / Facts(material).Sigma; };

	for (const Material material : Gathering)
	{
		const float high = shape(material, Tier::High);
		const float resolution = std::abs(shape(material, Tier::Medium) - high);
		const float passes = std::abs(shape(material, Tier::Low) - shape(material, Tier::Medium));

		GYRO_CHECK(resolution < 0.10F * high);
		GYRO_CHECK(passes > 4.0F * resolution);
	}
}

// A cheaper tier is cheaper. Area over the square of the divisor, times the passes in flight — which
// is the figure decision 29's `C` is keyed on and the only reason to step down at all.
GYRO_TEST(Dressing, ACheaperTierCostsLess)
{
	const auto work = [](Tier tier) {
		const TierFacts facts = Facts(tier);

		return static_cast<float>(facts.Passes) / static_cast<float>(facts.Divisor * facts.Divisor);
	};

	GYRO_CHECK(work(Tier::Medium) < work(Tier::High));
	GYRO_CHECK(work(Tier::Low) < work(Tier::Medium));
}

// The discrete kernel's variance is what `SolveHalfWidth` inverts, so it has to be the exact figure
// rather than the continuous box's. A half-width of one is three equal taps at -1, 0, 1, whose
// variance is 2/3.
GYRO_TEST(Dressing, TheBoxVarianceIsTheDiscreteKernelsAndNotTheContinuousOnes)
{
	GYRO_CHECK(std::abs(BoxVariance(1.0F) - (2.0F / 3.0F)) < 1e-5F);
	GYRO_CHECK(std::abs(BoxVariance(2.0F) - 2.0F) < 1e-5F);
	GYRO_CHECK(BoxVariance(0.0F) == 0.0F);

	// Monotone in the half-width, which is what the bisection rests on.
	for (float width = 0.0F; width < 8.0F; width += 0.125F)
	{
		GYRO_CHECK(BoxVariance(width + 0.125F) > BoxVariance(width));
	}
}

// A fractional half-width is the mechanism the test above rests on: an integer one would round the
// sigma to whatever the chain's grid allowed, which is decision 34's radius arriving on the ladder
// through the back door.
GYRO_TEST(Dressing, HalfWidthsAreFractional)
{
	bool fractional = false;

	for (const Material material : Gathering)
	{
		for (const Tier tier : Tiers)
		{
			const float width = ChainPlan::For(material, tier).HalfWidth;

			fractional = fractional || std::abs(width - std::round(width)) > 0.01F;
		}
	}

	GYRO_CHECK(fractional);
}

// `Smoke` sits over content gyro did not choose, so its opacity comes from a floor rather than from
// taste — an arithmetic difference from `Glass` rather than a stylistic one, which is the whole of
// why decision 103 has two materials instead of one with a weight.
GYRO_TEST(Dressing, SmokeIsFlooredAndGlassIsNot)
{
	GYRO_CHECK(Facts(Material::Smoke).ContrastFloor > 0.0F);
	GYRO_CHECK(Facts(Material::Glass).ContrastFloor == 0.0F);

	GYRO_CHECK(ResolvedTint(Facts(Material::Glass)) == Facts(Material::Glass).Tint);

	// The floor has to *bind*, or it is decorative: `Smoke`'s stated alpha is taste and its floor is
	// the obligation, and a row whose taste already cleared its floor would pass every other check
	// here while testing nothing.
	GYRO_CHECK(ResolvedTint(Facts(Material::Smoke)).Alpha > Facts(Material::Smoke).Tint.Alpha);
	GYRO_CHECK_EQ(ResolvedTint(Facts(Material::Smoke)).Alpha, Facts(Material::Smoke).ContrastFloor);

	// And `Smoke` ends up the heavier of the two, which is a consequence of the two obligations rather
	// than of an ordering — decision 103 refuses a set ordered by weight, and this is the check that
	// the numbers came out that way rather than being placed that way.
	GYRO_CHECK(ResolvedTint(Facts(Material::Smoke)).Alpha > ResolvedTint(Facts(Material::Glass)).Alpha);
}

// Raising the alpha to the floor scales the colour with it, because the tint is premultiplied. A
// floor applied to the alpha alone would lighten the material as it darkened it.
GYRO_TEST(Dressing, TheFloorKeepsTheTintPremultiplied)
{
	MaterialFacts facts{ .Gathering = true,
		                 .Sigma = 8.0F,
		                 .Tint = { .Red = 0.1F, .Green = 0.2F, .Blue = 0.3F, .Alpha = 0.5F },
		                 .ContrastFloor = 1.0F };

	const MaterialTint resolved = ResolvedTint(facts);

	GYRO_CHECK(std::abs(resolved.Alpha - 1.0F) < 1e-6F);
	GYRO_CHECK(std::abs(resolved.Red - 0.2F) < 1e-6F);
	GYRO_CHECK(std::abs(resolved.Green - 0.4F) < 1e-6F);
	GYRO_CHECK(std::abs(resolved.Blue - 0.6F) < 1e-6F);
}

// Decision 63's declaration, and the property that makes a box chain worth having beyond its cost: a
// box is compactly supported, so the bound is exact rather than a number of standard deviations
// somebody picked. Under-declaring leaves a trail at the edge of what moved and over-declaring costs
// frames on an idle machine, and only the second one is silent.
GYRO_TEST(Dressing, TheDeclaredExpansionCoversTheChainsWholeReach)
{
	for (const Material material : Gathering)
	{
		for (const Tier tier : Tiers)
		{
			const ChainPlan plan = ChainPlan::For(material, tier);

			// Every pass reaches its own half-width, and the downscale and the upscale each reach one
			// chain texel further.
			const float reach =
				(static_cast<float>(plan.Passes) * plan.HalfWidth + 2.0F) * static_cast<float>(plan.Divisor);

			GYRO_CHECK(Expansion(material, tier) >= reach);
		}
	}
}

// A material that gathers nothing expands nothing, which is what keeps the ordinary node — the
// wallpaper, a plain window — off the whole mechanism.
GYRO_TEST(Dressing, NoneGathersNothingAndExpandsNothing)
{
	GYRO_CHECK(!Facts(Material::None).Gathering);
	GYRO_CHECK(Expansion(Material::None, Tier::High) == 0.0F);
	GYRO_CHECK(ChainPlan::For(Material::None, Tier::High).Passes == 0);
}

// Decision 103's pointwise column ships empty, and the classification is fixed per material rather
// than varying with tier — the cute answer of gathering at high tiers and going pointwise at low
// ones couples the two axes decision 62 spends a paragraph holding apart.
GYRO_TEST(Dressing, EveryMaterialInTheSetGathers)
{
	for (const Material material : Gathering)
	{
		GYRO_CHECK(Facts(material).Gathering);
	}
}
