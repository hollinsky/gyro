#include "Scene/Cursor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "Animation/Author/Bundle.h"
#include "Core/ColorState.h"
#include "Core/Transfer.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Pointer.h"
#include "World/Content.h"
#include "World/Node.h"

namespace
{
// A point in whatever space the caller is working in. Two doubles rather than `Vector3` or `Point`,
// because everything below is plane geometry with no space attached to it and borrowing a typed one
// would say this arithmetic happens in global or surface coordinates, which it does not: it happens in
// device pixels of one output, offset from a hotspot.
struct Vertex
{
	double X = 0.0;
	double Y = 0.0;
};

// The arrow as a closed polygon, in the design units `ArrowTop` and `ArrowBottom` measure and derived
// from them rather than restated, so the baked glyph and Gym/Pointer.h's staircase are the same shape
// and stay so when the shape is tuned.
//
// The wing is where the two functions stop describing one boundary: `ArrowBottom` steps from the tail's
// corner down to the wing's underside at `0.56`, and a step in a function of `x` is a vertical edge in
// the polygon. So there are two vertices at that `x` rather than one.
constexpr std::size_t ArrowVertices = 7;

[[nodiscard]] std::array<Vertex, ArrowVertices> ArrowPolygon() noexcept
{
	return { Vertex{ 0.0, 0.0 },
		     Vertex{ ArrowWidth, ArrowTop(ArrowWidth) },
		     Vertex{ 0.56, ArrowBottom(ArrowWidth) },
		     Vertex{ 0.56, ArrowBottom(0.56) },
		     Vertex{ 0.40, ArrowBottom(0.40) },
		     Vertex{ 0.24, ArrowBottom(0.24) },
		     Vertex{ 0.0, ArrowBottom(0.0) } };
}

// How far a mitred corner may reach, as a multiple of the outline's width. The arrow's sharpest corner
// is its tip at a shade under forty-six degrees, which mitres to two and a half, so this is a guard
// rather than a shape decision — and it clamps the length rather than inserting a bevel vertex,
// because a bevel changes the vertex count and this one is never reached.
constexpr double MiterLimit = 3.0;

// The most vertices a clipped polygon can carry. Each half-plane emits at most two vertices per input
// edge, and the sweep below clips four times; the arrow never comes close, and this exists so that the
// clip has somewhere to stop rather than to be tight.
constexpr std::size_t MaxClipped = 64;

// A polygon under construction, with a fixed ceiling and a latch. A clip that would overflow drops the
// vertex, which would understate an area — so the latch is read rather than assumed, and the caller
// refuses.
struct Polygon
{
	std::array<Vertex, MaxClipped> Points{};
	std::size_t Count = 0;
	bool Ok = true;

	void Push(Vertex point) noexcept
	{
		if (Count >= MaxClipped)
		{
			Ok = false;

			return;
		}

		Points[Count++] = point;
	}
};

// Twice the signed area, which is the shoelace sum with the halving left off. Sign carries the
// winding, which is what the offset below needs and what the areas do not.
[[nodiscard]] double DoubleSignedArea(std::span<const Vertex> polygon) noexcept
{
	double sum = 0.0;

	for (std::size_t index = 0; index < polygon.size(); ++index)
	{
		const Vertex& here = polygon[index];
		const Vertex& next = polygon[(index + 1) % polygon.size()];

		sum += here.X * next.Y - next.X * here.Y;
	}

	return sum;
}

// The polygon grown outward by `grow` on every side, by mitring each vertex along the bisector of its
// two edge normals. Exact for a simple polygon whose features are wider than twice `grow`, which the
// arrow's are: its thinnest place is the wing, and this grows the silhouette rather than insetting the
// body precisely because insetting would collapse it.
[[nodiscard]] std::array<Vertex, ArrowVertices>
Grown(const std::array<Vertex, ArrowVertices>& polygon, double grow) noexcept
{
	const double sign = DoubleSignedArea(polygon) >= 0.0 ? 1.0 : -1.0;

	const auto normal = [&](std::size_t index) {
		const Vertex& here = polygon[index];
		const Vertex& next = polygon[(index + 1) % ArrowVertices];

		const double dx = next.X - here.X;
		const double dy = next.Y - here.Y;
		const double length = std::hypot(dx, dy);

		return length > 0.0 ? Vertex{ sign * dy / length, -sign * dx / length } : Vertex{};
	};

	std::array<Vertex, ArrowVertices> out{};

	for (std::size_t index = 0; index < ArrowVertices; ++index)
	{
		const Vertex before = normal((index + ArrowVertices - 1) % ArrowVertices);
		const Vertex after = normal(index);

		// The mitre: the bisector scaled so the offset edges still pass through it. Degenerate only
		// where the two edges double back on each other, which a simple polygon's vertex does not.
		const double denominator = 1.0 + before.X * after.X + before.Y * after.Y;

		Vertex miter = denominator > 1e-9 ?
		                   Vertex{ (before.X + after.X) / denominator, (before.Y + after.Y) / denominator } :
		                   after;

		const double reach = std::hypot(miter.X, miter.Y);

		if (reach > MiterLimit)
		{
			miter = { miter.X * MiterLimit / reach, miter.Y * MiterLimit / reach };
		}

		out[index] = { polygon[index].X + grow * miter.X, polygon[index].Y + grow * miter.Y };
	}

	return out;
}

// Sutherland-Hodgman against one edge of an axis-aligned rectangle. Clipping a simple polygon to a
// convex region this way can leave zero-area seams along the boundary where a concave piece was cut in
// two, and that is exactly why it is the right tool here: a seam contributes nothing to the shoelace
// sum, so the area is right even where the outline is not.
void ClipHalfPlane(const Polygon& in, Polygon& out, bool horizontal, double bound, bool keepAbove) noexcept
{
	out.Count = 0;
	out.Ok = in.Ok;

	if (in.Count == 0)
	{
		return;
	}

	const auto coordinate = [&](const Vertex& point) { return horizontal ? point.X : point.Y; };
	const auto inside = [&](const Vertex& point) {
		return keepAbove ? coordinate(point) >= bound : coordinate(point) <= bound;
	};

	for (std::size_t index = 0; index < in.Count; ++index)
	{
		const Vertex& here = in.Points[index];
		const Vertex& next = in.Points[(index + 1) % in.Count];

		const bool hereIn = inside(here);
		const bool nextIn = inside(next);

		if (hereIn)
		{
			out.Push(here);
		}

		if (hereIn == nextIn)
		{
			continue;
		}

		const double from = coordinate(here);
		const double to = coordinate(next);
		const double span = to - from;
		const double t = span == 0.0 ? 0.0 : (bound - from) / span;

		out.Push({ here.X + t * (next.X - here.X), here.Y + t * (next.Y - here.Y) });
	}
}

// The polygon clipped to one axis-aligned slab, which is two half-planes.
void ClipSlab(const Polygon& in, Polygon& out, bool horizontal, double low, double high) noexcept
{
	Polygon scratch{};

	ClipHalfPlane(in, scratch, horizontal, low, true);
	ClipHalfPlane(scratch, out, horizontal, high, false);
}

// The area of a clipped piece, which is a coverage once the clip was a unit pixel. Unsigned, because
// the winding was the offset's business and is nobody's here.
[[nodiscard]] double Area(const Polygon& polygon) noexcept
{
	return std::abs(DoubleSignedArea(std::span{ polygon.Points.data(), polygon.Count })) * 0.5;
}

// Everything a coverage below this would contribute, which is nothing an eight-bit texel can hold.
constexpr double NoAlpha = 1.0 / 512.0;

[[nodiscard]] std::uint32_t Quantize(double value) noexcept
{
	return static_cast<std::uint32_t>(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5);
}

// One texel: the light body's coverage `filled`, inside the whole silhouette's coverage `covered`.
//
// **The alpha is the silhouette and the colour is the body**, which is the composite of white over
// black over nothing done once, here, instead of twice per pixel on the far side of the renderer.
// Drawn `over` a background this leaves `filled·white + (1 − covered)·background`, and the outline is
// the difference between them by construction rather than by an alpha anybody had to solve for.
//
// **The colour is stored encoded and premultiplied, which is not the coverage.** `Blit` decodes a
// texel by unpremultiplying, linearising and remultiplying — decision 47 composites in linear light —
// so what has to be written is the value that decodes back to `filled`. That is
// `alpha · LinearToSrgb(filled / alpha)`, taken against the *quantised* alpha rather than the exact
// one, because the quantised one is what the decode will divide by.
[[nodiscard]] std::uint32_t Texel(double covered, double filled) noexcept
{
	if (covered <= NoAlpha)
	{
		return 0;
	}

	const std::uint32_t alpha = Quantize(covered);

	if (alpha == 0)
	{
		return 0;
	}

	const double quantized = static_cast<double>(alpha) / 255.0;
	const double body = std::clamp(filled / quantized, 0.0, 1.0);

	const std::uint32_t level =
		std::min(alpha, Quantize(quantized * static_cast<double>(LinearToSrgb(static_cast<float>(body)))));

	return (alpha << 24U) | (level << 16U) | (level << 8U) | level;
}
} // namespace

Result<CursorImage> CursorImage::Draw(double height, Scale density)
{
	if (height <= 0.0)
	{
		return Failure(EINVAL, "a glyph is authored at a height, and this one has none");
	}

	// Design units to device pixels in one factor. The glyph's box is `ArrowHeight` design units tall
	// and `height` output units, and an output unit is `density` device pixels.
	const double unit = (height / ArrowHeight) * density.ToDouble();

	std::array<Vertex, ArrowVertices> light = ArrowPolygon();

	for (Vertex& point : light)
	{
		point = { point.X * unit, point.Y * unit };
	}

	// `dark` contains `light` by construction, which is what lets one texel carry both: the body's
	// coverage can never exceed the silhouette's.
	const std::array<Vertex, ArrowVertices> dark = Grown(light, ArrowOutline * ArrowHeight * unit);

	double top = dark.front().Y;
	double bottom = dark.front().Y;
	double left = dark.front().X;
	double right = dark.front().X;

	for (const Vertex& point : dark)
	{
		top = std::min(top, point.Y);
		bottom = std::max(bottom, point.Y);
		left = std::min(left, point.X);
		right = std::max(right, point.X);
	}

	// The image is the silhouette's bound rounded out to whole device pixels, so the hotspot lands on
	// one — which is what `Node::Snap` then keeps it on.
	const std::int32_t originX = static_cast<std::int32_t>(std::floor(left));
	const std::int32_t originY = static_cast<std::int32_t>(std::floor(top));

	const std::int32_t width = static_cast<std::int32_t>(std::ceil(right)) - originX;
	const std::int32_t rows = static_cast<std::int32_t>(std::ceil(bottom)) - originY;

	if (width <= 0 || rows <= 0)
	{
		return Failure(EINVAL, "the glyph's silhouette covers no device pixels at this density");
	}

	// **Padded out to a size a display engine will scan out, with the glyph in the top-left corner.**
	// See `CursorPlaneSizes`: the pointer is the frontmost node, decision 152's promoted set is a suffix,
	// so a glyph a plane refuses costs every layer beneath it too. The padding is right and bottom only,
	// which is what leaves the hotspot where the sweep put it — it is measured from the image's origin
	// and the origin has not moved.
	const PixelSize<BufferSpace> image = CursorImageSize({ width, rows });

	// Against the padded extent rather than the silhouette's, because the padded extent is what is
	// allocated. The two are the same wherever the bound refuses: a glyph large enough to reach it is
	// far past the largest size in the set, so it is not padded at all.
	if (static_cast<std::size_t>(image.Width) * static_cast<std::size_t>(image.Height) > MaxCursorTexels)
	{
		return Failure(
			E2BIG, "the glyph covers more texels than a cursor image will hold, so it is authored too large"
		);
	}

	Polygon outer{};
	Polygon inner{};

	for (const Vertex& point : dark)
	{
		outer.Push(point);
	}

	for (const Vertex& point : light)
	{
		inner.Push(point);
	}

	// Zero is transparent black in this layout, so the padding is written by the allocation and the
	// sweep below never visits it.
	std::vector<std::uint32_t> words(
		static_cast<std::size_t>(image.Width) * static_cast<std::size_t>(image.Height), 0U
	);

	for (std::int32_t row = 0; row < rows; ++row)
	{
		const double y = static_cast<double>(originY + row);

		Polygon outerRow{};
		Polygon innerRow{};

		ClipSlab(outer, outerRow, false, y, y + 1.0);
		ClipSlab(inner, innerRow, false, y, y + 1.0);

		if (!outerRow.Ok || !innerRow.Ok)
		{
			return Failure(EINVAL, "the glyph's polygon clipped to more vertices than the sweep carries");
		}

		for (std::int32_t column = 0; column < width; ++column)
		{
			const double x = static_cast<double>(originX + column);

			Polygon outerPixel{};
			Polygon innerPixel{};

			ClipSlab(outerRow, outerPixel, true, x, x + 1.0);
			ClipSlab(innerRow, innerPixel, true, x, x + 1.0);

			if (!outerPixel.Ok || !innerPixel.Ok)
			{
				return Failure(EINVAL, "the glyph's polygon clipped to more vertices than the sweep carries");
			}

			const double covered = std::clamp(Area(outerPixel), 0.0, 1.0);
			const double filled = std::clamp(Area(innerPixel), 0.0, covered);

			words
				[static_cast<std::size_t>(row) * static_cast<std::size_t>(image.Width) +
			     static_cast<std::size_t>(column)] = Texel(covered, filled);
		}
	}

	return CursorImage{ image, { width, rows }, std::move(words), -originX, -originY, density };
}

std::uint32_t CursorImage::At(std::int32_t x, std::int32_t y) const noexcept
{
	if (x < 0 || y < 0 || x >= m_Size.Width || y >= m_Size.Height)
	{
		return 0;
	}

	return m_Words[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Size.Width) + static_cast<std::size_t>(x)];
}

Result<EntityId> AuthorCursor(SceneStore& scene, EntityId parent, TextureId texture, const CursorImage& image)
{
	if (texture.IsNull())
	{
		return Failure(EINVAL, "a cursor node names the image its pixels were adopted as, and this one names none");
	}

	const double scale = image.Density().ToDouble();

	// Back from device pixels to the output's own units, which is the space a node's extent is in. The
	// texels are then one to one with device pixels, which is what makes `Blit`'s bilinear filter land
	// exactly on texel centres and return the copy — its own comment says so — so the glyph reaches the
	// panel as the bytes that were baked rather than as a resample of them.
	//
	// **The padded extent rather than the glyph's, which is the node being honest about what it draws.**
	// `CursorImageSize` grows the image to a square a display engine will take, so the node is a quad
	// larger than the arrow with transparent texels around it. Sizing it to the silhouette instead would
	// crop the source and hand a plane a rectangle that is not the buffer, which is the one thing that
	// would put the promotion back where it started. What it costs is a few thousand transparent texels
	// blended on the frames the pointer is composited rather than promoted, against the whole screen's
	// composite it exists to avoid.
	const auto units = [&](std::int32_t pixels) { return static_cast<double>(pixels) / scale; };

	// `Node::Snap` for decision 156's reason and one that is particular to a baked glyph: the coverage
	// was computed for the shape sitting exactly on the device grid, so a node at a fractional position
	// would draw one alignment's answer at another's — through a filter, which turns it soft as well as
	// wrong.
	const std::optional<EntityId> node = scene.CreateImage(
		parent,
		{ .Position = { -units(image.HotspotX()), -units(image.HotspotY()), 0.0 },
	      .Extent = { static_cast<float>(units(image.Size().Width)), static_cast<float>(units(image.Size().Height)) },
	      .Flags = Node::Snap },
		// **The source is the whole image and is stated rather than left empty**, which is the one
	    // place the two spellings of *everything* differ. World/Content.h lets empty mean the whole
	    // texture, and the frame walk has no way to turn that back into a texel count — so a node that
	    // says nothing here is a node whose sampling cannot be classified, and the pointer is exactly
	    // the node that must be: it is baked at the panel's density and sized at one over it, so it
	    // reaches the glass texel for texel and is the one thing on screen a display engine can always
	    // scan out for itself. An empty frame stays empty: there is no window geometry to round a
	    // corner against.
		{ .Texture = texture,
	      .Source = { {}, { static_cast<float>(image.Size().Width), static_cast<float>(image.Size().Height) } },
	      .Frame = {},
	      .Color = ColorState::Srgb() }
	);

	if (!node)
	{
		return Failure(ENOSPC, "the entity index space is exhausted, so the cursor has no node");
	}

	return *node;
}

namespace
{
// The half of `Withdraw` that a half-built cursor needs: an entity nothing has drawn yet, put back.
// Written as a commit for the reason `Withdraw` is — retirement is the only door out of the tree an
// author has.
void Discard(SceneStore& scene, EntityId container) noexcept
{
	SceneCommit commit{ scene, CommitAuthor::Compositor };

	static_cast<void>(commit.Retire(container));
}
} // namespace

void SceneCursor::Step(SceneStore& scene, ITextures& textures)
{
	const ScenePointer& pointer = scene.Pointer();
	const std::span<const SceneOutput> outputs = scene.Outputs();

	// No pointer to draw, or nowhere to draw it. The second is not a corner: a world with no outputs
	// is one the dispatch loop refuses to open, and a world whose only monitor was unplugged passes
	// through here before the next set arrives.
	if (!pointer.IsVisible() || outputs.empty())
	{
		Withdraw(scene, textures);

		return;
	}

	// The output the pointer is on, and the first one where it is on none — which happens for exactly
	// as long as it takes `Reconfine` to pull it back onto the union, and is a scale to draw at rather
	// than a claim about where the pointer is.
	Scale density = outputs.front().Density;

	const OutputId on = pointer.On(outputs);

	for (const SceneOutput& output : outputs)
	{
		if (output.Id == on)
		{
			density = output.Density;
		}
	}

	if (density != m_Density)
	{
		// The glyph in hand was baked for another panel's grid. Taken away rather than reused, which is
		// the header's argument: a pointer that keeps one image across a mixed-density desk is one that
		// changes size as it crosses, and does it through a filter.
		Withdraw(scene, textures);

		m_Density = density;
		m_Refused = false;
	}

	if (m_Container.IsNull() && !m_Refused && !Author(scene, textures, density))
	{
		m_Refused = true;
	}

	if (m_Container.IsNull())
	{
		return;
	}

	// **Frontmost every iteration rather than once at the author.** The glyph becomes a root when a
	// device first moves the pointer, and decision 55 makes the last root the frontmost — but a root
	// created *after* that lands in front of it, and a session's floor is created when its agent hands
	// over a listener, which is whenever a person logs in. Moving the mouse before the first window
	// arrives put the cursor behind every window on the machine for the life of the session. `Raise`
	// answers true and touches nothing where the node is already last, which is every iteration but the
	// one after a floor opened, so this is a comparison per pointer motion.
	static_cast<void>(scene.Raise(m_Container));

	const Point<GlobalSpace> at = pointer.Position();

	// **Immediate, and it is the one node in the world for which that is not a shortcut.** The pointer
	// is where a person's hand put it, so a spring between the device and the glyph would be lag by
	// construction — the one place in a compositor where somebody notices a single frame. The position
	// is written unrounded and `Node::Snap` on the glyph is what lands it on the panel's grid, which is
	// decision 52 applied at the moment the cursor is drawn rather than at the moment it is moved.
	//
	// Written every iteration rather than on a change, because `SetImmediate` is a spring at rest at a
	// value and re-stating one is the same coefficients — `Animation/Author/Animatable.h`'s point that
	// an immediate write and *never animated* are one state and not two.
	SceneCommit commit{ scene, CommitAuthor::Compositor };

	static_cast<void>(commit.Move(m_Container, { at.X, at.Y, 0.0 }));
}

bool SceneCursor::Author(SceneStore& scene, ITextures& textures, Scale density)
{
	const Result<CursorImage> image = CursorImage::Draw(CursorHeight, density);

	if (!image)
	{
		return false;
	}

	// The bytes are read during the call and the registry holds them afterwards, so the image is a
	// local: Scene/Textures.h's contract, and the reason nothing here owns a pixel past this line.
	const Result<TextureId> texture =
		textures.Adopt(image->Size(), image->Stride(), image->Bytes(), TextureAlpha::Premultiplied);

	if (!texture)
	{
		return false;
	}

	// A container at the top level and the glyph under it, so that what moves at input rate is one
	// translation on one node and the hotspot offset is stated once. It carries no extent for
	// `Protocol/Floor.h`'s reason: the cursor is where the glyph hangs rather than a rectangle
	// anything draws.
	const std::optional<EntityId> container = scene.CreateContainer(EntityId{}, {});

	if (!container)
	{
		textures.Retire(*texture);

		return false;
	}

	const Result<EntityId> glyph = AuthorCursor(scene, *container, *texture, *image);

	if (!glyph)
	{
		Discard(scene, *container);
		textures.Retire(*texture);

		return false;
	}

	m_Container = *container;
	m_Texture = *texture;
	m_Density = density;

	return true;
}

void SceneCursor::Withdraw(SceneStore& scene, ITextures& textures) noexcept
{
	if (m_Container.IsNull())
	{
		return;
	}

	// **Retired rather than destroyed, and it is gone on this pass anyway.** Decision 114 makes an
	// entity leave the world in two steps — the author says it is over, and the serialiser sweeps the
	// subtree once every channel in it has settled — and there is no other door: the store's `Destroy`
	// is the sweep's. A cursor has no exit to play, so the sweep at the top of the serialisation this
	// step is about to run destroys it before the walk that would have published it, which is the
	// property that entry argues for in the case of a closing window and gets right for free here.
	//
	// The texture goes with it, and the registry is what waits for the watermark: the snapshots already
	// out still name the id, and the frame thread is still reading from them.
	{
		SceneCommit commit{ scene, CommitAuthor::Compositor };

		static_cast<void>(commit.Retire(m_Container));
	}

	textures.Retire(m_Texture);

	m_Container = {};
	m_Texture = {};
}
