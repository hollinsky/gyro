#pragma once

#include <cstdint>
#include <format>
#include <type_traits>

// Output scale, as an exact rational.
//
// Scaling is the most visible thing a compositor gets wrong and the least diagnosable afterwards,
// because nearly every artefact of it is a value that got rounded once and then stored. This type
// is the arithmetic half of not doing that: gyro and a client have to arrive independently at the
// same integer or there is a gap, an overlap, or a protocol error, and floating point does not
// dependably let them.
//
//     1000 x 1.1       = 1100.0000000000001  -> ceil -> 1101
//     1000 x 132 / 120 = 1100                            exactly
//
// 1.25, 1.5, 1.75, and 2.0 are all exact in binary, so the ordinary settings ladder never shows
// this and 110% at a large surface size does — on one output, intermittently. That failure profile
// is why this is a type rather than a comment on a float.
//
// The denominator is fixed at wp_fractional_scale_v1's own unit rather than being carried per
// value. A general rational would need normalizing, would let two scales that are equal compare
// unequal, and would be a second answer to a question the protocol has already answered. The wire
// unit is the shared one, so it is the stored one.
//
// **This is not the ratio a surface is resampled by.** That is the composed buffer-to-device
// transform's scale — output scale over buffer scale, which is not expressible in 120ths — and it
// belongs to the transform. The two are easy to conflate and mean different things: this one does
// integer size arithmetic, that one decides whether a node samples one-to-one. See
// Docs/Decisions.md decision 53 and Docs/Architecture.md#scale-is-an-exact-rational.

// Named at every call site, with no default, because the direction is the caller's and not the
// type's. A buffer must *cover* the surface it backs, so it rounds Up; a configure's logical size
// must not exceed what layout allotted, so it rounds Down and gyro absorbs the remainder into its
// own gap; a position landing on the device grid rounds Nearest. A single conversion that silently
// picked one would hand the other caller the one-pixel seam between two tiled windows that
// Docs/Experience.md promises against.
enum class Rounding : std::uint8_t
{
	Down,
	Nearest,
	Up,
};

namespace Detail
{
// Rounds toward negative infinity rather than toward zero, which is what C++ integer division does.
// The distinction is invisible until an output sits to the left of the global origin: truncation
// makes a coordinate round one way at x > 0 and the other way at x < 0, so a window dragged across
// the origin changes which device pixel it lands on. The asymmetry is one pixel and its cause is
// wherever someone happened to put their second monitor, which is about as hard to attribute as a
// bug gets.
[[nodiscard]] constexpr std::int64_t FloorDivide(std::int64_t value, std::int64_t divisor) noexcept
{
	const std::int64_t quotient = value / divisor;
	const std::int64_t remainder = value % divisor;

	return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

[[nodiscard]] constexpr std::int64_t Divide(std::int64_t value, std::int64_t divisor, Rounding rounding) noexcept
{
	switch (rounding)
	{
		case Rounding::Down:
			return FloorDivide(value, divisor);

		case Rounding::Up:
			return FloorDivide(value + divisor - 1, divisor);

		case Rounding::Nearest:
			// Ties go toward positive infinity rather than away from zero, for FloorDivide's reason: a
			// tie-break that depends on sign is a tie-break that depends on which side of the origin an
			// output was placed.
			return FloorDivide(2 * value + divisor, 2 * divisor);
	}

	return 0;
}

// Extents and positions are int32 on the wire and in the model alike. A conversion that leaves the
// range has been handed something already meaningless, so it saturates rather than wrapping: an
// absurd size is recoverable and a negative one that used to be positive is not.
[[nodiscard]] constexpr std::int32_t Saturate(std::int64_t value) noexcept
{
	constexpr std::int64_t Low = -2147483648;
	constexpr std::int64_t High = 2147483647;

	return static_cast<std::int32_t>(value < Low ? Low : (value > High ? High : value));
}
} // namespace Detail

class Scale
{
public:
	// wp_fractional_scale_v1 speaks 120ths, which is 8 x 15 and therefore divides every scale
	// anybody ships: 1.25, 1.5, 1.75, 2, and the thirds.
	static constexpr std::int32_t Denominator = 120;

	// The bound is arithmetic headroom rather than taste, and it is generous by a wide margin. An
	// int32 extent times this numerator is about 2^43, and Nearest doubles that — both exact in the
	// int64 the conversions compute in. No display is within an order of magnitude of scale 32.
	static constexpr std::int32_t MinimumNumerator = 1;
	static constexpr std::int32_t MaximumNumerator = Denominator * 32;

	// Identity, because a scale has no useful null state the way a handle does — every surface and
	// every output has a real one, and a default that meant "none" would only be a value to check
	// for at the sites that are already required to have asked.
	constexpr Scale() = default;

	// The wire ingress, and the shape wp_fractional_scale_v1.preferred_scale carries. Total and
	// clamping rather than fallible: the numerator reaching here is gyro's own output configuration
	// rather than client input, and configuration is the thing Docs/Animation.md requires be clamped
	// and warned about instead of refused, since a system-layer compositor does not fail to start
	// because someone fat-fingered a display setting.
	[[nodiscard]] static constexpr Scale FromNumerator(std::int32_t numerator) noexcept
	{
		return Scale{ numerator < MinimumNumerator ? MinimumNumerator :
			                                         (numerator > MaximumNumerator ? MaximumNumerator : numerator) };
	}

	// wl_surface.set_buffer_scale's unit. Correct and second-class, per decision 56 — legacy is
	// deprecated by being visibly worse rather than by being refused.
	[[nodiscard]] static constexpr Scale FromInteger(std::int32_t scale) noexcept
	{
		return FromNumerator(Detail::Saturate(static_cast<std::int64_t>(scale) * Denominator));
	}

	// Configuration authors decimals. Rounds to the nearest representable 120th, which is exact for
	// every scale anyone writes down and within 1/240 for the ones they do not.
	[[nodiscard]] static constexpr Scale FromDouble(double scale) noexcept
	{
		if (!(scale > 0.0))
		{
			return FromNumerator(MinimumNumerator);
		}

		const double numerator = scale * Denominator;

		return FromNumerator(
			numerator >= static_cast<double>(MaximumNumerator) ? MaximumNumerator :
																 static_cast<std::int32_t>(numerator + 0.5)
		);
	}

	[[nodiscard]] constexpr std::int32_t Numerator() const noexcept { return m_Numerator; }

	// Egress, for logs and for the sampling ratio the transform composes. Never for size arithmetic
	// — that is what the conversions below are, and routing a size through a double is the whole
	// defect this type exists to remove.
	[[nodiscard]] constexpr double ToDouble() const noexcept
	{
		return static_cast<double>(m_Numerator) / static_cast<double>(Denominator);
	}

	[[nodiscard]] constexpr bool IsInteger() const noexcept { return m_Numerator % Denominator == 0; }

	// Decision 56: a client that speaks only integer buffer scale renders at the ceiling of what it
	// is on and gyro downscales, because minification degrades gracefully and magnification does
	// not.
	[[nodiscard]] constexpr std::int32_t CeilToInteger() const noexcept
	{
		return static_cast<std::int32_t>(Detail::Divide(m_Numerator, Denominator, Rounding::Up));
	}

	[[nodiscard]] constexpr std::int32_t DeviceFromLogical(std::int32_t logical, Rounding rounding) const noexcept
	{
		const std::int64_t scaled = static_cast<std::int64_t>(logical) * m_Numerator;

		return Detail::Saturate(Detail::Divide(scaled, Denominator, rounding));
	}

	[[nodiscard]] constexpr std::int32_t LogicalFromDevice(std::int32_t device, Rounding rounding) const noexcept
	{
		const std::int64_t scaled = static_cast<std::int64_t>(device) * Denominator;

		return Detail::Saturate(Detail::Divide(scaled, m_Numerator, rounding));
	}

	// Ordering is what "preferred scale is the maximum over the outputs a surface intersects" is
	// spelled with, and comparing numerators over a shared denominator is exact where comparing two
	// doubles derived from them would not dependably be.
	friend constexpr bool operator==(Scale, Scale) noexcept = default;
	friend constexpr auto operator<=>(Scale, Scale) noexcept = default;

private:
	constexpr explicit Scale(std::int32_t numerator) noexcept : m_Numerator{ numerator } {}

	std::int32_t m_Numerator = Denominator;
};

// Prints as 1.5x. The value is exact and this rendering is not — 1/120 does not terminate in
// decimal — which is acceptable for the only thing that reads it. No format spec is accepted.
//
// The context is a template parameter for the reason recorded at length in Core/Handle.h: naming
// std::format_context leaves std::format working and std::formattable false, so a value goes missing
// from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<Scale>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(Scale scale, Context& context) const
	{
		return std::format_to(context.out(), "{}x", scale.ToDouble());
	}
};

// The contract everything downstream assumes. The runtime half waits on the test harness.
static_assert(
	std::is_trivially_copyable_v<Scale> && std::is_standard_layout_v<Scale>,
	"Output configuration crosses the publication boundary"
);
static_assert(sizeof(Scale) == sizeof(std::int32_t), "The published representation has no padding");
static_assert(std::formattable<Scale, char>, "A report prints the scale rather than <unprintable>");

static_assert(Scale{} == Scale::FromInteger(1), "The default is identity, not a null state");
static_assert(Scale::FromNumerator(180).ToDouble() == 1.5);
static_assert(Scale::FromDouble(1.5) == Scale::FromNumerator(180), "The ladder's scales are exact");
static_assert(Scale::FromDouble(1.25) == Scale::FromNumerator(150));
static_assert(Scale::FromDouble(1.1) == Scale::FromNumerator(132));

// The worked example this type exists for. The float form of the same computation yields 1101.
static_assert(Scale::FromDouble(1.1).DeviceFromLogical(1000, Rounding::Up) == 1100);

static_assert(Scale::FromInteger(2).IsInteger() && !Scale::FromNumerator(180).IsInteger());
static_assert(Scale::FromNumerator(180).CeilToInteger() == 2, "A client on 1.5x renders at 2x");
static_assert(Scale::FromInteger(2).CeilToInteger() == 2, "The ceiling of an integer scale is itself");

// Rounding is the caller's, and all three answers differ where it matters.
static_assert(Scale::FromNumerator(180).DeviceFromLogical(3, Rounding::Down) == 4);
static_assert(Scale::FromNumerator(180).DeviceFromLogical(3, Rounding::Nearest) == 5);
static_assert(Scale::FromNumerator(180).DeviceFromLogical(3, Rounding::Up) == 5);

// Negative coordinates round the same direction as positive ones. An output to the left of the
// global origin is the ordinary reason to be here, and truncation would round these toward zero.
static_assert(Scale::FromNumerator(180).DeviceFromLogical(-3, Rounding::Down) == -5);
static_assert(Scale::FromNumerator(180).DeviceFromLogical(-3, Rounding::Up) == -4);
static_assert(Scale::FromNumerator(60).DeviceFromLogical(-1, Rounding::Nearest) == 0, "Ties go up on both sides");
static_assert(Scale::FromNumerator(60).DeviceFromLogical(1, Rounding::Nearest) == 1);

static_assert(Scale::FromNumerator(180).LogicalFromDevice(3, Rounding::Up) == 2);
static_assert(Scale::FromInteger(1).DeviceFromLogical(7, Rounding::Nearest) == 7, "Identity moves nothing");

// Clamped rather than wrapped or undefined, at both ends and in both directions.
static_assert(Scale::FromNumerator(0) == Scale::FromNumerator(Scale::MinimumNumerator));
static_assert(Scale::FromNumerator(-5) == Scale::FromNumerator(Scale::MinimumNumerator));
static_assert(Scale::FromInteger(4096) == Scale::FromNumerator(Scale::MaximumNumerator));
static_assert(Scale::FromDouble(0.0) == Scale::FromNumerator(Scale::MinimumNumerator));
static_assert(Scale::FromInteger(2).DeviceFromLogical(2147483647, Rounding::Up) == 2147483647);
