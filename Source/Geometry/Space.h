#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <format>
#include <string_view>
#include <type_traits>

// The coordinate spaces, and the values that live in them.
//
// A compositor's geometry bugs are nearly all one of two mistakes: a value used in a space it does
// not belong to, and a value that got rounded once and then stored. Both are compile-time
// preventable and neither is preventable by care, so the space is part of the type and the grid is
// part of the type, and going between them is a named conversion somebody had to write.
//
// The chain is buffer -> surface -> global -> device, and every step is an *adapter* that is exact
// and declared rather than inferred — the surface adapter from buffer_transform, buffer_scale and
// the viewport's src and dst; the output adapter from output configuration. Nothing here recomputes
// one from a scale factor. A fifth space, panel, arrives with the DRM backend: a ninety-degree
// rotation may be executed by a KMS plane or by us, and direct scanout depends on knowing which.
//
// See Docs/Decisions.md decision 52 and Docs/Architecture.md#the-spaces.

// A space carries its name for logs, the scalar its coordinates are real-valued in, and whether it
// has an integer grid at its boundary.
template<typename T>
concept SpaceTag = requires {
	{ T::Name } -> std::convertible_to<std::string_view>;
	typename T::Scalar;
	{ T::HasGrid } -> std::convertible_to<bool>;
};

// Integer texels, one space per attached buffer, and the client decides it. Real-valued because the
// viewport's src rectangle is wl_fixed and therefore samples between texels; the integer grid is
// what the buffer's own extents are on.
struct BufferSpace
{
	static constexpr std::string_view Name = "buffer";
	using Scalar = float;
	static constexpr bool HasGrid = true;
};

// Surface-local, one per surface. Single precision because a surface is bounded by its own size, so
// nothing here is far from the origin — and because wl_fixed's 1/256 is the resolution the wire
// delivers pointer positions in anyway. The grid exists because wl_subsurface.set_position is
// integer surface-local, which is a limitation of the protocol rather than a choice.
struct SurfaceSpace
{
	static constexpr std::string_view Name = "surface";
	using Scalar = float;
	static constexpr bool HasGrid = true;
};

// Exactly one, continuous, output-independent, and Y-down — which is Wayland's convention and
// Vulkan's clip space alike. Its basis unit is one logical pixel at scale 1, for wire compatibility
// and for nothing else: nothing assumes integer alignment in it and no logical size of a window is
// stored anywhere.
//
// Double precision, and this is the one place the width is load-bearing rather than habitual.
// Single gives 1/256 of a pixel around +/-32768, which is exactly wl_fixed's resolution and too
// near the floor for a large arrangement of outputs — a coordinate on the far monitor of a wide
// desk would have no room left underneath it for the sub-pixel offsets everything here is about.
//
// **No grid.** This is decision 52's headline rule expressed where the compiler can hold us to it:
// quantization is a property of an output and never of the world, so an integer global coordinate
// is not a value that rounds badly, it is a value that cannot be spelled. One model position on a
// 1x and a 1.5x output has to produce two different snapped positions, and it cannot do that if the
// rounding already happened before the output was known.
struct GlobalSpace
{
	static constexpr std::string_view Name = "global";
	using Scalar = double;
	static constexpr bool HasGrid = false;
};

// One per output, and the grid the composite target actually has. Real-valued in the interior so
// that a transform composes exactly, integer at the boundary where it meets a scissor rectangle, a
// damage rectangle, or KMS.
struct DeviceSpace
{
	static constexpr std::string_view Name = "device";
	using Scalar = float;
	static constexpr bool HasGrid = true;
};

// A coordinate is either real, or integral in a space that has a grid to be integral on. This is
// what makes Point<GlobalSpace, std::int32_t> fail to compile rather than merely be a bad idea.
template<typename S, typename T>
concept CoordinateOf = SpaceTag<S> && (std::floating_point<T> || (std::integral<T> && S::HasGrid));

// A position. Aggregates throughout, because this is the shape that crosses the publication
// boundary: the frame side reconstitutes it from bytes at an offset rather than through a
// constructor, exactly as Core/Handle.h does.
template<SpaceTag S, typename T = typename S::Scalar>
	requires CoordinateOf<S, T>
struct Point
{
	using Space = S;
	using Scalar = T;

	T X{};
	T Y{};

	friend constexpr bool operator==(Point, Point) noexcept = default;
};

// A displacement. Distinct from Size because a displacement is signed and an extent is not, and
// distinct from Point because two positions do not add — the same argument Core/Time.h makes about
// Instant and Duration, in two dimensions.
template<SpaceTag S, typename T = typename S::Scalar>
	requires CoordinateOf<S, T>
struct Offset
{
	using Space = S;
	using Scalar = T;

	T X{};
	T Y{};

	friend constexpr bool operator==(Offset, Offset) noexcept = default;

	friend constexpr Offset operator+(Offset left, Offset right) noexcept
	{
		return { left.X + right.X, left.Y + right.Y };
	}

	friend constexpr Offset operator-(Offset left, Offset right) noexcept
	{
		return { left.X - right.X, left.Y - right.Y };
	}

	friend constexpr Offset operator-(Offset value) noexcept { return { -value.X, -value.Y }; }

	friend constexpr Offset operator*(Offset value, T factor) noexcept
	{
		return { value.X * factor, value.Y * factor };
	}
};

// An extent. Width and Height are non-negative by invariant rather than by construction, since an
// aggregate has nowhere to check — the same standing Core/Handle.h gives a forged id. IsValid is
// where anything that cares asks.
template<SpaceTag S, typename T = typename S::Scalar>
	requires CoordinateOf<S, T>
struct Size
{
	using Space = S;
	using Scalar = T;

	T Width{};
	T Height{};

	[[nodiscard]] constexpr bool IsValid() const noexcept { return Width >= T{} && Height >= T{}; }
	[[nodiscard]] constexpr bool IsEmpty() const noexcept { return !(Width > T{}) || !(Height > T{}); }

	friend constexpr bool operator==(Size, Size) noexcept = default;
};

template<SpaceTag S, typename T = typename S::Scalar>
	requires CoordinateOf<S, T>
struct Rect
{
	using Space = S;
	using Scalar = T;

	Point<S, T> Origin{};
	Size<S, T> Extent{};

	[[nodiscard]] constexpr T Left() const noexcept { return Origin.X; }
	[[nodiscard]] constexpr T Top() const noexcept { return Origin.Y; }
	[[nodiscard]] constexpr T Right() const noexcept { return Origin.X + Extent.Width; }
	[[nodiscard]] constexpr T Bottom() const noexcept { return Origin.Y + Extent.Height; }

	[[nodiscard]] constexpr bool IsEmpty() const noexcept { return Extent.IsEmpty(); }

	// The constructor that encodes what Geometry/Scale.h's tests are about: edges convert, and an
	// extent is the difference between two converted edges. Building a rect from a converted origin
	// and a separately converted extent is how a row of tiled windows ends up a pixel wider than the
	// space it was given, and this is the spelling that does not let it happen.
	[[nodiscard]] static constexpr Rect FromEdges(Point<S, T> topLeft, Point<S, T> bottomRight) noexcept
	{
		return { topLeft, { bottomRight.X - topLeft.X, bottomRight.Y - topLeft.Y } };
	}

	friend constexpr bool operator==(Rect, Rect) noexcept = default;
};

// Point + Point is ill-formed for Instant + Instant's reason. Point - Point is the displacement
// between them, and Point + Offset is the position that displacement reaches.
template<SpaceTag S, typename T>
[[nodiscard]] constexpr Offset<S, T> operator-(Point<S, T> left, Point<S, T> right) noexcept
{
	return { left.X - right.X, left.Y - right.Y };
}

template<SpaceTag S, typename T>
[[nodiscard]] constexpr Point<S, T> operator+(Point<S, T> point, Offset<S, T> offset) noexcept
{
	return { point.X + offset.X, point.Y + offset.Y };
}

template<SpaceTag S, typename T>
[[nodiscard]] constexpr Point<S, T> operator-(Point<S, T> point, Offset<S, T> offset) noexcept
{
	return { point.X - offset.X, point.Y - offset.Y };
}

// The integer grid, where a space has one. Named rather than spelled inline at every use, because
// the scissor rectangle, the damage rectangle, the buffer's own extents, and a subsurface's
// position are all this and there is no reason for four spellings.
//
// A grid value and a real value in the same space are different types, deliberately. Getting from
// one to the other is a rounding, rounding is a function of (node, output, frame), and the result
// dies with the frame that computed it — so the conversion is somewhere a reader can find rather
// than an implicit narrowing anywhere the two meet.
template<SpaceTag S>
using PixelPoint = Point<S, std::int32_t>;

template<SpaceTag S>
using PixelOffset = Offset<S, std::int32_t>;

template<SpaceTag S>
using PixelSize = Size<S, std::int32_t>;

template<SpaceTag S>
using PixelRect = Rect<S, std::int32_t>;

// Prints as global(12.5, 40) and device[0, 0 640x480]. The space is in the output because the whole
// point of the type is that the same three numbers mean different things in different ones, and a
// log that elided it would be the one place that distinction is invisible.
//
// The context is a template parameter for the reason recorded at length in Core/Handle.h: naming
// std::format_context leaves std::format working and std::formattable false, so a value goes missing
// from a test failure at exactly the moment it was wanted.
namespace Detail
{
template<typename Value>
struct SpaceFormatter
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }
};
} // namespace Detail

template<SpaceTag S, typename T>
struct std::formatter<Point<S, T>> : Detail::SpaceFormatter<Point<S, T>>
{
	template<typename Context>
	auto format(Point<S, T> point, Context& context) const
	{
		return std::format_to(context.out(), "{}({}, {})", S::Name, point.X, point.Y);
	}
};

template<SpaceTag S, typename T>
struct std::formatter<Offset<S, T>> : Detail::SpaceFormatter<Offset<S, T>>
{
	template<typename Context>
	auto format(Offset<S, T> offset, Context& context) const
	{
		// The sign is the spec's rather than a literal +, so a negative displacement prints as -1
		// instead of +-1. A displacement is signed and reads as one.
		return std::format_to(context.out(), "{}({:+}, {:+})", S::Name, offset.X, offset.Y);
	}
};

template<SpaceTag S, typename T>
struct std::formatter<Size<S, T>> : Detail::SpaceFormatter<Size<S, T>>
{
	template<typename Context>
	auto format(Size<S, T> size, Context& context) const
	{
		return std::format_to(context.out(), "{}({}x{})", S::Name, size.Width, size.Height);
	}
};

template<SpaceTag S, typename T>
struct std::formatter<Rect<S, T>> : Detail::SpaceFormatter<Rect<S, T>>
{
	template<typename Context>
	auto format(Rect<S, T> rect, Context& context) const
	{
		return std::format_to(
			context.out(),
			"{}[{}, {} {}x{}]",
			S::Name,
			rect.Origin.X,
			rect.Origin.Y,
			rect.Extent.Width,
			rect.Extent.Height
		);
	}
};

// The contract everything downstream assumes. The runtime half waits on the test harness.
static_assert(SpaceTag<BufferSpace> && SpaceTag<SurfaceSpace> && SpaceTag<GlobalSpace> && SpaceTag<DeviceSpace>);

// The rule this file exists for. Quantization belongs to an output, so the world has no grid to be
// quantized onto, and the type system rather than a review comment is what says so.
static_assert(!CoordinateOf<GlobalSpace, std::int32_t>, "No integer flows backwards into the model");
static_assert(CoordinateOf<DeviceSpace, std::int32_t> && CoordinateOf<BufferSpace, std::int32_t>);
static_assert(CoordinateOf<SurfaceSpace, std::int32_t>, "wl_subsurface.set_position is integer surface-local");
static_assert(CoordinateOf<GlobalSpace, double>);

// Precision is per space, and global is the one that needed the width.
static_assert(std::is_same_v<Point<GlobalSpace>::Scalar, double>);
static_assert(std::is_same_v<Point<DeviceSpace>::Scalar, float>);
static_assert(std::is_same_v<Point<SurfaceSpace>::Scalar, float>);

// The published representation. Aggregates, no padding, reconstitutable from bytes at an offset.
static_assert(std::is_trivially_copyable_v<Point<GlobalSpace>> && std::is_standard_layout_v<Point<GlobalSpace>>);
static_assert(std::is_aggregate_v<Rect<DeviceSpace>> && std::is_standard_layout_v<Rect<DeviceSpace>>);
static_assert(sizeof(Point<GlobalSpace>) == 2 * sizeof(double));
static_assert(sizeof(PixelRect<DeviceSpace>) == 4 * sizeof(std::int32_t));

// Trivially copyable is asserted above and is not the whole claim. The frame side resolves the
// snapshot from a base address that differs from the one it was written at, so it reads fields by
// position rather than through a constructor — which additionally requires that origin precede
// extent and x precede y, and that nothing sits between them. Nothing about the declaration
// guarantees that; this is where it is guaranteed.
//
// bit_cast rather than memcpy because the default member initializers make these types non-trivial
// even though they stay trivially copyable, which is exactly the distinction -Wclass-memaccess
// exists to flag. Core/Handle.h has the same shape for the same reason.
static_assert(
	std::bit_cast<std::array<std::int32_t, 4>>(PixelRect<DeviceSpace>{ { 3, 5 }, { 640, 480 } }) ==
	std::array<std::int32_t, 4>{ 3, 5, 640, 480 }
);
static_assert(
	std::bit_cast<PixelRect<DeviceSpace>>(std::array<std::int32_t, 4>{ 3, 5, 640, 480 }) ==
	PixelRect<DeviceSpace>{ { 3, 5 }, { 640, 480 } }
);

// Two spaces are two types, and so are the real and grid forms of one space. Neither converts to
// the other by accident; both are reached by a named conversion or not at all.
static_assert(!std::is_convertible_v<Point<GlobalSpace>, Point<DeviceSpace>>);
static_assert(!std::is_convertible_v<Point<DeviceSpace>, PixelPoint<DeviceSpace>>);
static_assert(!std::is_convertible_v<Point<DeviceSpace>, Offset<DeviceSpace>>, "A position is not a displacement");
static_assert(!std::is_convertible_v<Size<DeviceSpace>, Offset<DeviceSpace>>, "An extent is not a displacement");

namespace Detail
{
// Only to give the assertion below a dependent context: a requires-expression outside a template is
// checked eagerly, so the ill-formed case is a hard error rather than a false result.
template<typename T>
concept PointAddable = requires(T a, T b) { a + b; };
} // namespace Detail

static_assert(!Detail::PointAddable<Point<GlobalSpace>>, "Two positions do not add");
static_assert(Detail::PointAddable<Offset<GlobalSpace>>);
static_assert(std::is_same_v<decltype(Point<GlobalSpace>{} - Point<GlobalSpace>{}), Offset<GlobalSpace>>);
static_assert(std::is_same_v<decltype(Point<GlobalSpace>{} + Offset<GlobalSpace>{}), Point<GlobalSpace>>);

static_assert(
	Point<GlobalSpace>{ 1.0, 2.0 } + (Point<GlobalSpace>{ 4.0, 6.0 } - Point<GlobalSpace>{ 1.0, 2.0 }) ==
	Point<GlobalSpace>{ 4.0, 6.0 }
);

// Edges in, extent out. The origin is kept and the extent is derived, which is the only ordering
// that makes two adjacent rects share a number rather than each round to one of their own.
static_assert(
	Rect<DeviceSpace>::FromEdges({ 2.0F, 3.0F }, { 10.0F, 8.0F }) == Rect<DeviceSpace>{ { 2.0F, 3.0F }, { 8.0F, 5.0F } }
);
static_assert(Rect<DeviceSpace>::FromEdges({ 2.0F, 3.0F }, { 10.0F, 8.0F }).Right() == 10.0F);

static_assert(PixelSize<DeviceSpace>{ 0, 4 }.IsEmpty() && PixelSize<DeviceSpace>{ 0, 4 }.IsValid());
static_assert(!PixelSize<DeviceSpace>{ -1, 4 }.IsValid());

static_assert(std::formattable<Point<GlobalSpace>, char>, "A report prints the point rather than <unprintable>");
static_assert(std::formattable<PixelRect<DeviceSpace>, char>);
static_assert(std::formattable<Offset<SurfaceSpace>, char>);
static_assert(std::formattable<Size<BufferSpace>, char>);
