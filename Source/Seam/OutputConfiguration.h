#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <type_traits>

#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Seam/ColorState.h"
#include "Seam/RenderTarget.h"

// What an output is programmed to be, in both directions: what `Reconfigure()` is asked for and what
// `Reconfigured` reports was achieved.
//
// **One type for both, because the interesting comparison is between them.** A request that the
// hardware could not honour reports itself by the achieved configuration differing from the wanted
// one — no failure signal, no query path, no reading hardware state back. `SatisfiedBy` below is that
// comparison written once, so that every caller asks it the same way.
//
// **Which is also why the variable-refresh range is here and is not requested.** `[vmin, vmax]` is
// derived from the mode and the panel's reported range and cannot be asked for; the servo learns its
// own authority only after a mode is set. So the range is a field the *achieved* configuration fills
// in and the wanted one leaves alone, and `SatisfiedBy` ignores it for exactly that reason. See
// KernelWishlist.md, *VRR range is discovered, never negotiated*.
//
// **Generation is what makes a late completion legible.** Decision 73 has the frame thread initiate
// reconfiguration when a per-output generation carried in the snapshot moves, and the transition
// completes as an event some time later. Two requests can therefore be outstanding across a resume or
// a fast sequence of user changes, and without the generation echoed back, a completion for the
// superseded one is indistinguishable from the completion being waited for. It is carried rather than
// invented here: dispatch authors it into the snapshot, the frame loop copies it into the request, the
// backend copies it into the report.
//
// **What is deliberately not here: scale.** An output's scale is layout policy — it decides how large
// things are drawn and nothing else — and no backend programs it into anything. The composite target
// is the mode's resolution whatever the scale is, so a scale change never reaches the presenter, and
// putting `Scale` here would create a hardware verb with a field no implementation reads. Geometry's
// output adapter is where scale lives; see Docs/Decisions.md decision 52.
//
// **Power is here, and it is here for decision 73's general rule rather than for convenience.** DPMS
// costs on the order of 100 ms by Architecture.md's own table, which puts it in the same class as a
// mode set: something that must never enter the frame thread's non-preemptible chunk. Since the class
// is *what may not be done inline*, its members belong to the verb that is defined as not being done
// inline, and the idle ladder turning a panel off is then the same mechanism as a user changing a
// mode rather than a second path with its own timing story.

// The orientation the *presenter* applies, which is not the same statement as the panel's physical
// mounting. A ninety-degree rotation may be executed by a KMS plane or by gyro's own composite, and
// exactly one of them may do it: where the composite has already rotated, this is Normal. Space.h
// records the same split from the geometry side, where the panel space arrives with the DRM backend.
enum class OutputTransform : std::uint8_t
{
	Normal,
	Rotate90,
	Rotate180,
	Rotate270,
	Flipped,
	Flipped90,
	Flipped180,
	Flipped270,
};

// The window a variable-refresh panel will accept, as periods rather than as rates — because a period
// is what the frame loop schedules against and converting at the boundary is decision 57's shape
// applied to a second quantity.
//
// Shortest is the highest rate the panel will run at and Longest the lowest, clear of whatever
// low-framerate compensation the panel does at the bottom. A fixed-refresh output reports a degenerate
// range, which is the same shape rather than a special case.
struct VariableRefresh
{
	bool Enabled = false;
	Duration Shortest{};
	Duration Longest{};

	[[nodiscard]] constexpr bool IsDegenerate() const noexcept { return Shortest == Longest; }

	friend constexpr bool operator==(VariableRefresh, VariableRefresh) noexcept = default;
};

struct OutputConfiguration
{
	// See above. Zero is the generation nothing has authored, which is what an adoption at boot
	// carries.
	std::uint64_t Generation = 0;

	// The mode, stated as what the seam can express on all three backends: an extent and a nominal
	// period. Timings are the DRM backend's business, and it selects the mode whose timings match —
	// `drm_mode_equal` compares real timings rather than names, so an adoption is recognised by the
	// kernel rather than asserted by gyro.
	PixelSize<DeviceSpace> Resolution{};
	Duration Period{};

	// Enabling variable refresh is set when a mode is set rather than per frame, which is why it is
	// here and not on `Present`. Once it is active the effective interval is carried entirely by when
	// the flip is submitted, and that is the clock's business.
	VariableRefresh Refresh{};

	OutputTransform Transform = OutputTransform::Normal;

	// False is DPMS off: the output holds no picture and owes no frames. `Present()` on an unpowered
	// output is an error rather than a silent no-op, because a frame loop still submitting to a dark
	// panel is a bug that otherwise shows up as battery life.
	bool Powered = true;

	std::uint16_t Reserved = 0;

	// What is scanned out: 8-bit sRGB or 10-bit PQ depending on the mode, per Architecture.md's
	// precision table. The pair is one decision — a format without its color state does not say what
	// the bits mean, and a color state without its format does not say how many there are.
	PixelFormat Format{};
	ColorState Color = ColorState::Srgb();

	// Whether an achieved configuration honours this request. Compares only what a caller can ask for:
	// the generation is an echo, and the variable-refresh range is learned rather than requested, so
	// neither is part of the question.
	[[nodiscard]] constexpr bool SatisfiedBy(const OutputConfiguration& achieved) const noexcept
	{
		return achieved.Resolution == Resolution && achieved.Period == Period &&
		       achieved.Refresh.Enabled == Refresh.Enabled && achieved.Transform == Transform &&
		       achieved.Powered == Powered && achieved.Format == Format && achieved.Color == Color;
	}

	friend constexpr bool operator==(const OutputConfiguration&, const OutputConfiguration&) noexcept = default;
};

[[nodiscard]] constexpr std::string_view Name(OutputTransform transform) noexcept
{
	switch (transform)
	{
		case OutputTransform::Normal:
			return "normal";
		case OutputTransform::Rotate90:
			return "90";
		case OutputTransform::Rotate180:
			return "180";
		case OutputTransform::Rotate270:
			return "270";
		case OutputTransform::Flipped:
			return "flipped";
		case OutputTransform::Flipped90:
			return "flipped-90";
		case OutputTransform::Flipped180:
			return "flipped-180";
		case OutputTransform::Flipped270:
			return "flipped-270";
	}

	return "?";
}

// Prints as gen 4 device(2560x1440) @6944444ns vrr[6944444ns, 20833333ns] normal XR24 mod 0x0
// bt709/srgb premultiplied @203nit, or gen 4 off.
template<>
struct std::formatter<OutputConfiguration>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const OutputConfiguration& configuration, Context& context) const
	{
		auto out = std::format_to(context.out(), "gen {}", configuration.Generation);

		if (!configuration.Powered)
		{
			return std::format_to(out, " off");
		}

		out = std::format_to(out, " {} @{}", configuration.Resolution, configuration.Period);

		if (configuration.Refresh.Enabled)
		{
			out = std::format_to(out, " vrr[{}, {}]", configuration.Refresh.Shortest, configuration.Refresh.Longest);
		}

		return std::format_to(
			out, " {} {} {}", Name(configuration.Transform), configuration.Format, configuration.Color
		);
	}
};

// The contract everything downstream assumes.
static_assert(std::is_trivially_copyable_v<OutputConfiguration> && std::is_standard_layout_v<OutputConfiguration>);
static_assert(std::is_aggregate_v<OutputConfiguration>);
static_assert(std::formattable<OutputConfiguration, char>);

// A fixed-refresh output has a degenerate range rather than a disabled one, which is what lets the
// clock's period window be read the same way on every output.
static_assert(VariableRefresh{}.IsDegenerate());
static_assert(!VariableRefresh{ true, PeriodFromHertz(144.0), PeriodFromHertz(48.0) }.IsDegenerate());

// The comparison this type exists for: an achieved configuration that learned a range still satisfies
// a request that could not state one.
static_assert(
	[] {
		const OutputConfiguration wanted{
			.Generation = 4,
			.Resolution = { 2560, 1440 },
			.Period = PeriodFromHertz(144.0),
			.Refresh = { .Enabled = true },
			.Format = { FormatXrgb8888, 0, ModifierLinear },
		};

		OutputConfiguration achieved = wanted;
		achieved.Refresh = { true, PeriodFromHertz(144.0), PeriodFromHertz(48.0) };

		return wanted.SatisfiedBy(achieved) && !(wanted == achieved);
	}(),
	"The learned range is not part of the request, and equality is not the question a caller asks"
);

// And the comparison that must fail: a backend that fell back to a mode nobody asked for.
static_assert([] {
	const OutputConfiguration wanted{ .Resolution = { 3840, 2160 }, .Period = PeriodFromHertz(60.0) };

	OutputConfiguration achieved = wanted;
	achieved.Resolution = { 1920, 1080 };

	return !wanted.SatisfiedBy(achieved);
}());
