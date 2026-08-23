#pragma once

#include <algorithm>
#include <cmath>
#include <span>

#include "Animation/Solve/Spring.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"

// What *close enough* means, as numbers, for every channel an entity has.
//
// `Animation/Solve/Spring.h` takes a `SettleThresholds` per channel and declines to know what is in
// it, on the grounds that neither half is that module's to decide. This is the file that decides,
// and it is here rather than there because the geometric half is a question about *outputs* — decision
// 54 puts it in device pixels of the finest grid a node intersects, and `Scene` is the only place that
// holds an output set at all.
//
// **The numbers are policy fields rather than constants, for `Frame/Timing.h`'s reason.** Docs/Open.md
// carries *settling thresholds for non-geometric properties* and it is still open; what is below is the
// narrowest answer the question admits today, and it is a struct a test can vary and a later decision
// can replace in one place. Every one of them errs long: a threshold that is too small costs a few
// redundant composites at the tail of an animation, and one that is too large freezes a motion a person
// is watching. Docs/Animation.md#settling-is-a-bound-not-a-solution is the same asymmetry one level
// down, and it is why nothing here is tuned toward the shorter tail.
//
// **What a person perceives is what sets the geometric pair.** Decision 54 snaps a node to the device
// grid at the instant it settles, and the whole argument for snapping there rather than every frame is
// that the residual is small enough to be invisible. So the position threshold is what bounds that
// jump, and it has to sit well under a device pixel: a shift of an eighth of a pixel changes an
// antialiased edge's coverage by an eighth, which on a black-on-white window border is a visible
// twitch. A sixteenth is half of that and costs one halving of the exponential — about ninety
// milliseconds on a standard response, at the tail of a transition rather than in steady-state idle.

// SPEC: the settling thresholds, in the units a person can be pointed at. Docs/Open.md's *settling
// thresholds* entry is what replaces these; decision 54 settles the geometric half's *form* and no
// entry has ever settled a number.
struct SettlePolicy
{
	// Device pixels of the finest grid, and device pixels per second. The velocity figure is a legible
	// unit rather than a derived one: at one device pixel per second, the fastest panel gyro will ever
	// drive moves a node by a hundred and forty-fourth of a pixel between frames, which is an order of
	// magnitude below the position threshold beside it. The pair is balanced rather than arbitrary —
	// on the catalog's own responses the two crossings land within a tenth of a second of each other,
	// which is the sign that neither one is doing all the work.
	double Position = 1.0 / 16.0;
	double Velocity = 1.0;

	// Opacity has no output pixel to be expressed in, which is exactly what Docs/Open.md says is
	// unresolved about it. The narrowest thing that is true anyway: a composite lands on an eight-bit
	// wire in the ordinary case, so a residual below half a code point cannot be the difference between
	// two pixels that leave the machine, and a rate below one code point per second cannot accumulate
	// into one either. That is a representability argument rather than a perceptual one, and it is
	// deliberately the weaker of the two — it settles what can be settled and leaves the perceptual half
	// to the review with a screen in front of it that the dressing numbers already want.
	float Opacity = 1.0F / 512.0F;
	float OpacityRate = 1.0F / 256.0F;
};

// The policy resolved against an output set: four thresholds, in the four channels' own units.
//
// Resolved once per publication rather than once per node, which is the whole reason the conversion
// radius below is not the node's own. See `Radius`.
class SettleThresholdSet
{
public:
	SettleThresholdSet() = default;

	// **The finest grid in the output set, rather than the finest grid a node intersects.** Decision 54
	// asks for the second and it is not available: which outputs a node touches is a screen-space
	// question, and dispatch has no screen space — the walk that projects a node onto an output is
	// `Frame`'s and runs a few milliseconds later. The substitution is the conservative one, since the
	// finest grid over every output is at least the finest grid over any subset, so a node settles no
	// earlier than decision 54 would have it. What it costs is a node on a 1× panel settling to the
	// tolerance of the 2× panel beside it — a longer tail on that node and nothing else.
	//
	// It is the same substitution the wake fold makes one field over, and that is not a coincidence:
	// both halves of settling want to know which outputs a node reaches, neither can, and both answer
	// *every output* in the direction that costs frames rather than freezes them.
	explicit SettleThresholdSet(std::span<const SceneOutput> outputs, SettlePolicy policy = {}) noexcept
		: m_Policy{ policy }, m_Density{ Finest(outputs) }, m_Radius{ Radius(outputs) }
	{}

	// A translation is in the global space the world is laid out in, and a device pixel is one over the
	// density there.
	[[nodiscard]] SettleThresholds<double> Translation() const noexcept
	{
		const double density = m_Density.ToDouble();

		return { .Position = m_Policy.Position / density, .Velocity = m_Policy.Velocity / density };
	}

	// Scale is dimensionless and a rotation in the log map is radians, and both displace a point at
	// radius `r` by about `ε·r` — Docs/Animation.md#implementation-notes' conversion, and the reason
	// `Spring`'s thresholds are on the magnitude rather than per component. Dividing the geometric
	// threshold by the radius puts both back in device pixels, and the density cancels: the answer is
	// the pixel threshold over the radius *in pixels*.
	//
	// Two accessors returning the same numbers, and they are two because the question is two. A later
	// answer that gives a rotation its own tolerance — a card flip is judged by its silhouette and a
	// resize by its edge — replaces one of these and not both.
	//
	// Spelled `Scaling` rather than `Scale` because `Geometry/Scale.h`'s exact rational is a type this
	// class also names, and a member function may not share a name with a type its own body resolves.
	[[nodiscard]] SettleThresholds<float> Scaling() const noexcept { return Angular(); }

	[[nodiscard]] SettleThresholds<float> Rotation() const noexcept { return Angular(); }

	[[nodiscard]] SettleThresholds<float> Opacity() const noexcept
	{
		return { .Position = m_Policy.Opacity, .Velocity = m_Policy.OpacityRate };
	}

	// The radius a dimensionless residual is judged at, in device pixels of the finest grid.
	//
	// **It is the screen's half-diagonal and not the node's own, and that is a deferral rather than an
	// answer.** The honest conversion needs the node's bounding radius composed through every ancestor's
	// scale, because an overview magnifying a thumbnail magnifies its residual too — which is the same
	// screen-space quantity the paragraph above says dispatch does not have. The largest radius anything
	// can be drawn at is the output's own half-diagonal, so judging every node there is the bound that
	// cannot be wrong in the direction that freezes a motion. What it costs is real and worth stating:
	// a hundred-pixel menu scaling open settles against a tolerance eleven times tighter than it needs,
	// which is roughly three tenths of a second of extra frames at the tail of the transition.
	[[nodiscard]] double ConversionRadius() const noexcept { return m_Radius; }

	[[nodiscard]] Scale Density() const noexcept { return m_Density; }

private:
	[[nodiscard]] SettleThresholds<float> Angular() const noexcept
	{
		return { .Position = static_cast<float>(m_Policy.Position / m_Radius),
			     .Velocity = static_cast<float>(m_Policy.Velocity / m_Radius) };
	}

	// An empty output set is identity rather than a refusal: a scene with nowhere to be drawn still has
	// springs running in it, and answering with a density of zero would divide the threshold to infinity
	// and settle everything at once — a freeze, on the path a headless test and a machine between
	// modesets both take.
	[[nodiscard]] static Scale Finest(std::span<const SceneOutput> outputs) noexcept
	{
		Scale finest = Scale::FromInteger(1);

		for (const SceneOutput& output : outputs)
		{
			finest = std::max(finest, output.Density);
		}

		return finest;
	}

	// The largest half-diagonal in the set, in the device pixels the grids are already stated in, and
	// floored so that an output nobody has configured yet cannot divide by zero. The floor is a whole
	// panel rather than a small number, for the reason the accessor's own comment gives: small is the
	// direction that freezes.
	[[nodiscard]] static double Radius(std::span<const SceneOutput> outputs) noexcept
	{
		// SPEC: the radius used where no output has stated one. Half the diagonal of a 1920x1080 panel,
		// rounded down, so a scene serialised before the first modeset settles about as late as the same
		// scene would on the commonest display there is — and so that a real panel in the set always
		// governs rather than being clamped up by a default that happened to be a fraction larger.
		double radius = DefaultRadius;

		for (const SceneOutput& output : outputs)
		{
			const auto width = static_cast<double>(output.Grid.Width);
			const auto height = static_cast<double>(output.Grid.Height);

			radius = std::max(radius, std::hypot(width, height) / 2.0);
		}

		return radius;
	}

	static constexpr double DefaultRadius = 1101.0;

	SettlePolicy m_Policy{};
	Scale m_Density = Scale::FromInteger(1);
	double m_Radius = DefaultRadius;
};

// The contract everything downstream assumes. The arithmetic itself is asserted in Settle.Test.cpp,
// because `std::hypot` is not constexpr in C++23 and the resolver's whole job is that call.
static_assert(SettlePolicy{}.Position < 1.0, "The snap at settle has to be invisible, so it is sub-pixel");
static_assert(SettlePolicy{}.Position > 0.0, "A threshold of zero is never crossed and never settles");
static_assert(SettlePolicy{}.Velocity > 0.0);
static_assert(SettlePolicy{}.Opacity > 0.0F && SettlePolicy{}.OpacityRate > 0.0F);
