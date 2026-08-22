#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <type_traits>

// What the numbers in a buffer mean as light.
//
// Docs/Architecture.md#the-composite-space fixes this completely, so this file is transcription
// rather than invention: **every surface carries a color state — primaries, transfer function,
// alpha mode, reference luminance — and untagged content is sRGB by rule and never by inspection.**
// Decisions 47 and 48 are the argument; what is here is the four fields that argument names.
//
// **Neither half of the render seam owns it, and neither half of the world does either — so it is
// in Core.** The renderer needs it to linearise on import and to encode at the end; the presenter
// needs it to program a plane's degamma, CTM, and gamma when a layer is promoted to scanout.
// Docs/Architecture.md's rule that direct scanout is conditional on the KMS pipeline expressing what
// the composite would have applied is a comparison between two of these, made by a party that can
// see both. That argument put this file in Seam and it was the wrong pair of halves: a color state
// is what a *client* declared, so Protocol authors one per commit and Scene stores it, and neither
// may name Seam. Decision 87 is the move; what makes Core the answer rather than Geometry is that a
// color state is not rect arithmetic, and Core's own test — depends on nothing, names no other
// domain's vocabulary, has more than one caller before it has two implementations — is met here.
//
// **Spelled without the u, alone in this codebase.** The documents say color and keep saying it;
// the identifiers say color, because the surrounding ecosystem an implementation has to be read
// against — `DRM_FORMAT`, `VkColorSpaceKHR`, `wp_color_manager_v1` — spells it that way without
// exception, and a codebase that alternates between the two spellings in identifiers is a codebase
// where every use is a small guess.
//
// **Reference luminance is a number policy will revisit, and it is here anyway.** Brightness-relative
// compositing means 1.0 is SDR reference white and HDR headroom lives above it, so a color state has
// to say what its own 1.0 is worth in nits or the relation between two of them is unstated. BT.2408's
// 203 is the default because it is what the SDR-on-HDR direction is going to use;
// [Open.md](../../Docs/Open.md)'s tone-mapping entry is where the policy that may change it lives.
// The field is not what that entry is deciding.

// The chromaticities. Named sets only: a custom triangle is a real thing to want for a measured panel
// and it belongs with per-output characterisation, which Open.md holds open — adding a variant here
// now would be inventing the storage before the profile it stores.
enum class ColorPrimaries : std::uint8_t
{
	Bt709,  // sRGB's, and the default for anything untagged
	DciP3,  // most laptop and phone panels that claim wide gamut
	Bt2020, // the composite space's, wide enough that ordinary content stays non-negative
};

// The encoding, not the appearance. Linear is the space weighted sums are correct in; the rest are
// what content and panels actually carry.
enum class TransferFunction : std::uint8_t
{
	Srgb,   // the piecewise curve, not a pure 2.2 gamma
	Linear, // proportional to light; the composite space
	Pq,     // SMPTE ST 2084, absolute
	Hlg,    // BT.2100 hybrid log-gamma, relative
};

// How alpha has already been applied. Docs/Architecture.md#premultiplied-alpha-is-the-sharp-edge is
// what makes this part of the color state rather than a detail of the format: a premultiplied
// value in an encoded transfer function has had its alpha applied in the wrong space, and correcting
// it needs both facts at once.
//
// Opacity is not here. Whether a layer has alpha *at all* is a property of how it is being blended
// this frame, which belongs to the layer; this says what the alpha that exists has already done.
enum class AlphaMode : std::uint8_t
{
	Premultiplied,
	Straight,
};

struct ColorState
{
	ColorPrimaries Primaries = ColorPrimaries::Bt709;
	TransferFunction Transfer = TransferFunction::Srgb;
	AlphaMode Alpha = AlphaMode::Premultiplied;

	// Padding, spelled. Nothing here crosses the publication boundary today, but a color state is
	// compared field by field and an uninitialised gap is the classic way two identical states fail
	// to be equal on one build and pass on another.
	std::uint8_t Reserved = 0;

	// What this content's 1.0 is worth, in cd/m². See above for why the default is 203.
	float ReferenceLuminance = 203.0F;

	// What an untagged buffer is, by rule. Named rather than left to the default member initializers
	// so that the rule has a spelling a reader can grep for, and so that changing the defaults for
	// some other reason cannot silently change what untagged means.
	[[nodiscard]] static constexpr ColorState Srgb() noexcept
	{
		return { ColorPrimaries::Bt709, TransferFunction::Srgb, AlphaMode::Premultiplied, 0, 203.0F };
	}

	// The space gyro composites in: linear light at wide primaries, brightness-relative.
	[[nodiscard]] static constexpr ColorState Composite() noexcept
	{
		return { ColorPrimaries::Bt2020, TransferFunction::Linear, AlphaMode::Premultiplied, 0, 203.0F };
	}

	// Exact, including the luminance. Two states that differ only in reference luminance produce
	// different pixels, so a comparison that ignored it would report a promotion as free when it
	// changes the picture — which is the one thing Docs/Experience.md's fourth promise forbids.
	friend constexpr bool operator==(ColorState, ColorState) noexcept = default;
};

[[nodiscard]] constexpr std::string_view Name(ColorPrimaries primaries) noexcept
{
	switch (primaries)
	{
		case ColorPrimaries::Bt709:
			return "bt709";
		case ColorPrimaries::DciP3:
			return "p3";
		case ColorPrimaries::Bt2020:
			return "bt2020";
	}

	return "?";
}

[[nodiscard]] constexpr std::string_view Name(TransferFunction transfer) noexcept
{
	switch (transfer)
	{
		case TransferFunction::Srgb:
			return "srgb";
		case TransferFunction::Linear:
			return "linear";
		case TransferFunction::Pq:
			return "pq";
		case TransferFunction::Hlg:
			return "hlg";
	}

	return "?";
}

[[nodiscard]] constexpr std::string_view Name(AlphaMode alpha) noexcept
{
	switch (alpha)
	{
		case AlphaMode::Premultiplied:
			return "premultiplied";
		case AlphaMode::Straight:
			return "straight";
	}

	return "?";
}

// Prints as bt2020/linear premultiplied @203nit.
template<>
struct std::formatter<ColorState>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(ColorState state, Context& context) const
	{
		return std::format_to(
			context.out(),
			"{}/{} {} @{}nit",
			Name(state.Primaries),
			Name(state.Transfer),
			Name(state.Alpha),
			state.ReferenceLuminance
		);
	}
};

// The contract everything downstream assumes.
static_assert(std::is_trivially_copyable_v<ColorState> && std::is_standard_layout_v<ColorState>);
static_assert(std::is_aggregate_v<ColorState>);
static_assert(sizeof(ColorState) == 8, "No padding to leave uninitialised");
static_assert(std::formattable<ColorState, char>);

// The rule, where the compiler can hold us to it: an untagged buffer is sRGB, and that is what a
// default-constructed state is.
static_assert(ColorState{} == ColorState::Srgb(), "Untagged content is sRGB by rule, never by inspection");
static_assert(ColorState::Composite().Transfer == TransferFunction::Linear);
static_assert(ColorState::Composite().Primaries == ColorPrimaries::Bt2020);
static_assert(ColorState::Srgb() != ColorState::Composite());

// Luminance is part of the identity. Two states alike in every enum still describe different light.
static_assert(
	ColorState{ ColorPrimaries::Bt709, TransferFunction::Srgb, AlphaMode::Premultiplied, 0, 100.0F } !=
	ColorState::Srgb()
);
