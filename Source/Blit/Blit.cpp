#include "Blit/Blit.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <variant>

#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Region.h"
#include "Seam/Pixel.h"
#include "World/Material.h"

namespace
{
// A float in `[0, 1]` as the composite's own units, rounded to nearest. Saturating rather than
// wrapping, because a coefficient that overshot its target by a rounding step is a real thing an
// animation produces and a wrapped one is a black pixel in the middle of a white logo.
[[nodiscard]] std::uint16_t Quantize(float value) noexcept
{
	return static_cast<std::uint16_t>(std::clamp(value, 0.0F, 1.0F) * 65535.0F + 0.5F);
}

// The rectangle an axis-aligned quad is, or nothing.
//
// **Exact float equality, and it is the right comparison rather than a tolerance.** The corners are
// `NodeTransform::Apply`'s output; for a chain that is a translation and a scale, the two top corners
// are computed from the same local `y` through the same arithmetic and come out bit-identical. A
// chain that rotated by a millionth of a degree did not, and refusing it is the honest answer — a
// tolerance here would silently draw a rotated logo as an unrotated one and be invisible until
// somebody measured the picture.
[[nodiscard]] bool AxisAligned(const Quad& quad, Rect<DeviceSpace>& into) noexcept
{
	for (const float weight : quad.Weights)
	{
		if (weight != 1.0F)
		{
			return false;
		}
	}

	const bool aligned = quad.Corners[0].Y == quad.Corners[1].Y && quad.Corners[2].Y == quad.Corners[3].Y &&
	                     quad.Corners[0].X == quad.Corners[3].X && quad.Corners[1].X == quad.Corners[2].X;

	if (!aligned)
	{
		return false;
	}

	into = Rect<DeviceSpace>::FromEdges(
		{ quad.Corners[0].X, quad.Corners[0].Y }, { quad.Corners[2].X, quad.Corners[2].Y }
	);

	return true;
}

// The item's colour, linearized and premultiplied — once per item, where an exact conversion costs
// nothing measurable.
//
// **Both alpha modes go through one path, and the unpremultiply is why.** A premultiplied value in a
// non-linear encoding is `encode(colour) * alpha`, so linearizing it means undoing the multiply
// first; a straight one is already `encode(colour)`. Doing the divide and the multiply around one
// conversion is the same three lines for both, and skipping the divide for the straight case is the
// only difference between them.
[[nodiscard]] Light Premultiplied(const DrawSolid& solid, ColorState state) noexcept
{
	const float alpha = std::clamp(solid.Alpha, 0.0F, 1.0F);

	float red = solid.Red;
	float green = solid.Green;
	float blue = solid.Blue;

	if (state.Alpha == AlphaMode::Premultiplied && alpha > 0.0F)
	{
		red /= alpha;
		green /= alpha;
		blue /= alpha;
	}

	if (state.Transfer == TransferFunction::Srgb)
	{
		red = SrgbToLinear(red);
		green = SrgbToLinear(green);
		blue = SrgbToLinear(blue);
	}

	return { Quantize(red * alpha), Quantize(green * alpha), Quantize(blue * alpha), Quantize(alpha) };
}

// How much of the unit interval starting at `at` the interval `[from, to)` covers. The whole of the
// antialiasing: an axis-aligned rectangle's coverage of a pixel is the product of this along each
// axis, exactly, with no sampling and no kernel.
[[nodiscard]] float Overlap(float from, float to, std::int32_t at) noexcept
{
	const float low = static_cast<float>(at);

	return std::max(0.0F, std::min(to, low + 1.0F) - std::max(from, low));
}
} // namespace

// What an item contributes, or nothing at all. `EINVAL` is the caller's answer for an item this
// renderer cannot express; a `Draws` of false is an item that is legal and covers no pixels, which
// draws nothing rather than being refused.
Result<Blit::Painted> Blit::Classify(const DrawItem& item) const noexcept
{
	if (item.Dress != Material::None)
	{
		return Failure(EINVAL, "no CPU composite draws a material");
	}

	if (item.Lift.Draws())
	{
		return Failure(EINVAL, "no CPU composite draws a shadow");
	}

	// Refused rather than drawn square, and not foreclosed: the coverage arithmetic below is most of
	// what an analytic corner needs. Until the boot scene has one, drawing a rounded panel with sharp
	// corners is the failure mode that reaches a user with nothing in a log about it.
	if (item.Radius != 0.0F)
	{
		return Failure(EINVAL, "no CPU composite rounds a corner yet");
	}

	const DrawSolid* const solid = std::get_if<DrawSolid>(&item.Content);
	const DrawTexture* const texture = std::get_if<DrawTexture>(&item.Content);
	const DrawGroup* const group = std::get_if<DrawGroup>(&item.Content);

	// A dressing with no material and no elevation has nothing of its own to draw, which is what the
	// three refusals above have already established. It is legal and it is empty.
	if (solid == nullptr && texture == nullptr && group == nullptr)
	{
		return Painted{};
	}

	// Render/Renderer.h's refusal, for its reason: nothing here converts between colour states, so an
	// item whose light differs from the target's is refused rather than written through as though the
	// numbers meant the same thing. Blit/Transfer.h is what this leaves standing — the transfer
	// function, which is a conversion within one state rather than between two. For a texture it is
	// also what makes the decode table below the *output's*, which is the only one this renderer has.
	//
	// **A group is exempt, because the field describes content it does not have.** What a group
	// composites is its members, each of which arrives as an item carrying its own colour state and is
	// checked here in its turn; the flattened result is in the band's units, which are the output's by
	// construction. Refusing a frame over a field nothing reads is a dark machine for no reason at all.
	if (group == nullptr && !(item.Color == m_Output))
	{
		return Failure(EINVAL, "an item's colour state is not the output's, and nothing here converts");
	}

	Painted painted{ .Opacity = std::clamp(item.Opacity, 0.0F, 1.0F), .Draws = true };

	if (group != nullptr)
	{
		// Recorded before any answer below, because the walk skips this group's run by this count
		// whether or not the group itself draws — the members belong to it either way, and a group that
		// collapsed to nothing takes its subtree with it rather than spilling it onto the output.
		painted.Grouped = true;
		painted.Members = group->Count;
	}

	if (!AxisAligned(item.Shape, painted.Shape))
	{
		return Failure(EINVAL, "no CPU composite rasterizes a rotated or projective quad");
	}

	// Empty or back-facing. Not a refusal: a quad that collapsed is a scene that animated something to
	// nothing, and a frame is not where that is reported. Returned before the map below, which would
	// otherwise divide by the extent that just came out zero.
	painted.Draws = painted.Shape.Right() > painted.Shape.Left() && painted.Shape.Bottom() > painted.Shape.Top();

	if (!painted.Draws)
	{
		return painted;
	}

	if (group != nullptr)
	{
		// A group with nothing in it draws nothing rather than being refused, which is Seam/Renderer.h's
		// answer: it is a caller's bug, and a frame is not where one gets reported.
		painted.Draws = group->Count > 0;

		// **The placement is whole device pixels, so the quad is rounded outward rather than covered
		// fractionally.** A group's offscreen is a grid of pixels and its bound is the union of its
		// members' — generally fractional — so the surface has to round out to contain the antialiased
		// edges the members already carry. Applying coverage to the group's own boundary on top of that
		// would attenuate those edges a second time, which on screen is a faint dark seam around every
		// subtree that fades: exactly the artefact decision 47's linear-light blend exists to remove,
		// arriving one group at a time. Rounding outward instead costs nothing, because the pixels it
		// adds are pixels no member wrote and `Over` of nothing is exactly what was beneath.
		painted.Shape = Rect<DeviceSpace>::FromEdges(
			{ std::floor(painted.Shape.Left()), std::floor(painted.Shape.Top()) },
			{ std::ceil(painted.Shape.Right()), std::ceil(painted.Shape.Bottom()) }
		);

		return painted;
	}

	if (solid != nullptr)
	{
		painted.Colour = Premultiplied(*solid, item.Color);

		return painted;
	}

	// A null or stale id draws nothing and says nothing. Core/Texture.h is explicit that a frame is
	// not where a lifetime bug gets reported — the frame after it would report the same one again —
	// and this is the one thing in this file that is silent rather than refused.
	const Image* const image = Find(texture->Texture);

	if (image == nullptr)
	{
		painted.Draws = false;

		return painted;
	}

	const Rect<BufferSpace> whole{ {},
		                           { static_cast<float>(image->Size.Width), static_cast<float>(image->Size.Height) } };

	// Empty means the whole image, which is Seam/Renderer.h's convention and World/Content.h's.
	const Rect<BufferSpace> source = texture->Source.Extent.IsEmpty() ? whole : texture->Source;

	// Refused rather than clamped. A rectangle outside the image is the scene and the image
	// disagreeing about what was adopted, which is gyro's own code on both ends — clamping would draw
	// a picture that is wrong in a way nothing reports, and this renderer's one safety argument is
	// that everything reaching it is gyro's own and a refusal is caught by a test.
	if (source.Left() < 0.0F || source.Top() < 0.0F || source.Right() > whole.Right() ||
	    source.Bottom() > whole.Bottom())
	{
		return Failure(EINVAL, "a sampled rectangle that is not inside the image it names");
	}

	const DecodeTable& table = m_Decode[Depth(image->Bits)];

	Sampled& sampled = painted.Source;

	sampled.Pixels = image->Pixels;
	sampled.Decode = table.Entries();
	sampled.Stride = image->Stride;
	sampled.Code = image->Code;
	sampled.Shift = table.Shift();
	sampled.Proportional = table.IsProportional();
	sampled.Straight = item.Color.Alpha == AlphaMode::Straight;

	// Decision 56's sharpness path, taken on the producer's word rather than recovered from the
	// floats below — Seam/Renderer.h says why that recovery is a guess and this is not the place to
	// make one.
	sampled.Exact = item.Sampling.IsResampleFree();

	sampled.ScaleX = source.Extent.Width / painted.Shape.Extent.Width;
	sampled.ScaleY = source.Extent.Height / painted.Shape.Extent.Height;
	sampled.OriginX = source.Left() + (0.5F - painted.Shape.Left()) * sampled.ScaleX - 0.5F;
	sampled.OriginY = source.Top() + (0.5F - painted.Shape.Top()) * sampled.ScaleY - 0.5F;

	// The texels the filter is allowed to reach: the sampled rectangle rounded outward to whole
	// texels, and never outside the image.
	sampled.MinX = std::clamp(static_cast<std::int32_t>(std::floor(source.Left())), 0, image->Size.Width - 1);
	sampled.MinY = std::clamp(static_cast<std::int32_t>(std::floor(source.Top())), 0, image->Size.Height - 1);
	sampled.MaxX =
		std::clamp(static_cast<std::int32_t>(std::ceil(source.Right())) - 1, sampled.MinX, image->Size.Width - 1);
	sampled.MaxY =
		std::clamp(static_cast<std::int32_t>(std::ceil(source.Bottom())) - 1, sampled.MinY, image->Size.Height - 1);

	painted.Textured = true;

	return painted;
}

// The run structure, before the first pixel and once for the whole list. `Classify` cannot do this
// because one item cannot see where another one's run ends, and the band loop must not: it walks the
// list once per band, and a list that is malformed is malformed the first time.
Result<void> Blit::Nesting(std::span<const DrawItem> items) const noexcept
{
	// Where each open group's run ends, innermost last. Bounded by `MaxDepth` because a group that
	// would push past it is exactly the refusal this walk exists to produce.
	std::array<std::size_t, MaxDepth> ends{};
	std::size_t open = 0;

	for (std::size_t index = 0; index < items.size(); ++index)
	{
		// Runs that ended before this item. A loop rather than an `if` because several can end at once
		// — a group whose last member is itself a group closes both.
		while (open > 0 && ends[open - 1] == index)
		{
			--open;
		}

		const DrawGroup* const group = std::get_if<DrawGroup>(&items[index].Content);

		if (group == nullptr)
		{
			continue;
		}

		// The end of the run this group is inside, which is the list itself at the top level. A run
		// that reaches past its enclosing one is a list whose structure does not nest, and there is no
		// reading of it that draws the right picture — the members past the boundary belong to two
		// groups at once.
		const std::size_t enclosing = open > 0 ? ends[open - 1] : items.size();

		if (index + 1 + static_cast<std::size_t>(group->Count) > enclosing)
		{
			return Failure(EINVAL, "a group's run reaches past the list it is inside");
		}

		if (open == MaxDepth)
		{
			return Failure(EINVAL, "a group nested deeper than this renderer reserves scratch for");
		}

		ends[open++] = index + 1 + static_cast<std::size_t>(group->Count);
	}

	return {};
}

const Blit::Image* Blit::Find(TextureId id) const noexcept
{
	if (id.IsNull())
	{
		return nullptr;
	}

	for (std::size_t index = 0; index < m_Held; ++index)
	{
		// The whole handle, generation included. An index alone would resolve a stale id to whatever
		// took the slot, which is the failure Core/Handle.h exists to make impossible.
		if (m_Images[index].Id == id)
		{
			return &m_Images[index];
		}
	}

	return nullptr;
}

Result<void> Blit::Adopt(TextureId id, const TextureSource& source)
{
	if (id.IsNull())
	{
		return Failure(EINVAL, "a null id names no image");
	}

	// Refused before anything is read out of the variant, because a source describing an image with no
	// extent is one whose stride arithmetic below has nothing to check against.
	if (!source.IsValid())
	{
		return Failure(EINVAL, "an image with no pixels or no extent");
	}

	// Seam/Importer.h's discriminated memory, at the end that has no device. A dmabuf is not a mapping
	// this can read rows out of, and there is nothing here to import it onto — so it is the composition
	// root wiring a client's dmabuf pool at a renderer that cannot take one, which is a branch somebody
	// wrote rather than a cast that happens to work.
	const MappedPixels* const pixels = source.AsMapped();

	if (pixels == nullptr)
	{
		return Failure(EINVAL, "a CPU sampler has no device to import a dmabuf onto");
	}

	if (DecodableBytesPerPixel(source.Format.Code) != 4)
	{
		return Failure(EINVAL, "no CPU composite samples this pixel format");
	}

	// A tiled source is a source whose rows are not rows, exactly as a tiled target is — see
	// `BindTargets`, which refuses it for the same reason at the other end of the composite.
	if (source.Format.Modifier != ModifierLinear && source.Format.Modifier != ModifierInvalid)
	{
		return Failure(EINVAL, "a CPU sampler reads rows, so the layout has to be linear");
	}

	const std::size_t row = static_cast<std::size_t>(source.Size.Width) * 4;
	const std::size_t needed =
		static_cast<std::size_t>(pixels->Stride) * static_cast<std::size_t>(source.Size.Height - 1) + row;

	if (pixels->Stride < row || pixels->Length < needed)
	{
		return Failure(EINVAL, "the allocation is shorter than the image it describes");
	}

	const Image held{ .Id = id,
		              .Pixels = pixels->Pixels,
		              .Stride = pixels->Stride,
		              .Code = source.Format.Code,
		              .Bits = BitsPerChannel(source.Format.Code),
		              .Size = source.Size };

	// Re-adopting a live id replaces what it names rather than taking a second slot: a console that
	// re-lays its grid across a mode change has the same grid at a new address, and the scene that
	// names it did not change. Seam/Importer.h has that under the same watermark rule as `Forget`,
	// because it stops the id naming what a frame in flight may still be reading.
	for (std::size_t index = 0; index < m_Held; ++index)
	{
		if (m_Images[index].Id == id)
		{
			m_Images[index] = held;

			return {};
		}
	}

	if (m_Held == MaxImages)
	{
		return Failure(EINVAL, "more images than this renderer holds");
	}

	m_Images[m_Held++] = held;

	return {};
}

void Blit::Forget(TextureId id) noexcept
{
	for (std::size_t index = 0; index < m_Held; ++index)
	{
		if (m_Images[index].Id == id)
		{
			m_Images[index] = m_Images[--m_Held];
			m_Images[m_Held] = Image{};

			return;
		}
	}
}

Result<void> Blit::BindTargets(std::span<const RenderTarget> targets, ColorState output)
{
	ReleaseTargets();

	if (targets.size() > MaxTargets)
	{
		return Failure(EINVAL, "more targets than this renderer holds");
	}

	if (const Result<void> built = m_Transfer.Build(output.Transfer); !built)
	{
		return std::unexpected{ built.error() };
	}

	// The decode direction, one table per source depth — see Blit/Transfer.h. Built here rather than
	// at an adoption because the transfer function they tabulate is the *output's*: an item's colour
	// state has to equal it to be drawn at all, so there is one curve in a composite and this is the
	// call that learns which.
	for (const std::uint32_t bits : { 8U, 10U })
	{
		if (const Result<void> decode = m_Decode[Depth(bits)].Build(output.Transfer, bits); !decode)
		{
			return std::unexpected{ decode.error() };
		}
	}

	std::int32_t widest = 0;

	for (const RenderTarget& target : targets)
	{
		const MappedImage* mapped = target.AsMapped();

		if (mapped == nullptr)
		{
			ReleaseTargets();

			return Failure(EINVAL, "a CPU renderer needs a mapped target");
		}

		if (DecodableBytesPerPixel(target.Format.Code) != 4)
		{
			ReleaseTargets();

			return Failure(EINVAL, "no CPU composite encodes this pixel format");
		}

		// A tiled mapping is a mapping whose rows are not rows. `ModifierInvalid` is the legacy
		// allocation that says nothing about its layout, and a dumb buffer's is linear — so the one
		// that has to be refused is the one that names a real tiling.
		if (target.Format.Modifier != ModifierLinear && target.Format.Modifier != ModifierInvalid)
		{
			ReleaseTargets();

			return Failure(EINVAL, "a CPU renderer writes rows, so the layout has to be linear");
		}

		if (!target.IsValid())
		{
			ReleaseTargets();

			return Failure(EINVAL, "a target with no extent, no format, or no mapping");
		}

		const std::size_t row = static_cast<std::size_t>(target.Size.Width) * 4;
		const std::size_t needed =
			static_cast<std::size_t>(mapped->Stride) * static_cast<std::size_t>(target.Size.Height - 1) + row;

		if (mapped->Stride < row || mapped->Length < needed)
		{
			ReleaseTargets();

			return Failure(EINVAL, "the mapping is shorter than the image it describes");
		}

		m_Targets[m_Count++] = Bound{ .Pixels = mapped->Pixels,
			                          .Length = mapped->Length,
			                          .Stride = mapped->Stride,
			                          .Size = target.Size,
			                          .Format = target.Format };

		widest = std::max(widest, target.Size.Width);
	}

	// One set of bands for the whole target set, sized to the widest of them. They hold nothing
	// between frames — see Blit/Band.h — so there is no reason for them to be per target, and every
	// reason for them not to be.
	//
	// **Every nesting level is reserved here whether or not a frame nests**, because `Record` may not
	// allocate and cannot know how deep a scene goes until it is walking it. `MaxDepth` is where the
	// cost of that is argued.
	if (widest > 0)
	{
		for (Band& level : m_Levels)
		{
			if (const Result<void> reserved = level.Reserve(widest); !reserved)
			{
				ReleaseTargets();

				return std::unexpected{ reserved.error() };
			}
		}

		// One row of sampled pixels, as wide as the widest run a damage rectangle can produce. Sized
		// here for the band's reason and for Core/FrameSection.h's: a frame may not allocate.
		m_Samples.resize(static_cast<std::size_t>(widest));
	}

	m_Output = output;

	return {};
}

void Blit::ReleaseTargets() noexcept
{
	m_Targets.fill(Bound{});
	m_Count = 0;

	for (Band& level : m_Levels)
	{
		level.Release();
	}

	m_Samples.clear();
	m_Samples.shrink_to_fit();

	// The adopted images are deliberately kept. An image outlives the target set it was drawn into —
	// a mode change rebinds targets and the logo on them is the same logo — and dropping them here
	// would make a reconfiguration a silent loss of every texture in the scene.
}

Result<Submission> Blit::Record(const RecordRequest& request)
{
	const Instant started = m_Clock->Now();

	if (request.Target >= m_Count)
	{
		return Failure(EINVAL, "recording into a target that is not bound");
	}

	if (request.Items.size() > MaxItems)
	{
		return Failure(EINVAL, "a draw list longer than any evaluator produces");
	}

	// The structure before the items, because `Classify` is written against a single item and the
	// walk below trusts what it says about a group's run.
	if (const Result<void> nesting = Nesting(request.Items); !nesting)
	{
		return std::unexpected{ nesting.error() };
	}

	// The whole list before the first pixel, and the answers kept. A refusal halfway through would
	// leave a target holding part of one frame and part of another, and the caller would present it.
	for (std::size_t index = 0; index < request.Items.size(); ++index)
	{
		Result<Painted> painted = Classify(request.Items[index]);

		if (!painted)
		{
			return std::unexpected{ painted.error() };
		}

		m_Painted[index] = *painted;
	}

	const Bound& target = m_Targets[request.Target];

	for (const PixelRect<DeviceSpace>& rect : request.Damage.Rects())
	{
		Paint(target, rect, request.Items.size());
	}

	return Submission{ .Point = SyncPoint::Immediate(), .RecordCost = Elapsed(started, m_Clock->Now()) };
}

Light Blit::Fetch(const Sampled& source, std::int32_t x, std::int32_t y) noexcept
{
	// Clamped to the sampled rectangle, which is a filter tap's whole boundary policy. Doing it here
	// rather than at the caller is what keeps the four taps of a corner texel from each needing their
	// own test.
	x = std::clamp(x, source.MinX, source.MaxX);
	y = std::clamp(y, source.MinY, source.MaxY);

	const Rgba16 texel = DecodePixel(
		LoadWord(
			source.Pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(source.Stride) +
			static_cast<std::size_t>(x) * 4
		),
		source.Code
	);

	const std::uint16_t alpha = texel.Alpha;

	// Nothing there. Also the answer for a malformed premultiplied texel whose components outlived its
	// alpha, which would otherwise come back through the table as light that is not underneath
	// anything.
	if (alpha == 0)
	{
		return {};
	}

	const std::uint16_t red = source.Decode[texel.Red >> source.Shift];
	const std::uint16_t green = source.Decode[texel.Green >> source.Shift];
	const std::uint16_t blue = source.Decode[texel.Blue >> source.Shift];

	// Straight alpha: the components are the colour, so they convert where they stand and the
	// multiply happens afterwards, in linear light, which is where premultiplication is defined.
	if (source.Straight)
	{
		return Attenuate(Light{ red, green, blue, 65535 }, alpha);
	}

	// Premultiplied, and either already proportional to light or fully opaque — both of which make the
	// stored value its own straight value, so the table answers directly. This is the branch a boot
	// screen takes: the firmware logo and the console grid are opaque images.
	if (source.Proportional || alpha == 65535)
	{
		return { red, green, blue, alpha };
	}

	// **Premultiplied in a non-linear encoding, which is Core/ColorState.h's sharp edge arriving one
	// texel at a time.** The alpha was applied in the wrong space, so it has to come off before the
	// curve and go back on after, and the undone value is no longer a source code — it is off the
	// table and costs a `std::pow` per channel. Correct rather than close: leaving it on the table
	// would darken every partially transparent texel by the same amount a blend in the target's
	// encoding darkens an edge, which is the artefact this whole module exists to remove.
	//
	// `Srgb` is the only curve that reaches here, because `Linear` took the branch above and the
	// absolute two are refused at the binding.
	const float scale = static_cast<float>(alpha) / 65535.0F;

	const auto convert = [scale](std::uint16_t component) noexcept {
		return Quantize(SrgbToLinear(static_cast<float>(component) / 65535.0F / scale) * scale);
	};

	return { convert(texel.Red), convert(texel.Green), convert(texel.Blue), alpha };
}

// **The taps are averaged in linear light**, which is the same argument as the antialiased edge one
// file over and is why the decode happens per texel rather than after the filter: a half-and-half of
// white and black texels is half the light, and averaging the encodings would put a dark seam through
// every scaled logo's edge.
Light Blit::Tap(const Sampled& source, float u, float v) noexcept
{
	const float column = std::floor(u);
	const float row = std::floor(v);

	const std::int32_t left = static_cast<std::int32_t>(column);
	const std::int32_t top = static_cast<std::int32_t>(row);

	const std::uint16_t across = Quantize(u - column);
	const std::uint16_t down = Quantize(v - row);

	const Light upper = Mix(Fetch(source, left, top), Fetch(source, left + 1, top), across);
	const Light lower = Mix(Fetch(source, left, top + 1), Fetch(source, left + 1, top + 1), across);

	return Mix(upper, lower, down);
}

float Blit::Ramp(float at, float width) noexcept
{
	// `u` is already the coordinate a tap interpolates on — Blit.h's `Sampled` states that zero is the
	// centre of texel zero — so the lattice a tap blends across is the whole numbers, and there is no
	// half-texel shift here where Render/Shaders/Sample.glsl needs one.
	const float centre = std::floor(at);

	return centre + std::clamp((at - centre - 0.5F) / width + 0.5F, 0.0F, 1.0F);
}

// **The image averaged over the pixel's footprint, and the floor under that footprint is what keeps
// the firmware handoff intact.** This renderer's one magnifying consumer is the BGRT logo: the
// offsets are in the firmware's mode and gyro draws in its own, so a 1024x768 GOP logo lands on a 4K
// panel scaled up several times. A box narrower than a texel — which is what magnification's true
// footprint is — evaluates to nearest-neighbour, and the logo's curved edges would come back as stair
// steps where the firmware had just drawn them smooth. That is a visible change at the exact frame
// Docs/Architecture.md#from-firmware-to-gyro exists to make invisible. So the footprint never falls
// below one texel, magnification stays exactly the bilinear it has always been, and only minification
// changes.
//
// **Under minification a single tap skips texels outright**, which is what this now fixes and what it
// previously only named. The footprint is split into sub-boxes no wider than a texel, each evaluated
// in closed form by one tap through `Ramp`, and their average is the average over the whole footprint
// because they tile it end to end. `SampleMaximumTaps` per axis is exact out to a fourfold shrink and
// under-filters rather than failing past it.
//
// **The same filter as Render/Shaders/Sample.glsl, deliberately and by the same arithmetic.** Two
// renderers have to produce one picture, and a difference in filter is a visible change in sharpness
// at the moment the handoff from this renderer to that one is supposed to be invisible —
// Render/Textures.cpp argues the same point from the other end.
//
// **The footprint is read off the item rather than measured**, which is the one structural difference
// from the shader: a fragment program has derivatives and this has `Sampled::ScaleX`, which is the
// same number stated instead of estimated.
Light Blit::Filter(const Sampled& source, float u, float v) noexcept
{
	const float widthX = std::max(std::abs(source.ScaleX), 1.0F);
	const float widthY = std::max(std::abs(source.ScaleY), 1.0F);

	const std::int32_t tapsX = std::min(static_cast<std::int32_t>(std::ceil(widthX)), SampleMaximumTaps);
	const std::int32_t tapsY = std::min(static_cast<std::int32_t>(std::ceil(widthY)), SampleMaximumTaps);

	if (tapsX == 1 && tapsY == 1)
	{
		// The whole of magnification and unit scale, where the map below is the identity and the average
		// is over one term. Written out so that the ordinary case is one tap and no arithmetic around it.
		return Tap(source, Ramp(u, widthX), Ramp(v, widthY));
	}

	const float spanX = widthX / static_cast<float>(tapsX);
	const float spanY = widthY / static_cast<float>(tapsY);

	// The first sub-box's centre, half a footprint back from the pixel's and half a sub-box in.
	const float firstX = u - 0.5F * (widthX - spanX);
	const float firstY = v - 0.5F * (widthY - spanY);

	std::uint32_t red = 0;
	std::uint32_t green = 0;
	std::uint32_t blue = 0;
	std::uint32_t alpha = 0;

	for (std::int32_t y = 0; y < tapsY; ++y)
	{
		const float down = Ramp(firstY + static_cast<float>(y) * spanY, spanY);

		for (std::int32_t x = 0; x < tapsX; ++x)
		{
			const Light tap = Tap(source, Ramp(firstX + static_cast<float>(x) * spanX, spanX), down);

			red += tap.Red;
			green += tap.Green;
			blue += tap.Blue;
			alpha += tap.Alpha;
		}
	}

	// Rounded to nearest rather than truncated, because the sub-boxes tile the footprint exactly and a
	// truncation would lose up to a code point per pixel — a whole surface a shade dark, which is the
	// kind of error that survives review by being uniform.
	const std::uint32_t count = static_cast<std::uint32_t>(tapsX * tapsY);
	const auto average = [count](std::uint32_t total) noexcept {
		return static_cast<std::uint16_t>((total + count / 2U) / count);
	};

	return { average(red), average(green), average(blue), average(alpha) };
}

Light Blit::Sample(const Sampled& source, float u, float v) noexcept
{
	if (source.Exact)
	{
		return Fetch(source, static_cast<std::int32_t>(std::lround(u)), static_cast<std::int32_t>(std::lround(v)));
	}

	return Filter(source, u, v);
}

// **The sharp path is a cost decision and not a correctness one, which is worth stating because it
// looks like the opposite.** Where the resample is a no-op the map lands exactly on texel centres, the
// two weights come out zero, and `Mix` is exact at zero — so the filter returns the same texel the
// copy does, bit for bit, and Blit.Test.cpp asserts precisely that. What the branch buys is four
// fetches becoming one, on the console's full-screen grid where that is thirty million decodes a frame
// against eight.
std::span<const Light>
Blit::SampleRow(const Sampled& source, float v, std::int32_t from, std::int32_t to, std::uint16_t scale) noexcept
{
	const std::size_t count = static_cast<std::size_t>(to - from);

	if (source.Exact)
	{
		const std::int32_t row = static_cast<std::int32_t>(std::lround(v));

		for (std::size_t index = 0; index < count; ++index)
		{
			const std::int32_t column = from + static_cast<std::int32_t>(index);

			m_Samples[index] =
				Attenuate(Fetch(source, static_cast<std::int32_t>(std::lround(source.Across(column))), row), scale);
		}

		return { m_Samples.data(), count };
	}

	for (std::size_t index = 0; index < count; ++index)
	{
		const std::int32_t column = from + static_cast<std::int32_t>(index);

		m_Samples[index] = Attenuate(Filter(source, source.Across(column), v), scale);
	}

	return { m_Samples.data(), count };
}

void Blit::Paint(const Bound& target, PixelRect<DeviceSpace> rect, std::size_t items) noexcept
{
	const std::int32_t left = std::max(rect.Left(), 0);
	const std::int32_t right = std::min(rect.Right(), target.Size.Width);
	const std::int32_t top = std::max(rect.Top(), 0);
	const std::int32_t bottom = std::min(rect.Bottom(), target.Size.Height);

	if (right <= left || bottom <= top || m_Levels[0].Rows() == 0)
	{
		return;
	}

	for (std::int32_t bandTop = top; bandTop < bottom; bandTop += m_Levels[0].Rows())
	{
		const std::int32_t rows = std::min(m_Levels[0].Rows(), bottom - bandTop);

		m_Levels[0].Clear(rows, left, right);

		Compose(0, 0, items, rows, bandTop, left, right);

		Emit(target, bandTop, rows, left, right);
	}
}

void Blit::Compose(
	std::size_t level,
	std::size_t from,
	std::size_t to,
	std::int32_t rows,
	std::int32_t bandTop,
	std::int32_t left,
	std::int32_t right
) noexcept
{
	// Painted in list order, which is decision 55's z: preorder is the painter's order, so there is
	// nothing to sort and no depth to compare. A group is the one place that order has structure, and
	// even there the members stay in list order — they are just composited somewhere else first.
	for (std::size_t index = from; index < to; ++index)
	{
		const Painted& painted = m_Painted[index];

		if (!painted.Grouped)
		{
			if (painted.Draws)
			{
				Draw(m_Levels[level], painted, rows, bandTop, left, right);
			}

			continue;
		}

		const std::size_t end = index + 1 + static_cast<std::size_t>(painted.Members);

		// The group's own columns, and the rows of this band it reaches. Both edges are whole numbers
		// — `Classify` rounded the placement out to pixels — so the cast is exact and there is no
		// partially covered column to carry.
		const std::int32_t spanFrom = std::max(static_cast<std::int32_t>(painted.Shape.Left()), left);
		const std::int32_t spanTo = std::min(static_cast<std::int32_t>(painted.Shape.Right()), right);

		const bool lands = painted.Draws && spanTo > spanFrom && painted.Shape.Bottom() > static_cast<float>(bandTop) &&
		                   painted.Shape.Top() < static_cast<float>(bandTop + rows);

		// Skipped where the group misses this band, which is most bands for most groups: a fading
		// window is a few hundred rows of a panel, and an offscreen is only worth building where it
		// lands. Skipping is *also* what a group that collapsed to nothing does, and it takes its whole
		// subtree with it — a faded-out group is not the same picture as its members drawn loose.
		if (lands)
		{
			// **The level a group composites into is erased rather than cleared**, because what is
			// beneath the group lives one level down and has to survive the blend that brings this one
			// to it. Erased over the group's own columns and read back over the same ones, so the level
			// is written wherever it will be read; the members are handed those columns too, which is
			// the offscreen's extent doing the clipping a real render target would do by being that
			// size.
			m_Levels[level + 1].Erase(rows, spanFrom, spanTo);

			Compose(level + 1, index + 1, end, rows, bandTop, spanFrom, spanTo);

			Flatten(level, painted, rows, bandTop, spanFrom, spanTo);
		}

		// The members belong to the group whether or not it drew them.
		index = end - 1;
	}
}

void Blit::Flatten(
	std::size_t level,
	const Painted& group,
	std::int32_t rows,
	std::int32_t bandTop,
	std::int32_t left,
	std::int32_t right
) noexcept
{
	// Whole rows and no vertical coverage, for the reason `Classify` rounds the placement out: a group
	// is an extent rather than a shape, and antialiasing its boundary would darken edges its members
	// have already antialiased once.
	const std::int32_t first = std::max(static_cast<std::int32_t>(group.Shape.Top()) - bandTop, 0);
	const std::int32_t last = std::min(static_cast<std::int32_t>(group.Shape.Bottom()) - bandTop, rows);

	// One number for the whole surface, which is decision 60 in a line: the members composited against
	// each other at full strength, and the fade applied once to the result.
	const std::uint16_t opacity = Quantize(group.Opacity);

	for (std::int32_t row = first; row < last; ++row)
	{
		m_Levels[level].BlendAbove(row, left, right, m_Levels[level + 1], opacity);
	}
}

void Blit::Draw(
	Band& into,
	const Painted& painted,
	std::int32_t rows,
	std::int32_t bandTop,
	std::int32_t left,
	std::int32_t right
) noexcept
{
	// The columns this item's edges fall in, and the run between them where every pixel is fully
	// covered. A rectangle whose edges land on whole pixels has no partial columns at all, which is
	// decision 67's settled geometry taking the path with no arithmetic in it.
	const std::int32_t runFrom = static_cast<std::int32_t>(std::ceil(painted.Shape.Left()));
	const std::int32_t runTo = std::max(static_cast<std::int32_t>(std::floor(painted.Shape.Right())), runFrom);
	const std::int32_t edgeLeft = static_cast<std::int32_t>(std::floor(painted.Shape.Left()));
	const std::int32_t edgeRight = static_cast<std::int32_t>(std::ceil(painted.Shape.Right())) - 1;

	for (std::int32_t row = 0; row < rows; ++row)
	{
		const float vertical = Overlap(painted.Shape.Top(), painted.Shape.Bottom(), bandTop + row);

		if (vertical <= 0.0F)
		{
			continue;
		}

		// The vertical coverage and the item's own opacity are one number by the time a span sees
		// them, because attenuating a premultiplied colour is the same operation for both.
		const float weight = vertical * painted.Opacity;

		// The texel row this device row's centre lands on. One value for the whole run, because the
		// quad is axis-aligned — which is most of why the refusal in `Classify` is worth having rather
		// than a general rasterizer.
		const float down = painted.Source.Down(bandTop + row);

		// The interior, where a solid is one constant blended across a span. This is what decision 110
		// replaced the sketch's fill-per-item-per-damage-rectangle with, and the reason is overdraw: a
		// CPU composite at a panel's resolution cannot afford to touch a pixel once per item in the
		// list. A texture resamples the same span into the scratch first and blends it the same way.
		const std::int32_t spanFrom = std::max(runFrom, left);
		const std::int32_t spanTo = std::min(runTo, right);

		if (spanTo > spanFrom)
		{
			if (painted.Textured)
			{
				into.BlendRun(
					row, spanFrom, spanTo, SampleRow(painted.Source, down, spanFrom, spanTo, Quantize(weight))
				);
			}
			else
			{
				into.BlendRun(row, spanFrom, spanTo, Attenuate(painted.Colour, Quantize(weight)));
			}
		}

		// The one or two partial columns, which carry the subpixel placement. A logo scaled to a panel
		// lands between pixels, and this is the whole of what keeps its edge from stepping as the scale
		// animates.
		if (edgeLeft < runFrom && edgeLeft >= left && edgeLeft < right)
		{
			const float horizontal = Overlap(painted.Shape.Left(), painted.Shape.Right(), edgeLeft);
			const std::uint16_t coverage = Quantize(horizontal * weight);

			into.BlendPixel(
				row,
				edgeLeft,
				painted.Textured ? Attenuate(Sample(painted.Source, painted.Source.Across(edgeLeft), down), coverage) :
								   Attenuate(painted.Colour, coverage)
			);
		}

		if (edgeRight >= runTo && edgeRight >= left && edgeRight < right)
		{
			const float horizontal = Overlap(painted.Shape.Left(), painted.Shape.Right(), edgeRight);
			const std::uint16_t coverage = Quantize(horizontal * weight);

			into.BlendPixel(
				row,
				edgeRight,
				painted.Textured ? Attenuate(Sample(painted.Source, painted.Source.Across(edgeRight), down), coverage) :
								   Attenuate(painted.Colour, coverage)
			);
		}
	}
}

void Blit::Emit(
	const Bound& target,
	std::int32_t top,
	std::int32_t rows,
	std::int32_t left,
	std::int32_t right
) const noexcept
{
	for (std::int32_t row = 0; row < rows; ++row)
	{
		const std::span<const Light> source = m_Levels[0].Row(row);

		if (source.empty())
		{
			continue;
		}

		std::byte* at = target.Pixels + static_cast<std::size_t>(top + row) * static_cast<std::size_t>(target.Stride) +
		                static_cast<std::size_t>(left) * 4;

		for (std::int32_t column = left; column < right; ++column)
		{
			const Light light = source[static_cast<std::size_t>(column)];

			// No unpremultiply. The band's bottom is an opaque clear and `Over` keeps an opaque
			// backdrop exactly opaque — Blit/Band.h asserts both — so a composited alpha is always full
			// range and the premultiplied value is the straight one.
			//
			// **A group's level is the one surface where that stops holding, and it costs nothing**,
			// which is not what this comment used to predict. It anticipated a divide belonging to that
			// surface; there is none, because that surface is never encoded. A group's level is erased
			// rather than cleared, so it does hold genuinely translucent light — and then it is consumed
			// by `Band::BlendAbove` into the level beneath it, where `Attenuate` and `Over` both want
			// the premultiplied value and the straight one is never asked for. Only level zero reaches
			// this loop, and level zero has an opaque bottom.
			const Rgba16 encoded{
				m_Transfer.Encode(light.Red), m_Transfer.Encode(light.Green), m_Transfer.Encode(light.Blue), light.Alpha
			};

			StoreWord(at, EncodePixel(encoded, target.Format.Code));
			at += 4;
		}
	}
}
