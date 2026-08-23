#include "Blit/Blit.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <variant>

#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Region.h"
#include "Seam/Pixel.h"
#include "World/Elevation.h"
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
Result<Blit::Painted> Blit::Classify(const DrawItem& item, ColorState output) noexcept
{
	if (item.Dress != Material::None)
	{
		return Failure(EINVAL, "no CPU composite draws a material");
	}

	if (item.Lift != Elevation::None)
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

	if (std::holds_alternative<DrawTexture>(item.Content))
	{
		return Failure(EINVAL, "no CPU composite samples a texture yet");
	}

	if (std::holds_alternative<DrawGroup>(item.Content))
	{
		return Failure(EINVAL, "no CPU composite flattens a group yet");
	}

	// A dressing with no material and no elevation has nothing of its own to draw, which is what the
	// two refusals above have already established. It is legal and it is empty.
	const DrawSolid* solid = std::get_if<DrawSolid>(&item.Content);

	if (solid == nullptr)
	{
		return Painted{};
	}

	// Render/Renderer.h's refusal, for its reason: nothing here converts between colour states, so an
	// item whose light differs from the target's is refused rather than written through as though the
	// numbers meant the same thing. Blit/Transfer.h is what this leaves standing — the transfer
	// function, which is a conversion within one state rather than between two.
	if (!(item.Color == output))
	{
		return Failure(EINVAL, "an item's colour state is not the output's, and nothing here converts");
	}

	Painted painted{ .Colour = Premultiplied(*solid, item.Color),
		             .Opacity = std::clamp(item.Opacity, 0.0F, 1.0F),
		             .Draws = true };

	if (!AxisAligned(item.Shape, painted.Shape))
	{
		return Failure(EINVAL, "no CPU composite rasterizes a rotated or projective quad");
	}

	// Empty or back-facing. Not a refusal: a quad that collapsed is a scene that animated something to
	// nothing, and a frame is not where that is reported.
	painted.Draws = painted.Shape.Right() > painted.Shape.Left() && painted.Shape.Bottom() > painted.Shape.Top();

	return painted;
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

	// One band for the whole set, sized to the widest of them. It holds nothing between frames — see
	// Blit/Band.h — so there is no reason for it to be per target, and every reason for it not to be.
	if (widest > 0)
	{
		if (const Result<void> reserved = m_Band.Reserve(widest); !reserved)
		{
			ReleaseTargets();

			return std::unexpected{ reserved.error() };
		}
	}

	m_Output = output;

	return {};
}

void Blit::ReleaseTargets() noexcept
{
	m_Targets.fill(Bound{});
	m_Count = 0;
	m_Band.Release();
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

	// The whole list before the first pixel, and the answers kept. A refusal halfway through would
	// leave a target holding part of one frame and part of another, and the caller would present it.
	for (std::size_t index = 0; index < request.Items.size(); ++index)
	{
		Result<Painted> painted = Classify(request.Items[index], m_Output);

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

void Blit::Paint(const Bound& target, PixelRect<DeviceSpace> rect, std::size_t items) noexcept
{
	const std::int32_t left = std::max(rect.Left(), 0);
	const std::int32_t right = std::min(rect.Right(), target.Size.Width);
	const std::int32_t top = std::max(rect.Top(), 0);
	const std::int32_t bottom = std::min(rect.Bottom(), target.Size.Height);

	if (right <= left || bottom <= top || m_Band.Rows() == 0)
	{
		return;
	}

	for (std::int32_t bandTop = top; bandTop < bottom; bandTop += m_Band.Rows())
	{
		const std::int32_t rows = std::min(m_Band.Rows(), bottom - bandTop);

		m_Band.Clear(rows, left, right);

		// Painted in list order, which is decision 55's z: preorder is the painter's order, so there is
		// nothing to sort and no depth to compare.
		for (std::size_t index = 0; index < items; ++index)
		{
			const Painted& painted = m_Painted[index];

			if (!painted.Draws)
			{
				continue;
			}

			// The columns this item's edges fall in, and the run between them where every pixel is
			// fully covered. A rectangle whose edges land on whole pixels has no partial columns at
			// all, which is decision 67's settled geometry taking the path with no arithmetic in it.
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

				// The vertical coverage and the item's own opacity are one number by the time a span
				// sees them, because attenuating a premultiplied colour is the same operation for both.
				const float weight = vertical * painted.Opacity;

				// The interior, where the run is one constant blended across a span. This is what
				// decision 110 replaced the sketch's fill-per-item-per-damage-rectangle with, and the
				// reason is overdraw: a CPU composite at a panel's resolution cannot afford to touch a
				// pixel once per item in the list.
				if (runTo > runFrom)
				{
					m_Band.BlendRun(
						row,
						std::max(runFrom, left),
						std::min(runTo, right),
						Attenuate(painted.Colour, Quantize(weight))
					);
				}

				// The one or two partial columns, which carry the subpixel placement. A logo scaled to
				// a panel lands between pixels, and this is the whole of what keeps its edge from
				// stepping as the scale animates.
				if (edgeLeft < runFrom && edgeLeft >= left && edgeLeft < right)
				{
					const float horizontal = Overlap(painted.Shape.Left(), painted.Shape.Right(), edgeLeft);

					m_Band.BlendPixel(row, edgeLeft, Attenuate(painted.Colour, Quantize(horizontal * weight)));
				}

				if (edgeRight >= runTo && edgeRight >= left && edgeRight < right)
				{
					const float horizontal = Overlap(painted.Shape.Left(), painted.Shape.Right(), edgeRight);

					m_Band.BlendPixel(row, edgeRight, Attenuate(painted.Colour, Quantize(horizontal * weight)));
				}
			}
		}

		Emit(target, bandTop, rows, left, right);
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
		const std::span<const Light> source = m_Band.Row(row);

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
			// range and the premultiplied value is the straight one. A group's offscreen is the first
			// surface where that stops holding, and the divide belongs to that surface rather than to
			// this loop.
			const Rgba16 encoded{
				m_Transfer.Encode(light.Red), m_Transfer.Encode(light.Green), m_Transfer.Encode(light.Blue), light.Alpha
			};

			StoreWord(at, EncodePixel(encoded, target.Format.Code));
			at += 4;
		}
	}
}
