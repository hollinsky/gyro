#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string_view>
#include <type_traits>

#include "Geometry/Space.h"

// The restricted transform, and the one classification of it.
//
// A quarter turn, an optional flip, a positive scale per axis, and a translation — closed under
// composition and deliberately unable to express anything else. This is what every one of
// Docs/Architecture.md#the-spaces' three *adapters* is made of, and decision 52's requirement that
// each be exact and declared rather than inferred is what they have in common:
//
//   the **surface adapter**, buffer to surface, built from `wl_surface.set_buffer_transform`,
//   `wl_surface.set_buffer_scale`, and `wp_viewport`'s `src` and `dst` — never recomputed from a
//   scale factor, because the whole structural gain decision 56 claims for viewporter is that this
//   mapping is stated by the client rather than derived from buffer dimensions and a float;
//
//   the **output adapter**, global onto an output's device grid, from output configuration;
//
//   the **panel adapter**, that grid onto the panel a KMS plane or the composite scans out.
//
// **The scale is per axis, and that is the surface adapter's doing.** `wp_viewport` does not require
// `src` and `dst` to share an aspect ratio, and anamorphic video is the case that reaches for it, so
// buffer to surface is genuinely non-uniform and a single factor could not express it. Nothing is
// given up for it: a quarter turn merely exchanges which factor multiplies which axis, so the set is
// closed exactly as before. It is two scalars rather than one from the beginning because this shape
// crosses the publication boundary with the output configuration, and a published layout is decided
// at line zero or renegotiated with every reader of it.
//
// **It is permanently the restricted one.** Widening it the day something wants a shear would
// silently withdraw the property every consumer below reads it for: that the image of an
// axis-aligned rectangle is an axis-aligned rectangle, and that composing two adapters loses
// nothing. Nodes carry a full 3D affine per decision 55 and that lives elsewhere; what reaches this
// type is what reduces to it, and a transform that does not reduce classifies as nothing at all
// rather than being approximated by one that does.
//
// The orientation half is `wl_output_transform`'s eight values, with its numbering, because that is
// where these enter the system — `wl_surface.set_buffer_transform` and the output's own
// configuration — and a translation table at the protocol boundary is a table somebody eventually
// writes backwards. Decision 55 records the corroboration from the other direction: Flatland's
// entire client-facing transform vocabulary is this set, a scale, and a translation.
//
// See Docs/Architecture.md#resample-once-and-know-when-it-is-zero for the classification, and
// Docs/Decisions.md decisions 52 through 56.

// The eight rigid orientations of the plane that map axes to axes, numbered as
// `wl_output_transform` numbers them: the low two bits are quarter turns and bit two is the flip.
//
// The convention is spelled out rather than left to be inferred, because "90 degrees" names two
// different matrices depending on which way Y points and everything here is Y-down. A quarter turn
// takes (x, y) to (y, -x); a flip takes (x, y) to (-x, y), which is the mirror about the vertical
// axis the protocol describes; and a flipped orientation is the flip *first*, then the turn — so
// Flipped90 is (x, y) to (y, x). KMS numbers its plane rotation property differently and in the
// other sense, and translating to it belongs to the DRM backend rather than here.
enum class AxisOrientation : std::uint8_t
{
	Normal = 0,
	Rotate90 = 1,
	Rotate180 = 2,
	Rotate270 = 3,
	Flipped = 4,
	Flipped90 = 5,
	Flipped180 = 6,
	Flipped270 = 7,
};

inline constexpr std::uint8_t OrientationCount = 8;

[[nodiscard]] constexpr bool IsFlipped(AxisOrientation orientation) noexcept
{
	return (static_cast<unsigned>(orientation) & 0b100U) != 0U;
}

[[nodiscard]] constexpr std::uint8_t QuarterTurns(AxisOrientation orientation) noexcept
{
	return static_cast<std::uint8_t>(static_cast<unsigned>(orientation) & 0b11U);
}

// Whether width and height exchange places. Asked often enough — a rotated output's mode, a rotated
// buffer's extent, the destination rectangle of a rotated plane — that deriving it from the enum at
// each site is three chances to derive it wrong.
[[nodiscard]] constexpr bool SwapsAxes(AxisOrientation orientation) noexcept
{
	return (QuarterTurns(orientation) & 1U) != 0U;
}

[[nodiscard]] constexpr std::string_view OrientationName(AxisOrientation orientation) noexcept
{
	switch (orientation)
	{
		case AxisOrientation::Normal:
			return "normal";

		case AxisOrientation::Rotate90:
			return "90";

		case AxisOrientation::Rotate180:
			return "180";

		case AxisOrientation::Rotate270:
			return "270";

		case AxisOrientation::Flipped:
			return "flipped";

		case AxisOrientation::Flipped90:
			return "flipped-90";

		case AxisOrientation::Flipped180:
			return "flipped-180";

		case AxisOrientation::Flipped270:
			return "flipped-270";
	}

	// A value that is not one of the eight is a forged or corrupted one rather than a case to
	// handle, and it prints as what it is instead of as one of the eight it is not.
	return "invalid";
}

// `first`, then `second`. The argument order is the pipeline's rather than the matrix product's,
// because every use of this reads as a chain — buffer through surface through global through device
// — and the two orders differ on exactly the flipped values, where getting it backwards produces a
// mirrored picture on one output and nothing anywhere else.
[[nodiscard]] constexpr AxisOrientation Compose(AxisOrientation first, AxisOrientation second) noexcept
{
	// The dihedral identity F·R(k) = R(-k)·F is the whole of it: pushing the second orientation's
	// flip past the first's rotation reverses that rotation's sense. Without it a composition of two
	// flipped orientations is wrong by a half turn, which looks like a correct picture upside down.
	const unsigned turns = IsFlipped(second) ? (QuarterTurns(second) + 4U - QuarterTurns(first)) :
	                                           (QuarterTurns(second) + QuarterTurns(first));
	const unsigned flip = (static_cast<unsigned>(first) ^ static_cast<unsigned>(second)) & 0b100U;

	return static_cast<AxisOrientation>(static_cast<std::uint8_t>(flip | (turns & 0b11U)));
}

[[nodiscard]] constexpr AxisOrientation Invert(AxisOrientation orientation) noexcept
{
	// A flipped orientation is a reflection, and a reflection is its own inverse. That is not an
	// optimization but the reason the eight values form a group at all: without the four reflections
	// this would be the cyclic group of turns, and a mirrored output would have nowhere to live.
	if (IsFlipped(orientation))
	{
		return orientation;
	}

	return static_cast<AxisOrientation>(static_cast<std::uint8_t>((4U - QuarterTurns(orientation)) & 0b11U));
}

namespace Detail
{
// An (x, y) pair with no space and no meaning, which is what an orientation acts on. Point, Offset
// and Size are three different things under a transform — a point translates, a displacement does
// not, an extent has no sign — so the shared part is the two numbers and nothing else.
template<typename T>
struct Axes
{
	T X{};
	T Y{};
};

template<typename T>
[[nodiscard]] constexpr Axes<T> Orient(AxisOrientation orientation, T x, T y) noexcept
{
	switch (orientation)
	{
		case AxisOrientation::Normal:
			return { x, y };

		case AxisOrientation::Rotate90:
			return { y, -x };

		case AxisOrientation::Rotate180:
			return { -x, -y };

		case AxisOrientation::Rotate270:
			return { -y, x };

		case AxisOrientation::Flipped:
			return { -x, y };

		case AxisOrientation::Flipped90:
			return { y, x };

		case AxisOrientation::Flipped180:
			return { x, -y };

		case AxisOrientation::Flipped270:
			return { -y, -x };
	}

	return {};
}

// A transform between two spaces computes in the wider of their scalars and narrows once, at the
// end. Global is the only double space (Space.h says why), so this is double wherever global is an
// endpoint and single everywhere else — which is Architecture.md#the-spaces' rule about what crosses
// the publication boundary at what width, arrived at from the type rather than remembered.
template<SpaceTag From, SpaceTag To>
using TransformScalar = std::common_type_t<typename From::Scalar, typename To::Scalar>;

// A cast that is not a cast when there is nothing to convert. -Wuseless-cast is on, and in a
// template the source and destination scalars are the same type for half the instantiations and
// different for the other half, so a bare static_cast is either the conversion the code needs or a
// diagnostic — depending on arguments the writer of the line cannot see.
template<typename Target, typename Value>
[[nodiscard]] constexpr Target ScalarCast(Value value) noexcept
{
	if constexpr (std::is_same_v<Target, Value>)
	{
		return value;
	}
	else
	{
		return static_cast<Target>(value);
	}
}

// Whether a real value is exactly an integer. Exactly, rather than within a tolerance: a tolerance
// here is a claim about how much softness a person cannot see, which is a property of a display and
// not of a number, and decision 53 already refused the same offer in the arithmetic half of this
// module. Nothing is lost by it, because the path that cares is the one decision 54 has just
// rounded onto the grid, and the value it produced is integral by construction.
template<std::floating_point T>
[[nodiscard]] constexpr bool IsWholeNumber(T value) noexcept
{
	// Past 2^52 the spacing of a double is already one, and of a float long before that, so
	// everything up here is an integer and the round trip below would leave int64's range trying to
	// establish it.
	constexpr T Ceiling = ScalarCast<T>(9.0e15);

	if (!(value > -Ceiling && value < Ceiling))
	{
		return true;
	}

	return value == ScalarCast<T>(static_cast<std::int64_t>(value));
}
} // namespace Detail

// What a transform is, graded. One computation with several readings, because its consumers want
// different rungs of one ladder and a bool would be copied to the second consumer with a tweak.
//
// Docs/Architecture.md#resample-once-and-know-when-it-is-zero requires this to exist before it has a
// second caller: plane promotion, damage rectangle mapping, and the sharpness path all ask about the
// same transform, and three independently derived answers is how they come to disagree about one
// surface on one output at one scale.
//
// **It does not decide where anything is placed.** Settled geometry snaps to the output's device
// grid unconditionally, including a node whose interior is being resampled — the crispness a snap
// buys is mostly in node edges, decoration borders, and tiling adjacency, all of which are
// one-to-one even when the interior is not, and Docs/Experience.md's "two tiled windows meet with no
// line of background showing between them" is stated with no condition attached to it. A snap that
// consulted this would keep that promise only for the nodes that happened to be sampling one-to-one.
// So this is diagnostic, and it decides quality and promotion; the snap path must not read it. That
// reading of decision 54 is newer than the decision log's entry for it.
struct TransformClass
{
	// The image of an axis-aligned rectangle is an axis-aligned rectangle, so a rectangle maps to a
	// rectangle with no dilation beyond the resampling filter's support radius. Every well-formed
	// AxisTransform sets this; what does not is a general affine that failed to reduce to one, which
	// reports the whole ladder unset rather than a rung of it.
	bool AxisAligned = false;

	// No turn and no flip. Read against a plane's rotation property, which most planes do not have.
	bool Upright = false;

	// Exactly one. Read against a plane's scaler, and half of the sharpness question.
	bool UnitScale = false;

	// The translation lands on the destination space's integer grid — and a space with no grid can
	// never set this, which is decision 52's rule showing up a second time: there is no such thing as
	// being pixel-aligned in global space, because global space has no pixels.
	bool IntegerOffset = false;

	// Damage rectangle mapping's rung, and the cheapest one. A rectangle in, a rectangle out.
	[[nodiscard]] constexpr bool MapsRectangles() const noexcept { return AxisAligned; }

	// The sharpness rung: the resample is a no-op, so a surface sampled through this is texel for
	// pixel and stays sharp. A quarter turn is admitted deliberately — it permutes texels and filters
	// nothing — which is why Upright is a separate bit that this reading does not consult.
	[[nodiscard]] constexpr bool IsResampleFree() const noexcept { return AxisAligned && UnitScale && IntegerOffset; }

	// The spatial half of plane promotion, and only the half that is gyro's. A plane's destination is
	// integer CRTC pixels, so a layer at a half-pixel offset cannot be handed to one without moving
	// it, and moving it is the visible flash Docs/Architecture.md#direct-scanout-is-conditional
	// refuses. What is left belongs to the plane assigner, which has what this cannot: the plane's
	// own capabilities, read against Upright and UnitScale, and the source extent, without which
	// "the destination rectangle is whole" is not a question about the transform alone.
	//
	// Note what is deliberately not required. A fractional scale is promotable here, because
	// decision 56 makes minification the common case and answering this with IsResampleFree would
	// foreclose promotion on every fractionally scaled output.
	[[nodiscard]] constexpr bool IsPlaneExpressible() const noexcept { return AxisAligned && IntegerOffset; }

	friend constexpr bool operator==(TransformClass, TransformClass) noexcept = default;
};

// The adapter itself, from one space to another. The spaces are template parameters for Space.h's
// reason: the output adapter and the panel adapter are then two types rather than two variables of
// one, composing them in the wrong order does not compile, and the chain in
// Docs/Architecture.md#the-spaces is checked rather than commented.
//
// An aggregate, because this is a shape that crosses the publication boundary with the output
// configuration and the frame side reads it as bytes. So the invariants — a positive finite scale,
// an orientation that is one of the eight — hold by construction at every site that builds one and
// are not enforced here, exactly as Space.h leaves a Size non-negative and Core/Handle.h leaves an
// id unforged. IsValid is where anything that cares asks, and Classify asks on its behalf.
template<SpaceTag From, SpaceTag To>
struct AxisTransform
{
	using Source = From;
	using Target = To;
	using Scalar = Detail::TransformScalar<From, To>;

	// Declared in the order they apply: v goes to Translation + Scale * Orientation(v). The default
	// is the identity rather than a null state, for Scale's reason — every adapter in the system is a
	// real one, and a value meaning "no transform" would only be a thing to check for at sites
	// already obliged to have asked.
	AxisOrientation Orientation = AxisOrientation::Normal;

	// Real numbers and not a Scale. Geometry/Scale.h says why at length: an output's scale is an
	// exact rational in 120ths and does integer size arithmetic, while these are the composed ratios
	// a surface is actually resampled by — output scale over buffer scale, which is not expressible
	// in 120ths and has no integer size arithmetic to be exact for.
	//
	// **In the destination's axes**, which is the half of this that has to be stated rather than
	// inferred. The scale multiplies after the orientation, so ScaleX stretches the destination's x
	// and not the source's, and that is the frame `wp_viewport` states `dst` in — the protocol's own
	// construction rather than a convention chosen here. It is also what makes composition an axis
	// exchange rather than a matrix: pushing a scale past a quarter turn swaps the two factors, and
	// Compose below is that one line.
	Scalar ScaleX = Scalar{ 1 };
	Scalar ScaleY = Scalar{ 1 };

	// In the destination space, because that is where it is added. An adapter's translation is
	// therefore on the grid the destination has, or on no grid at all where the destination is the
	// model.
	Offset<To, Scalar> Translation{};

	[[nodiscard]] static constexpr AxisTransform Identity() noexcept { return AxisTransform{}; }

	[[nodiscard]] constexpr bool IsValid() const noexcept
	{
		// Both axes, and NaN fails the comparison written in this direction, which is the point of
		// writing it in this direction. An adapter with one good axis is not half valid: it collapses
		// a rectangle to a line on the other one, and every consumer below would rather be told.
		return ScaleX > Scalar{} && ScaleX < std::numeric_limits<Scalar>::infinity() && ScaleY > Scalar{} &&
		       ScaleY < std::numeric_limits<Scalar>::infinity() &&
		       static_cast<std::uint8_t>(Orientation) < OrientationCount;
	}

	[[nodiscard]] constexpr TransformClass Classify() const noexcept
	{
		// A malformed transform is not a rung of the ladder. It reports what a general affine that
		// would not reduce reports, so a consumer that forgot to check validity separately declines
		// to promote and declines to call the resample free, which are the safe answers to both.
		if (!IsValid())
		{
			return {};
		}

		return {
			.AxisAligned = true,
			.Upright = Orientation == AxisOrientation::Normal,
			.UnitScale = ScaleX == Scalar{ 1 } && ScaleY == Scalar{ 1 },
			.IntegerOffset =
				To::HasGrid && Detail::IsWholeNumber(Translation.X) && Detail::IsWholeNumber(Translation.Y),
		};
	}

	// The result is real-valued in the destination space, always, whatever went in. Landing it on
	// that space's grid is a rounding; a rounding is a function of (node, output, frame) per decision
	// 52, and its direction belongs to whoever is asking — a damage rectangle wants the enclosing
	// integer rectangle plus the filter's support radius, and a scissor rectangle wants something
	// else again. Neither is spelled here, because neither is a property of the transform.
	template<typename T>
		requires CoordinateOf<From, T>
	[[nodiscard]] constexpr Point<To, typename To::Scalar> Map(Point<From, T> point) const noexcept
	{
		const Detail::Axes<Scalar> turned =
			Detail::Orient(Orientation, Detail::ScalarCast<Scalar>(point.X), Detail::ScalarCast<Scalar>(point.Y));

		return { Detail::ScalarCast<typename To::Scalar>(Translation.X + ScaleX * turned.X),
			     Detail::ScalarCast<typename To::Scalar>(Translation.Y + ScaleY * turned.Y) };
	}

	// A displacement does not translate. Two positions and the displacement between them have to
	// still be two positions and the displacement between them on the far side of an adapter, and
	// they are not if the translation is applied three times.
	template<typename T>
		requires CoordinateOf<From, T>
	[[nodiscard]] constexpr Offset<To, typename To::Scalar> Map(Offset<From, T> offset) const noexcept
	{
		const Detail::Axes<Scalar> turned =
			Detail::Orient(Orientation, Detail::ScalarCast<Scalar>(offset.X), Detail::ScalarCast<Scalar>(offset.Y));

		return { Detail::ScalarCast<typename To::Scalar>(ScaleX * turned.X),
			     Detail::ScalarCast<typename To::Scalar>(ScaleY * turned.Y) };
	}

	// An extent has no sign, so an orientation exchanges width and height and does nothing else to
	// it. Running a Size through the matrix would negate one of them on six of the eight
	// orientations and produce the negative extent Space.h's IsValid exists to catch downstream.
	//
	// The exchange happens before the scale rather than after, which is where a per-axis scale is
	// easiest to get wrong: ScaleX belongs to the destination's x axis, so on a quarter turn it
	// multiplies what used to be the source's height.
	template<typename T>
		requires CoordinateOf<From, T>
	[[nodiscard]] constexpr Size<To, typename To::Scalar> Map(Size<From, T> size) const noexcept
	{
		const Scalar width = Detail::ScalarCast<Scalar>(SwapsAxes(Orientation) ? size.Height : size.Width);
		const Scalar height = Detail::ScalarCast<Scalar>(SwapsAxes(Orientation) ? size.Width : size.Height);

		return { Detail::ScalarCast<typename To::Scalar>(ScaleX * width),
			     Detail::ScalarCast<typename To::Scalar>(ScaleY * height) };
	}

	// Two corners across, and the extent derived from them — Space.h's FromEdges rather than a
	// mapped origin beside a separately mapped extent, for the reason its comment gives. The corners
	// exchange places under a flip or a turn, so which one is top-left is recovered rather than
	// assumed; assuming it yields a negative extent on exactly the outputs that are rotated.
	template<typename T>
		requires CoordinateOf<From, T>
	[[nodiscard]] constexpr Rect<To, typename To::Scalar> Map(Rect<From, T> rect) const noexcept
	{
		using Result = typename To::Scalar;

		const Point<To, Result> first = Map(rect.Origin);
		const Point<To, Result> second = Map(Point<From, T>{ rect.Right(), rect.Bottom() });

		return Rect<To, Result>::FromEdges(
			{ first.X < second.X ? first.X : second.X, first.Y < second.Y ? first.Y : second.Y },
			{ first.X < second.X ? second.X : first.X, first.Y < second.Y ? second.Y : first.Y }
		);
	}

	// Closure under inversion is what makes hit-testing and pointer constraint one adapter rather
	// than a second one maintained alongside it and able to disagree. The scale is where exactness
	// stops: 1/s is exact for a power of two and rounds for 1.1, so an inverse round trip returns
	// the value to within an ulp rather than to itself. That is a property of the reciprocal and not
	// of this set, which is closed regardless.
	//
	// The reciprocals cross where the orientation swaps axes, for the reason Compose's do: the
	// inverse's scale is stated in *its* destination, which is this transform's source.
	[[nodiscard]] constexpr AxisTransform<To, From> Inverse() const noexcept
	{
		const AxisOrientation inverse = Invert(Orientation);
		const bool swapped = SwapsAxes(Orientation);
		const Scalar reciprocalX = Scalar{ 1 } / (swapped ? ScaleY : ScaleX);
		const Scalar reciprocalY = Scalar{ 1 } / (swapped ? ScaleX : ScaleY);
		const Detail::Axes<Scalar> turned = Detail::Orient(inverse, Translation.X, Translation.Y);

		using Result = AxisTransform<To, From>;
		using ResultScalar = typename Result::Scalar;

		return { inverse,
			     Detail::ScalarCast<ResultScalar>(reciprocalX),
			     Detail::ScalarCast<ResultScalar>(reciprocalY),
			     { Detail::ScalarCast<ResultScalar>(-reciprocalX * turned.X),
			       Detail::ScalarCast<ResultScalar>(-reciprocalY * turned.Y) } };
	}

	friend constexpr bool operator==(AxisTransform, AxisTransform) noexcept = default;
};

// `first`, then `second`, and the spaces make that the only order that compiles. Closure is the
// whole point: an output adapter composed with a panel adapter is one adapter with one resample,
// which is what Docs/Architecture.md#resample-once-and-know-when-it-is-zero asks for and what the
// classification is then asked about. Closure is structural — the composition of two members of this
// set is a member of it, with no residue to drop — and the arithmetic is the scalar's, one rounding
// per product.
//
// The one line worth reading twice is the axis exchange. Each scale is stated in its own
// destination's axes, so the first transform's factors reach the result through the second's
// orientation: composing through a quarter turn multiplies the result's x by the first transform's
// *y* factor. Getting it backwards is invisible for as long as every adapter in the chain happens to
// be uniform, which is every arrangement anybody tests by hand, and shows up on a rotated output
// playing anamorphic video. The exhaustive check at the foot of this header carries a non-uniform
// scale through all sixty-four orientation pairs for exactly that reason.
template<SpaceTag A, SpaceTag B, SpaceTag C>
[[nodiscard]] constexpr AxisTransform<A, C> Compose(AxisTransform<A, B> first, AxisTransform<B, C> second) noexcept
{
	using Result = AxisTransform<A, C>;
	using Scalar = typename Result::Scalar;

	const Scalar scaleX = Detail::ScalarCast<Scalar>(second.ScaleX);
	const Scalar scaleY = Detail::ScalarCast<Scalar>(second.ScaleY);
	const bool swapped = SwapsAxes(second.Orientation);
	const Scalar carriedX = Detail::ScalarCast<Scalar>(swapped ? first.ScaleY : first.ScaleX);
	const Scalar carriedY = Detail::ScalarCast<Scalar>(swapped ? first.ScaleX : first.ScaleY);

	const Detail::Axes<Scalar> turned = Detail::Orient(
		second.Orientation,
		Detail::ScalarCast<Scalar>(first.Translation.X),
		Detail::ScalarCast<Scalar>(first.Translation.Y)
	);

	return { Compose(first.Orientation, second.Orientation),
		     scaleX * carriedX,
		     scaleY * carriedY,
		     { Detail::ScalarCast<Scalar>(second.Translation.X) + scaleX * turned.X,
		       Detail::ScalarCast<Scalar>(second.Translation.Y) + scaleY * turned.Y } };
}

// Prints as flipped-90, which is `wl_output_transform`'s own name for it minus the prefix — so a
// log line and a protocol dump say the same word about the same value. No format spec is accepted.
//
// The context is a template parameter for the reason recorded at length in Core/Handle.h: naming
// std::format_context leaves std::format working and std::formattable false, so a value goes missing
// from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<AxisOrientation>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(AxisOrientation orientation, Context& context) const
	{
		return std::format_to(context.out(), "{}", OrientationName(orientation));
	}
};

// Prints as global->device[90 1.5x (+12, +40)], and as global->device[90 2x/1.5x (+12, +40)] where
// the two axes differ. Everything a reader needs to reproduce the mapping by hand, in the order it
// applies, with the pair of spaces in front for Space.h's reason: the same numbers describe a
// different mapping between a different pair, and a log that elided the pair would be the one place
// that distinction is invisible.
//
// The uniform case keeps the short spelling because it is the one a reader sees a thousand times and
// because "1.5x/1.5x" would be a second way of writing it. The pair appears exactly where it means
// something, which also makes an unintended non-uniform scale visible in a log rather than being
// something to go looking for.
template<SpaceTag From, SpaceTag To>
struct std::formatter<AxisTransform<From, To>>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(AxisTransform<From, To> transform, Context& context) const
	{
		auto out =
			std::format_to(context.out(), "{}->{}[{} ", From::Name, To::Name, OrientationName(transform.Orientation));

		if (transform.ScaleX == transform.ScaleY)
		{
			out = std::format_to(out, "{}x", transform.ScaleX);
		}
		else
		{
			out = std::format_to(out, "{}x/{}x", transform.ScaleX, transform.ScaleY);
		}

		return std::format_to(out, " ({:+}, {:+})]", transform.Translation.X, transform.Translation.Y);
	}
};

// Prints as class(axis-aligned upright unit-scale integer-offset), or class(none). The set rather
// than a rung name, because there is no single ladder here — the three consumers read three
// different subsets, so a report that named one rung would be answering a question its reader may
// not have been asking.
template<>
struct std::formatter<TransformClass>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(TransformClass value, Context& context) const
	{
		auto out = std::format_to(context.out(), "class(");

		if (value == TransformClass{})
		{
			return std::format_to(out, "none)");
		}

		std::string_view separator = "";
		const auto append = [&out, &separator](bool set, std::string_view name) {
			if (set)
			{
				out = std::format_to(out, "{}{}", separator, name);
				separator = " ";
			}
		};

		append(value.AxisAligned, "axis-aligned");
		append(value.Upright, "upright");
		append(value.UnitScale, "unit-scale");
		append(value.IntegerOffset, "integer-offset");

		return std::format_to(out, ")");
	}
};

namespace Detail
{
// The assertion block wants a name for one of these, because offsetof is a macro and a template
// argument list has a comma in it.
using ProbeAdapter = AxisTransform<GlobalSpace, DeviceSpace>;

// The group law and the scale exchange, checked exhaustively rather than at the two points somebody
// thought of, and in one sweep because they are one claim: composing two of these and applying them
// one after the other are the same computation. Sixty-four pairs, each checked twice over —
//
//   as orientations, against the matrices themselves. An orientation is a linear map, so the two
//   basis vectors determine it, and this is what pins the convention the rest of the header states
//   in prose. The flipped half of the table is the half most implementations get wrong; it is wrong
//   by a half turn rather than by anything structural, and a mirrored output is rare enough that the
//   mistake ships.
//
//   as whole transforms carrying a **non-uniform** scale. The factors are deliberately unequal and
//   deliberately different between the two transforms, so a composition that failed to exchange them
//   through a quarter turn cannot pass by symmetry. Every value here is dyadic, which makes the two
//   routes exact and the comparison an equality rather than a tolerance.
[[nodiscard]] constexpr bool CompositionHolds() noexcept
{
	using Probe = AxisTransform<DeviceSpace, DeviceSpace>;

	for (std::uint8_t a = 0; a < OrientationCount; ++a)
	{
		const AxisOrientation firstOrientation = static_cast<AxisOrientation>(a);

		for (std::uint8_t b = 0; b < OrientationCount; ++b)
		{
			const AxisOrientation secondOrientation = static_cast<AxisOrientation>(b);
			const AxisOrientation composedOrientation = Compose(firstOrientation, secondOrientation);

			for (const Axes<int> probe : { Axes<int>{ 1, 0 }, Axes<int>{ 0, 1 } })
			{
				const Axes<int> once = Orient(firstOrientation, probe.X, probe.Y);
				const Axes<int> stepwise = Orient(secondOrientation, once.X, once.Y);
				const Axes<int> direct = Orient(composedOrientation, probe.X, probe.Y);

				if (stepwise.X != direct.X || stepwise.Y != direct.Y)
				{
					return false;
				}
			}

			const Probe first{ firstOrientation, 2.0F, 4.0F, { 3.0F, -5.0F } };
			const Probe second{ secondOrientation, 0.5F, 8.0F, { -7.0F, 1.0F } };
			const Probe composed = Compose(first, second);

			for (const Point<DeviceSpace> probe : { Point<DeviceSpace>{ 1.0F, 0.0F },
			                                        Point<DeviceSpace>{ 0.0F, 1.0F },
			                                        Point<DeviceSpace>{ -3.0F, 6.0F } })
			{
				if (composed.Map(probe) != second.Map(first.Map(probe)))
				{
					return false;
				}
			}
		}
	}

	return true;
}

// And every one of them has an inverse in the set, with the same exchange in it. Composed rather
// than applied one after the other, so what is checked is that the whole thing collapses to the
// identity exactly — a reciprocal pair left uncrossed under a quarter turn comes back as a scale of
// four rather than of one, and a translation inverted before the orientation comes back as an offset.
[[nodiscard]] constexpr bool InversionHolds() noexcept
{
	using Probe = AxisTransform<DeviceSpace, DeviceSpace>;

	for (std::uint8_t a = 0; a < OrientationCount; ++a)
	{
		const AxisOrientation orientation = static_cast<AxisOrientation>(a);

		if (Compose(orientation, Invert(orientation)) != AxisOrientation::Normal ||
		    Compose(Invert(orientation), orientation) != AxisOrientation::Normal)
		{
			return false;
		}

		const Probe transform{ orientation, 2.0F, 4.0F, { 3.0F, -5.0F } };

		if (Compose(transform, transform.Inverse()) != Probe::Identity() ||
		    Compose(transform.Inverse(), transform) != Probe::Identity())
		{
			return false;
		}
	}

	return true;
}
} // namespace Detail

// The contract everything downstream assumes. The runtime half waits on the test harness.
static_assert(
	std::is_trivially_copyable_v<Detail::ProbeAdapter> && std::is_standard_layout_v<Detail::ProbeAdapter>,
	"An output adapter crosses the publication boundary with the output configuration"
);
static_assert(std::is_aggregate_v<Detail::ProbeAdapter> && std::is_aggregate_v<TransformClass>);

// Field order, since the frame side resolves the snapshot from a base address that differs from the
// one it was written at. There is real padding after the orientation byte and there is no arrangement
// of these four that removes it, so unlike Space.h's types this one is not bit_cast to an array —
// a padding byte is indeterminate and would take the assertion out of constant evaluation. Order is
// what the boundary needs and order is what is guaranteed.
static_assert(offsetof(Detail::ProbeAdapter, Orientation) == 0);
static_assert(offsetof(Detail::ProbeAdapter, ScaleX) < offsetof(Detail::ProbeAdapter, ScaleY));
static_assert(offsetof(Detail::ProbeAdapter, ScaleY) < offsetof(Detail::ProbeAdapter, Translation));

// The wire's numbering, which is the reason for using its enum at all. A protocol adapter that
// static_casts an incoming wl_output_transform is correct by this and by nothing else.
static_assert(static_cast<std::uint8_t>(AxisOrientation::Normal) == 0);
static_assert(static_cast<std::uint8_t>(AxisOrientation::Rotate90) == 1);
static_assert(static_cast<std::uint8_t>(AxisOrientation::Rotate180) == 2);
static_assert(static_cast<std::uint8_t>(AxisOrientation::Rotate270) == 3);
static_assert(static_cast<std::uint8_t>(AxisOrientation::Flipped) == 4);
static_assert(static_cast<std::uint8_t>(AxisOrientation::Flipped90) == 5);
static_assert(static_cast<std::uint8_t>(AxisOrientation::Flipped180) == 6);
static_assert(static_cast<std::uint8_t>(AxisOrientation::Flipped270) == 7);

static_assert(Detail::CompositionHolds(), "The set is closed under composition, scale exchange included");
static_assert(Detail::InversionHolds(), "And every member has an inverse in it, with the same exchange");
static_assert(SwapsAxes(AxisOrientation::Rotate90) && SwapsAxes(AxisOrientation::Flipped270));
static_assert(!SwapsAxes(AxisOrientation::Rotate180) && !SwapsAxes(AxisOrientation::Flipped));

// The convention, pinned. A quarter turn takes the +x axis to -y, and the flip is about the vertical
// axis — which is what the protocol says and what everything downstream will assume without asking.
static_assert(
	Detail::Orient(AxisOrientation::Rotate90, 1, 0).X == 0 && Detail::Orient(AxisOrientation::Rotate90, 1, 0).Y == -1
);
static_assert(
	Detail::Orient(AxisOrientation::Flipped, 1, 2).X == -1 && Detail::Orient(AxisOrientation::Flipped, 1, 2).Y == 2
);
static_assert(
	Detail::Orient(AxisOrientation::Flipped90, 1, 2).X == 2 && Detail::Orient(AxisOrientation::Flipped90, 1, 2).Y == 1
);

// The flip comes first and then the turn, which is the half of the convention that is a choice.
static_assert(Compose(AxisOrientation::Flipped, AxisOrientation::Rotate90) == AxisOrientation::Flipped90);
static_assert(Compose(AxisOrientation::Rotate90, AxisOrientation::Flipped) == AxisOrientation::Flipped270);
static_assert(Compose(AxisOrientation::Rotate270, AxisOrientation::Rotate180) == AxisOrientation::Rotate90);
static_assert(Compose(AxisOrientation::Flipped90, AxisOrientation::Flipped90) == AxisOrientation::Normal);

// Precision follows the spaces rather than the transform. An adapter with global at either end
// computes in double; one between two device grids stays single, which is what keeps the panel
// adapter the size it should be.
static_assert(std::is_same_v<AxisTransform<GlobalSpace, DeviceSpace>::Scalar, double>);
static_assert(std::is_same_v<AxisTransform<DeviceSpace, GlobalSpace>::Scalar, double>);
static_assert(std::is_same_v<AxisTransform<DeviceSpace, DeviceSpace>::Scalar, float>);
static_assert(std::is_same_v<AxisTransform<BufferSpace, SurfaceSpace>::Scalar, float>);

// Two adapters are two types. Composing the output adapter with itself does not compile, and
// neither does handing the panel adapter where the output adapter belongs.
static_assert(!std::is_convertible_v<AxisTransform<GlobalSpace, DeviceSpace>, AxisTransform<DeviceSpace, DeviceSpace>>);
static_assert(std::is_same_v<
			  decltype(AxisTransform<GlobalSpace, DeviceSpace>{}.Inverse()),
			  AxisTransform<DeviceSpace, GlobalSpace>>);

static_assert(AxisTransform<GlobalSpace, DeviceSpace>{} == AxisTransform<GlobalSpace, DeviceSpace>::Identity());
static_assert(AxisTransform<GlobalSpace, DeviceSpace>::Identity().IsValid());
static_assert(
	!AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 0.0, 1.0, {} }.IsValid(),
	"Both axes are positive"
);
static_assert(!AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 1.0, -1.0, {} }.IsValid());
static_assert(!AxisTransform<GlobalSpace, DeviceSpace>{ static_cast<AxisOrientation>(9), 1.0, 1.0, {} }.IsValid());

// A 1920x1080 output at 1.5, placed at the origin of the global arrangement. The edges of a
// three-logical-pixel window convert and the extent is their difference, which is Space.h's
// FromEdges rule surviving the transform rather than being re-established after it.
static_assert(
	AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 1.5, 1.5, { 0.0, 0.0 } }
		.Map(Rect<GlobalSpace>{ { 1.0, 1.0 }, { 3.0, 3.0 } }) == Rect<DeviceSpace>{ { 1.5F, 1.5F }, { 4.5F, 4.5F } }
);

// The panel adapter, which is the case the whole restricted type exists for: a quarter turn of a
// 1920x1080 device grid onto a 1080x1920 panel. The corners exchange places and the extent comes
// out positive, which is the thing an assumed top-left corner gets wrong.
static_assert(
	AxisTransform<DeviceSpace, DeviceSpace>{ AxisOrientation::Rotate90, 1.0F, 1.0F, { 0.0F, 1920.0F } }.Map(
		Rect<DeviceSpace>{ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } }
	) == Rect<DeviceSpace>{ { 0.0F, 0.0F }, { 1080.0F, 1920.0F } }
);

// A displacement does not translate and an extent does not take a sign.
static_assert(
	AxisTransform<DeviceSpace, DeviceSpace>{ AxisOrientation::Rotate90, 2.0F, 2.0F, { 5.0F, 7.0F } }
		.Map(Offset<DeviceSpace>{ 1.0F, 0.0F }) == Offset<DeviceSpace>{ 0.0F, -2.0F }
);
static_assert(
	AxisTransform<DeviceSpace, DeviceSpace>{ AxisOrientation::Rotate90, 2.0F, 2.0F, { 5.0F, 7.0F } }
		.Map(Size<DeviceSpace>{ 640.0F, 480.0F }) == Size<DeviceSpace>{ 960.0F, 1280.0F }
);

// The surface adapter, which is what the per-axis scale is here for: a 640x480 anamorphic buffer
// presented at 1280x720, which is `wp_viewport`'s src and dst declining to share an aspect ratio.
// A single factor would have to pick one of these two numbers and be wrong about the other.
static_assert(
	AxisTransform<BufferSpace, SurfaceSpace>{ AxisOrientation::Normal, 2.0F, 1.5F, {} }
		.Map(Size<BufferSpace>{ 640.0F, 480.0F }) == Size<SurfaceSpace>{ 1280.0F, 720.0F }
);

// And the same buffer through a quarter turn, which is where the two factors have to cross. ScaleX
// is the destination's x, so it multiplies what was the buffer's height: 480 by 2 and 640 by 1.5,
// rather than the 960 by 720 an uncrossed pair would produce.
static_assert(
	AxisTransform<BufferSpace, SurfaceSpace>{ AxisOrientation::Rotate90, 2.0F, 1.5F, {} }
		.Map(Size<BufferSpace>{ 640.0F, 480.0F }) == Size<SurfaceSpace>{ 960.0F, 960.0F }
);

// Composition against sequential application, on the chain the design actually has: global onto a
// scaled output, then that output onto a rotated panel.
static_assert(
	Compose(
		AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 2.0, 2.0, { 10.0, 20.0 } },
		AxisTransform<DeviceSpace, DeviceSpace>{ AxisOrientation::Rotate90, 1.0F, 1.0F, { 0.0F, 1080.0F } }
	)
		.Map(Point<GlobalSpace>{ 3.0, 4.0 }) ==
	AxisTransform<DeviceSpace, DeviceSpace>{ AxisOrientation::Rotate90, 1.0F, 1.0F, { 0.0F, 1080.0F } }.Map(
		AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 2.0, 2.0, { 10.0, 20.0 } }
			.Map(Point<GlobalSpace>{ 3.0, 4.0 })
	)
);

// The exchange, stated once at a point as well as swept over all sixty-four pairs in
// Detail::CompositionHolds. A non-uniform surface adapter composed through a quarter turn: the
// buffer's 2 and 1.5 arrive on the destination's y and x respectively, so the composed scale is
// 1.5 by 2 rather than 2 by 1.5. This is the assertion that fails if the exchange is dropped.
static_assert(
	Compose(
		AxisTransform<BufferSpace, SurfaceSpace>{ AxisOrientation::Normal, 2.0F, 1.5F, {} },
		AxisTransform<SurfaceSpace, SurfaceSpace>{ AxisOrientation::Rotate90, 1.0F, 1.0F, {} }
	) == AxisTransform<BufferSpace, SurfaceSpace>{ AxisOrientation::Rotate90, 1.5F, 2.0F, {} }
);

// An adapter composed with its own inverse is the identity, exactly, where the scales are powers of
// two. This is the round trip pointer input takes, and it takes it every frame.
static_assert(
	Compose(
		AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Rotate90, 2.0, 4.0, { 10.0, 20.0 } },
		AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Rotate90, 2.0, 4.0, { 10.0, 20.0 } }.Inverse()
	) == AxisTransform<GlobalSpace, GlobalSpace>::Identity()
);

// The ladder. One computation, and the three consumers reading three different subsets of it.
static_assert(AxisTransform<GlobalSpace, DeviceSpace>::Identity().Classify().IsResampleFree());
static_assert(AxisTransform<GlobalSpace, DeviceSpace>::Identity().Classify().IsPlaneExpressible());

// A quarter turn at unit scale is still resample-free: it permutes texels and filters nothing. The
// rotated output is not the soft one.
static_assert(AxisTransform<DeviceSpace, DeviceSpace>{ AxisOrientation::Rotate90, 1.0F, 1.0F, { 0.0F, 1080.0F } }
                  .Classify()
                  .IsResampleFree());
static_assert(!AxisTransform<DeviceSpace, DeviceSpace>{ AxisOrientation::Rotate90, 1.0F, 1.0F, {} }.Classify().Upright);

// Unit scale is both axes, and an adapter that resamples one of them resamples. An anamorphic
// surface is the case that would otherwise report itself sharp on the strength of the axis that
// happened to be checked.
static_assert(
	!AxisTransform<BufferSpace, SurfaceSpace>{ AxisOrientation::Normal, 1.0F, 1.5F, {} }.Classify().UnitScale
);
static_assert(
	!AxisTransform<BufferSpace, SurfaceSpace>{ AxisOrientation::Normal, 1.0F, 1.5F, {} }.Classify().IsResampleFree()
);

// Decision 56's common case. A fractional scale resamples and must not be called sharp; it is still
// promotable, and answering the plane's question with the sharpness path's answer would foreclose
// promotion on every fractionally scaled output.
static_assert(
	!AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 1.5, 1.5, {} }.Classify().IsResampleFree()
);
static_assert(
	AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 1.5, 1.5, {} }.Classify().IsPlaneExpressible()
);

// Half a device pixel, which is the offset that makes text soft and the one a plane cannot express.
// Damage still maps rectangle to rectangle, which is the rung damage was always reading.
static_assert(!AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 1.0, 1.0, { 0.5, 0.0 } }
                   .Classify()
                   .IsResampleFree());
static_assert(!AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 1.0, 1.0, { 0.5, 0.0 } }
                   .Classify()
                   .IsPlaneExpressible());
static_assert(AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 1.0, 1.0, { 0.5, 0.0 } }
                  .Classify()
                  .MapsRectangles());

// Decision 52 once more, in the classification. There is no being pixel-aligned in global space,
// because global space has no pixels — so an adapter pointing back into the model cannot report an
// integer offset however round its translation is.
static_assert(
	!AxisTransform<DeviceSpace, GlobalSpace>{ AxisOrientation::Normal, 1.0, 1.0, { 4.0, 4.0 } }.Classify().IntegerOffset
);
static_assert(
	AxisTransform<DeviceSpace, GlobalSpace>{ AxisOrientation::Normal, 1.0, 1.0, { 4.0, 4.0 } }.Classify().AxisAligned
);

// A malformed transform reports the empty ladder, which is what a general affine that would not
// reduce to this type reports. Every reading is false, so every consumer takes its safe branch.
static_assert(
	AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, -1.0, 1.0, {} }.Classify() == TransformClass{}
);

static_assert(Detail::IsWholeNumber(4.0) && !Detail::IsWholeNumber(4.5));
static_assert(Detail::IsWholeNumber(-4.0) && !Detail::IsWholeNumber(-0.5));
static_assert(Detail::IsWholeNumber(1.0e300), "Past the point where the spacing is one, everything is an integer");

static_assert(std::formattable<AxisTransform<GlobalSpace, DeviceSpace>, char>, "A report prints the adapter");
static_assert(std::formattable<AxisOrientation, char>);
static_assert(std::formattable<TransformClass, char>);
