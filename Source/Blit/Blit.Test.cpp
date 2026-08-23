#include "Blit/Blit.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Blit/Transfer.h"
#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Seam/Pixel.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Testing/Test.h"
#include "World/Elevation.h"
#include "World/Material.h"

// The composite, checked by reading the bytes it left.
//
// **Nothing here is a golden image, for Virtual/Pixels.h's reason** — a checked-in reference frame
// asserts every pixel and so fails on the ones nobody meant to promise. What is asserted instead is
// where the edges are, that the interior is exactly the colour asked for, that what was outside the
// damage did not move, and that a coverage value sits between the two things it is between. Those
// are true on any machine, which matters more here than anywhere: this is the renderer that runs
// when there is nothing else to run it against.
//
// It reaches for Seam/Pixel.h to read what it wrote, which is the same codec `Blit` encodes through.
// That is deliberate and it is not circular: the codec's own channel order is asserted against
// hand-laid bytes in Seam/Pixel.Test.cpp, so what is left for this file to check is the composite.

namespace
{
constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };

// A target and the memory under it, in one object, so a test body is about what was drawn.
class Surface
{
public:
	Surface(std::int32_t width, std::int32_t height, PixelFormat format = Linear)
		: m_Bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, std::byte{ 0x5A }),
		  m_Size{ width, height }
	{
		m_Target.Size = m_Size;
		m_Target.Format = format;
		m_Target.Memory = MappedImage{ .Pixels = m_Bytes.data(),
			                           .Stride = static_cast<std::uint32_t>(width) * 4,
			                           .Reserved = 0,
			                           .Length = m_Bytes.size() };
	}

	[[nodiscard]] const RenderTarget& Target() const noexcept { return m_Target; }

	[[nodiscard]] std::span<const RenderTarget> One() const noexcept { return { &m_Target, 1 }; }

	[[nodiscard]] Rgba16 At(std::int32_t x, std::int32_t y) const noexcept
	{
		const std::size_t offset =
			(static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Size.Width) + static_cast<std::size_t>(x)) * 4;

		return DecodePixel(LoadWord(m_Bytes.data() + offset), m_Target.Format.Code);
	}

	// The value every byte starts at, which no composite produces — so *was not written* is a claim
	// the bytes can answer.
	[[nodiscard]] static bool IsUntouched(Rgba16 pixel) noexcept
	{
		return pixel == DecodePixel(0x5A5A5A5AU, Linear.Code);
	}

private:
	std::vector<std::byte> m_Bytes;
	PixelSize<DeviceSpace> m_Size{};
	RenderTarget m_Target{};
};

[[nodiscard]] DrawItem Solid(Rect<DeviceSpace> at, float red, float green, float blue, float alpha = 1.0F)
{
	DrawItem item{};
	item.Content = DrawSolid{ .Red = red, .Green = green, .Blue = blue, .Alpha = alpha };
	item.Shape = Quad::FromRect(at);
	item.Extent = { at.Extent.Width, at.Extent.Height };

	return item;
}

// An image and the bytes under it, in one object — the other end of `Surface`, and written through
// the same codec so that a test says what colour a texel is rather than what word it is.
class Picture
{
public:
	Picture(std::int32_t width, std::int32_t height, PixelFormat format = Linear)
		: m_Bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, std::byte{}),
		  m_Size{ width, height }, m_Format{ format }
	{}

	void Set(std::int32_t x, std::int32_t y, Rgba16 pixel) noexcept
	{
		const std::size_t offset =
			(static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Size.Width) + static_cast<std::size_t>(x)) * 4;

		StoreWord(m_Bytes.data() + offset, EncodePixel(pixel, m_Format.Code));
	}

	[[nodiscard]] SourceImage Image() const noexcept
	{
		return { .Pixels = m_Bytes.data(),
			     .Length = m_Bytes.size(),
			     .Stride = static_cast<std::uint32_t>(m_Size.Width) * 4,
			     .Size = m_Size,
			     .Format = m_Format };
	}

private:
	std::vector<std::byte> m_Bytes;
	PixelSize<BufferSpace> m_Size{};
	PixelFormat m_Format{};
};

// One id, minted the way a dispatch side would mint one: a slot and the generation it carried.
constexpr TextureId Logo{ 3, 2 };

[[nodiscard]] DrawItem Textured(TextureId id, Rect<DeviceSpace> at, Rect<BufferSpace> source = {})
{
	DrawItem item{};
	item.Content = DrawTexture{ .Texture = id, .Source = source };
	item.Shape = Quad::FromRect(at);
	item.Extent = { at.Extent.Width, at.Extent.Height };

	return item;
}

// What decision 56's producer says when the resample is a no-op. Set by hand because the walk does not
// derive it yet — Frame/Evaluator.h says so in as many words — so this is the only place the sharp
// path can be entered from today.
constexpr TransformClass Sharp{ .AxisAligned = true, .Upright = true, .UnitScale = true, .IntegerOffset = true };

[[nodiscard]] RecordRequest Frame(std::span<const DrawItem> items, PixelRect<DeviceSpace> damage)
{
	RecordRequest request{};
	request.Items = items;
	request.Damage.Add(damage);

	return request;
}

// What full-range red encodes to once it has been through linear light and back, which is exactly
// what it started as: the round trip is lossless at the ends of the range.
constexpr Rgba16 Red = Rgb8(255, 0, 0);
constexpr Rgba16 Black = Rgb8(0, 0, 0);
} // namespace

// A CPU blitter handed a dmabuf is a composition-root miswiring, and Seam/RenderTarget.h carries
// discriminated memory precisely so that this is a branch somebody wrote.
GYRO_TEST(Blit, ADmabufTargetIsRefusedRatherThanAssumed)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	RenderTarget target{};
	target.Size = { 16, 16 };
	target.Format = Linear;
	target.Memory = DmabufImage{ .Planes = { DmabufPlane{ RawFd{ 7 }, 0, 64 } }, .PlaneCount = 1 };

	GYRO_CHECK_EQ(blit.BindTargets({ &target, 1 }, ColorState::Srgb()).has_value(), false);

	// And nothing is bound afterwards, so a caller that ignored the error records into nothing rather
	// than into whatever was left over.
	const std::array<DrawItem, 0> nothing{};
	GYRO_CHECK_EQ(blit.Record(Frame(nothing, PixelRect<DeviceSpace>{ {}, { 16, 16 } })).has_value(), false);
}

// The other end of the same rule: a format or a layout this cannot write is refused at bind, where
// there is time to say so, rather than at the first frame.
GYRO_TEST(Blit, AFormatOrLayoutItCannotWriteIsRefusedAtBind)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	const Surface planar{ 16, 16, PixelFormat{ FormatNv12, 0, ModifierLinear } };
	GYRO_CHECK_EQ(blit.BindTargets(planar.One(), ColorState::Srgb()).has_value(), false);

	const Surface tiled{ 16, 16, PixelFormat{ FormatXrgb8888, 0, 0x0100000000000001ULL } };
	GYRO_CHECK_EQ(blit.BindTargets(tiled.One(), ColorState::Srgb()).has_value(), false);

	// An absolute transfer function on the output, which Blit/Transfer.h declines to approximate.
	const Surface fine{ 16, 16 };
	ColorState pq = ColorState::Srgb();
	pq.Transfer = TransferFunction::Pq;
	GYRO_CHECK_EQ(blit.BindTargets(fine.One(), pq).has_value(), false);

	GYRO_CHECK(blit.BindTargets(fine.One(), ColorState::Srgb()).has_value());
}

// The whole point, at whole pixels: a solid lands exactly where the quad put it, and one pixel past
// each edge is the clear. Checked at the boundary rather than from the middle, because an off-by-one
// in the span arithmetic is invisible from the centre of a rectangle.
GYRO_TEST(Blit, ASolidLandsExactlyWhereTheQuadPutIt)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 32, 16 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 1> items{ Solid({ { 4.0F, 3.0F }, { 8.0F, 5.0F } }, 1.0F, 0.0F, 0.0F) };

	const Result<Submission> submission = blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 32, 16 } }));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	// Finished on the CPU before the call returned, which is what an immediate point means and what
	// makes the presenter's wait a no-op.
	GYRO_CHECK(blit.IsComplete(submission->Point));

	GYRO_CHECK_EQ(surface.At(4, 3), Red);
	GYRO_CHECK_EQ(surface.At(11, 7), Red);

	GYRO_CHECK_EQ(surface.At(3, 3), Black);
	GYRO_CHECK_EQ(surface.At(12, 3), Black);
	GYRO_CHECK_EQ(surface.At(4, 2), Black);
	GYRO_CHECK_EQ(surface.At(4, 8), Black);
}

// A subpixel edge is coverage rather than a jump, which is the whole of what a scaled logo needs and
// what decision 110 authors the boot scene down to.
GYRO_TEST(Blit, ASubpixelEdgeIsCoverageAndTheInteriorIsExact)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 16, 8 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	// Half a pixel in from each side, so columns 2 and 6 are half covered and 3 through 5 are whole.
	const std::array<DrawItem, 1> items{ Solid({ { 2.5F, 0.0F }, { 4.0F, 8.0F } }, 1.0F, 1.0F, 1.0F) };

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 16, 8 } })).has_value());

	GYRO_CHECK_EQ(surface.At(1, 0), Black);
	GYRO_CHECK_EQ(surface.At(3, 0), Rgb8(255, 255, 255));
	GYRO_CHECK_EQ(surface.At(5, 0), Rgb8(255, 255, 255));
	GYRO_CHECK_EQ(surface.At(7, 0), Black);

	// The two edges are between, equal to each other, and — the point of the whole exercise — above
	// the midpoint, because half the *light* encodes well above half the code. A blend done in the
	// target's own encoding would put 128 here.
	const Rgba16 left = surface.At(2, 0);
	const Rgba16 right = surface.At(6, 0);

	GYRO_CHECK_EQ(left, right);
	GYRO_CHECK(left.Red > Black.Red && left.Red < Rgb8(255, 255, 255).Red);
	GYRO_CHECK(left.Red > FromEightBit(180));
}

// A corner pixel takes the product of both coverages, which is what makes a scaled logo's corner
// fade rather than step.
GYRO_TEST(Blit, ACornerTakesTheProductOfBothCoverages)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 8, 8 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 1> items{ Solid({ { 2.5F, 2.5F }, { 3.0F, 3.0F } }, 1.0F, 1.0F, 1.0F) };

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 8 } })).has_value());

	const Rgba16 corner = surface.At(2, 2);
	const Rgba16 edge = surface.At(3, 2);
	const Rgba16 whole = surface.At(3, 3);

	// A quarter, a half, and all of it — strictly increasing, which is the assertion that the two
	// axes are multiplied rather than one of them being dropped.
	GYRO_CHECK(corner.Red < edge.Red);
	GYRO_CHECK(edge.Red < whole.Red);
	GYRO_CHECK_EQ(whole, Rgb8(255, 255, 255));
}

// `LOAD` semantics: what is outside the damage is what was there. A renderer that cleared the whole
// target would make every partial-damage frame a full repaint that happened to look right.
GYRO_TEST(Blit, WhatIsOutsideTheDamageIsUntouched)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 16, 8 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 1> items{ Solid({ {}, { 16.0F, 8.0F } }, 1.0F, 0.0F, 0.0F) };

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ { 4, 2 }, { 4, 3 } })).has_value());

	GYRO_CHECK_EQ(surface.At(4, 2), Red);
	GYRO_CHECK_EQ(surface.At(7, 4), Red);

	GYRO_CHECK(Surface::IsUntouched(surface.At(3, 2)));
	GYRO_CHECK(Surface::IsUntouched(surface.At(8, 2)));
	GYRO_CHECK(Surface::IsUntouched(surface.At(4, 1)));
	GYRO_CHECK(Surface::IsUntouched(surface.At(4, 5)));

	// Empty damage is nothing to redraw, and it is not the same as a frame nobody asked for: the
	// target keeps every byte it had.
	Surface fresh{ 8, 4 };
	GYRO_REQUIRE(blit.BindTargets(fresh.One(), ColorState::Srgb()).has_value());

	RecordRequest empty{};
	empty.Items = items;
	GYRO_REQUIRE(blit.Record(empty).has_value());
	GYRO_CHECK(Surface::IsUntouched(fresh.At(0, 0)));
}

// A target taller than one band is painted band by band, and the seams between them are not visible
// in the result. The band height is an implementation detail; a picture that changed at row 64 would
// be the way it stopped being one.
GYRO_TEST(Blit, ATargetTallerThanOneBandIsStillOnePicture)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 8, 200 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 1> items{ Solid({ { 2.0F, 0.5F }, { 4.0F, 199.0F } }, 1.0F, 0.0F, 0.0F) };

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 200 } })).has_value());

	// Every row of the interior is the same, across every band boundary there is.
	for (std::int32_t y = 1; y < 199; ++y)
	{
		GYRO_REQUIRE_EQ(surface.At(3, y), Red);
		GYRO_REQUIRE_EQ(surface.At(1, y), Black);
	}

	// And the half-covered first row is half covered in the same way the last one is.
	GYRO_CHECK_EQ(surface.At(3, 0), surface.At(3, 199));
	GYRO_CHECK(surface.At(3, 0).Red > Black.Red && surface.At(3, 0).Red < Red.Red);
}

// Preorder is the painter's order, so the later item is on top and there is nothing to sort.
GYRO_TEST(Blit, TheListOrderIsTheZOrder)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 8, 8 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 2> items{ Solid({ {}, { 8.0F, 8.0F } }, 1.0F, 0.0F, 0.0F),
		                                 Solid({ { 2.0F, 2.0F }, { 4.0F, 4.0F } }, 0.0F, 0.0F, 1.0F) };

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 8 } })).has_value());

	GYRO_CHECK_EQ(surface.At(0, 0), Red);
	GYRO_CHECK_EQ(surface.At(3, 3), Rgb8(0, 0, 255));
	GYRO_CHECK_EQ(surface.At(6, 6), Red);
}

// Per-node opacity is a blend in linear light, which is the number this whole module is arranged
// around: half of full-range white over black is not code 128.
GYRO_TEST(Blit, OpacityBlendsInLinearLight)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 8, 8 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	std::array<DrawItem, 1> items{ Solid({ {}, { 8.0F, 8.0F } }, 1.0F, 1.0F, 1.0F) };
	items[0].Opacity = 0.5F;

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 8 } })).has_value());

	// Half the light, encoded — well above the midpoint and within a code of the exact answer.
	const std::uint16_t expected = FromEightBit(static_cast<std::uint8_t>(LinearToSrgb(0.5F) * 255.0F + 0.5F));

	GYRO_CHECK(surface.At(4, 4).Red >= expected - FromEightBit(1));
	GYRO_CHECK(surface.At(4, 4).Red <= expected + FromEightBit(1));
}

// Everything decision 110 authors the boot scene away from, refused by name — and the target left
// exactly as it was, because the refusal happens before the first pixel. A `DrawTexture` was on this
// list until it was built and is not any more; what a *stale* one does is below, and it is silence
// rather than a refusal.
GYRO_TEST(Blit, WhatItCannotExpressIsRefusedBeforeAnythingIsDrawn)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 8, 8 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	const PixelRect<DeviceSpace> whole{ {}, { 8, 8 } };
	const Rect<DeviceSpace> square{ {}, { 4.0F, 4.0F } };

	const auto refused = [&](DrawItem item) {
		const std::array<DrawItem, 2> items{ Solid(square, 1.0F, 0.0F, 0.0F), item };

		return blit.Record(Frame(items, whole)).has_value();
	};

	DrawItem dressed = Solid(square, 1.0F, 1.0F, 1.0F);
	dressed.Dress = Material::Glass;
	GYRO_CHECK_EQ(refused(dressed), false);

	DrawItem lifted = Solid(square, 1.0F, 1.0F, 1.0F);
	lifted.Lift = Elevation::Resting;
	GYRO_CHECK_EQ(refused(lifted), false);

	DrawItem rounded = Solid(square, 1.0F, 1.0F, 1.0F);
	rounded.Radius = 4.0F;
	GYRO_CHECK_EQ(refused(rounded), false);

	DrawItem grouped{};
	grouped.Content = DrawGroup{ .Count = 0 };
	grouped.Shape = Quad::FromRect(square);
	GYRO_CHECK_EQ(refused(grouped), false);

	// A rotated quad. The producer culls back faces and composes the chain, so what arrives here is a
	// plane in space; this renderer rasterizes the axis-aligned ones and says so about the rest.
	DrawItem turned = Solid(square, 1.0F, 1.0F, 1.0F);
	turned.Shape.Corners[1] = { 4.0F, 0.5F };
	GYRO_CHECK_EQ(refused(turned), false);

	// A projective one, which is the same refusal reached through the weights.
	DrawItem projected = Solid(square, 1.0F, 1.0F, 1.0F);
	projected.Shape.Weights[2] = 0.5F;
	GYRO_CHECK_EQ(refused(projected), false);

	// An item whose light is not the target's. Nothing here converts between colour states, so
	// writing the numbers through as though they meant the same thing is the one failure a user could
	// not report — the picture would be subtly wrong with nothing in a log about it.
	DrawItem foreign = Solid(square, 1.0F, 1.0F, 1.0F);
	foreign.Color = ColorState::Composite();
	GYRO_CHECK_EQ(refused(foreign), false);

	// Not one pixel was written by any of them, including the legal solid that came first in every
	// list. That is what makes a refused frame safe to present: it is still the previous frame.
	GYRO_CHECK(Surface::IsUntouched(surface.At(0, 0)));
	GYRO_CHECK(Surface::IsUntouched(surface.At(7, 7)));
}

// A dressing with no material and no elevation has nothing of its own to draw. It is legal and it is
// empty, which is not the same as being refused — decision 95 puts a material on every node, so a
// node that names no content run and is dressed in nothing is an ordinary thing to emit.
GYRO_TEST(Blit, ADressingWithNothingOnItDrawsNothingRatherThanFailing)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 8, 8 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	std::array<DrawItem, 2> items{ DrawItem{}, Solid({ { 2.0F, 2.0F }, { 2.0F, 2.0F } }, 1.0F, 0.0F, 0.0F) };
	items[0].Shape = Quad::FromRect({ {}, { 8.0F, 8.0F } });

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 8 } })).has_value());

	GYRO_CHECK_EQ(surface.At(0, 0), Black);
	GYRO_CHECK_EQ(surface.At(2, 2), Red);

	// And a quad that collapsed to nothing is the same answer: a scene animated something away, which
	// a frame is not the place to report.
	Surface second{ 8, 8 };
	GYRO_REQUIRE(blit.BindTargets(second.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 1> collapsed{ Solid({ { 4.0F, 4.0F }, { 0.0F, 0.0F } }, 1.0F, 0.0F, 0.0F) };
	GYRO_REQUIRE(blit.Record(Frame(collapsed, PixelRect<DeviceSpace>{ {}, { 8, 8 } })).has_value());
	GYRO_CHECK_EQ(second.At(4, 4), Black);
}

// Several damage rectangles are several composites into one target, and the gaps between them keep
// what they had.
GYRO_TEST(Blit, EveryDamageRectangleIsPaintedAndNothingBetweenThemIs)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 16, 8 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 1> items{ Solid({ {}, { 16.0F, 8.0F } }, 1.0F, 0.0F, 0.0F) };

	RecordRequest request{};
	request.Items = items;
	request.Damage.Add(PixelRect<DeviceSpace>{ { 0, 0 }, { 4, 8 } });
	request.Damage.Add(PixelRect<DeviceSpace>{ { 12, 0 }, { 4, 8 } });

	GYRO_REQUIRE(blit.Record(request).has_value());

	GYRO_CHECK_EQ(surface.At(0, 0), Red);
	GYRO_CHECK_EQ(surface.At(15, 7), Red);
	GYRO_CHECK(Surface::IsUntouched(surface.At(8, 4)));
}

// The report a renderer with no second device owes: a cost for the work it did, and nothing at all
// for work that happened somewhere else, because there is nowhere else.
GYRO_TEST(Blit, TheCostIsAllOnOneDeviceAndThereIsNoOther)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Surface surface{ 64, 64 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 1> items{ Solid({ {}, { 64.0F, 64.0F } }, 1.0F, 0.0F, 0.0F) };

	const Result<Submission> submission = blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 64, 64 } }));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	GYRO_CHECK(submission->RecordCost >= Duration{});

	std::array<GpuCost, 4> costs{};
	GYRO_CHECK_EQ(blit.CollectCosts(costs), std::size_t{ 0 });

	// Released targets cannot be recorded into, which is what keeps a stale index from resolving to
	// whatever image took the slot.
	blit.ReleaseTargets();
	GYRO_CHECK_EQ(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 64, 64 } })).has_value(), false);
}

// The console's case: an image the size of the thing it is drawn into, landing texel for pixel. Text
// that resampled would be blurry text, which is the failure everybody recognises and nobody can
// attribute to a filter.
GYRO_TEST(Blit, ATextureAtOneToOneIsTexelForPixel)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Picture picture{ 4, 2 };
	picture.Set(0, 0, Rgb8(255, 0, 0));
	picture.Set(1, 0, Rgb8(0, 255, 0));
	picture.Set(2, 0, Rgb8(0, 0, 255));
	picture.Set(3, 0, Rgb8(255, 255, 255));
	picture.Set(0, 1, Rgb8(0, 0, 0));
	picture.Set(1, 1, Rgb8(255, 255, 0));
	picture.Set(2, 1, Rgb8(0, 255, 255));
	picture.Set(3, 1, Rgb8(255, 0, 255));

	Surface surface{ 8, 4 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(blit.Adopt(Logo, picture.Image()).has_value());

	const std::array<DrawItem, 1> items{ Textured(Logo, { { 2.0F, 1.0F }, { 4.0F, 2.0F } }) };

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 4 } })).has_value());

	// Every texel, exactly, at the offset the quad put it — and the round trip through linear light is
	// lossless at the ends of the range, so these are the codes that went in.
	GYRO_CHECK_EQ(surface.At(2, 1), Rgb8(255, 0, 0));
	GYRO_CHECK_EQ(surface.At(3, 1), Rgb8(0, 255, 0));
	GYRO_CHECK_EQ(surface.At(4, 1), Rgb8(0, 0, 255));
	GYRO_CHECK_EQ(surface.At(5, 1), Rgb8(255, 255, 255));
	GYRO_CHECK_EQ(surface.At(2, 2), Black);
	GYRO_CHECK_EQ(surface.At(3, 2), Rgb8(255, 255, 0));
	GYRO_CHECK_EQ(surface.At(5, 2), Rgb8(255, 0, 255));

	// And the clear one pixel outside it, which is what says the placement is the quad's rather than
	// the image's.
	GYRO_CHECK_EQ(surface.At(1, 1), Black);
	GYRO_CHECK_EQ(surface.At(6, 1), Black);
	GYRO_CHECK_EQ(surface.At(2, 0), Black);
	GYRO_CHECK_EQ(surface.At(2, 3), Black);
}

// The sharp path is a cost decision rather than a correctness one, and this is what makes that claim
// checkable: the same image drawn the same place through both paths is the same bytes. If it ever
// stops being, the branch has become a second picture and one of the two is wrong.
GYRO_TEST(Blit, TheSharpPathAndTheFilterAgreeWhereTheResampleIsANoOp)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Picture picture{ 5, 3 };

	for (std::int32_t y = 0; y < 3; ++y)
	{
		for (std::int32_t x = 0; x < 5; ++x)
		{
			picture.Set(x, y, Rgba8(static_cast<std::uint8_t>(x * 50), static_cast<std::uint8_t>(y * 80), 17, 255));
		}
	}

	const Rect<DeviceSpace> at{ { 1.0F, 1.0F }, { 5.0F, 3.0F } };
	const PixelRect<DeviceSpace> whole{ {}, { 8, 6 } };

	Surface filtered{ 8, 6 };
	GYRO_REQUIRE(blit.BindTargets(filtered.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(blit.Adopt(Logo, picture.Image()).has_value());

	const std::array<DrawItem, 1> resampled{ Textured(Logo, at) };
	GYRO_REQUIRE(blit.Record(Frame(resampled, whole)).has_value());

	Surface copied{ 8, 6 };
	GYRO_REQUIRE(blit.BindTargets(copied.One(), ColorState::Srgb()).has_value());

	std::array<DrawItem, 1> exact{ Textured(Logo, at) };
	exact[0].Sampling = Sharp;
	GYRO_REQUIRE(blit.Record(Frame(exact, whole)).has_value());

	for (std::int32_t y = 0; y < 6; ++y)
	{
		for (std::int32_t x = 0; x < 8; ++x)
		{
			GYRO_REQUIRE_EQ(filtered.At(x, y), copied.At(x, y));
		}
	}
}

// The logo's case, and the reason the filter is bilinear rather than a box: magnified, a box is
// nearest-neighbour, and a logo that came off the firmware smooth would come back stair-stepped at
// exactly the frame the handoff is about.
GYRO_TEST(Blit, AMagnifiedTextureIsFilteredRatherThanBlocky)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Picture picture{ 2, 1 };
	picture.Set(0, 0, Rgb8(255, 255, 255));
	picture.Set(1, 0, Rgb8(0, 0, 0));

	Surface surface{ 8, 1 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(blit.Adopt(Logo, picture.Image()).has_value());

	const std::array<DrawItem, 1> items{ Textured(Logo, { {}, { 8.0F, 1.0F } }) };

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 1 } })).has_value());

	// The two pixels either side of the midpoint. Nearest-neighbour would put 255 and 0 here; what a
	// filter puts here is a ramp, so they are between and in order.
	const Rgba16 lighter = surface.At(3, 0);
	const Rgba16 darker = surface.At(4, 0);

	GYRO_CHECK(lighter.Red < Rgb8(255, 255, 255).Red);
	GYRO_CHECK(darker.Red > Black.Red);
	GYRO_CHECK(lighter.Red > darker.Red);

	// And the ramp is in light rather than in codes. Three-eighths of the way across is 0.375 of the
	// light, which encodes to about 165; averaging the *encodings* would put 96 there, which is the
	// same dark rim a blend in the target's encoding leaves around a solid.
	GYRO_CHECK(darker.Red > FromEightBit(150));

	// Past the outermost texel centres there is nothing to interpolate towards, so the clamp holds the
	// ends at the ends rather than fading them into whatever is off the edge.
	GYRO_CHECK_EQ(surface.At(0, 0), Rgb8(255, 255, 255));
	GYRO_CHECK_EQ(surface.At(7, 0), Black);
}

// An atlas slot is sampled without its neighbours, which is what the clamp is for. Bleeding one slot
// into another is the artefact that shows up as a coloured fringe somewhere else entirely.
GYRO_TEST(Blit, ASampledRectangleTakesItsOwnTexelsAndNoOthers)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Picture picture{ 4, 1 };
	picture.Set(0, 0, Rgb8(255, 0, 0));
	picture.Set(1, 0, Rgb8(0, 255, 0));
	picture.Set(2, 0, Rgb8(0, 0, 255));
	picture.Set(3, 0, Rgb8(255, 255, 255));

	Surface surface{ 8, 1 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(blit.Adopt(Logo, picture.Image()).has_value());

	// One texel of four, magnified eight times. Every pixel is that texel and none of them has any
	// red or blue in it.
	const std::array<DrawItem, 1> items{
		Textured(Logo, { {}, { 8.0F, 1.0F } }, Rect<BufferSpace>{ { 1.0F, 0.0F }, { 1.0F, 1.0F } })
	};

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 1 } })).has_value());

	for (std::int32_t x = 0; x < 8; ++x)
	{
		GYRO_REQUIRE_EQ(surface.At(x, 0), Rgb8(0, 255, 0));
	}

	// A rectangle that is not inside the image is the scene and the image disagreeing about what was
	// adopted, and it is refused rather than clamped into something that draws.
	const std::array<DrawItem, 1> outside{
		Textured(Logo, { {}, { 8.0F, 1.0F } }, Rect<BufferSpace>{ { 2.0F, 0.0F }, { 4.0F, 1.0F } })
	};

	GYRO_CHECK_EQ(blit.Record(Frame(outside, PixelRect<DeviceSpace>{ {}, { 8, 1 } })).has_value(), false);
}

// A texture's edge is coverage, exactly as a solid's is. A logo placed at a subpixel offset is the
// case, and it is the whole of what keeps the boot picture from stepping by a pixel at the handoff.
GYRO_TEST(Blit, ATexturesSubpixelEdgeIsCoverage)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Picture picture{ 1, 1 };
	picture.Set(0, 0, Rgb8(255, 255, 255));

	Surface surface{ 8, 1 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(blit.Adopt(Logo, picture.Image()).has_value());

	// Half a pixel in from each side, so columns 2 and 6 are half covered and 3 through 5 are whole.
	const std::array<DrawItem, 1> items{ Textured(Logo, { { 2.5F, 0.0F }, { 4.0F, 1.0F } }) };

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 1 } })).has_value());

	GYRO_CHECK_EQ(surface.At(1, 0), Black);
	GYRO_CHECK_EQ(surface.At(4, 0), Rgb8(255, 255, 255));

	const Rgba16 left = surface.At(2, 0);

	GYRO_CHECK_EQ(left, surface.At(6, 0));
	GYRO_CHECK(left.Red > FromEightBit(180) && left.Red < Rgb8(255, 255, 255).Red);
}

// A translucent texel, in both of the two ways a colour state can say its alpha was applied. They are
// the same picture, which is the point: the alpha mode is a statement about the encoding rather than
// about what the texel looks like.
GYRO_TEST(Blit, ATranslucentTexelIsUndoneBeforeTheCurveAndNotAfter)
{
	const MonotonicClock clock;

	// Premultiplied sRGB: white at half alpha is stored as the encoded white *scaled*, which is not a
	// value the curve can be applied to where it stands.
	Blit premultiplied{ clock };

	Picture folded{ 2, 1, PixelFormat{ FormatArgb8888, 0, ModifierLinear } };
	folded.Set(0, 0, Rgba8(128, 128, 128, 128));
	folded.Set(1, 0, Rgba8(128, 128, 128, 128));

	Surface over{ 4, 1 };
	GYRO_REQUIRE(premultiplied.BindTargets(over.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(premultiplied.Adopt(Logo, folded.Image()).has_value());

	const std::array<DrawItem, 1> items{ Textured(Logo, { {}, { 4.0F, 1.0F } }) };
	GYRO_REQUIRE(premultiplied.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 4, 1 } })).has_value());

	// Half of white's light over black, encoded — about 188. Applying the curve to the premultiplied
	// value where it stands would put 129 here, which is the same class of error as blending in the
	// target's encoding and is invisible until somebody measures it.
	const Rgba16 blended = over.At(1, 0);

	GYRO_CHECK(blended.Red > FromEightBit(180) && blended.Red < FromEightBit(196));

	// Straight alpha is the same picture through the cheap path: the components are already the
	// colour, so they convert where they stand and the multiply happens after, in linear light.
	Blit straight{ clock };

	ColorState state = ColorState::Srgb();
	state.Alpha = AlphaMode::Straight;

	Picture apart{ 2, 1, PixelFormat{ FormatArgb8888, 0, ModifierLinear } };
	apart.Set(0, 0, Rgba8(255, 255, 255, 128));
	apart.Set(1, 0, Rgba8(255, 255, 255, 128));

	Surface beside{ 4, 1 };
	GYRO_REQUIRE(straight.BindTargets(beside.One(), state).has_value());
	GYRO_REQUIRE(straight.Adopt(Logo, apart.Image()).has_value());

	std::array<DrawItem, 1> loose{ Textured(Logo, { {}, { 4.0F, 1.0F } }) };
	loose[0].Color = state;

	GYRO_REQUIRE(straight.Record(Frame(loose, PixelRect<DeviceSpace>{ {}, { 4, 1 } })).has_value());

	// Within a code of each other, which is as close as two different roundings of the same number
	// come.
	GYRO_CHECK(beside.At(1, 0).Within(blended, FromEightBit(1)));
}

// Core/Texture.h's rule, which is the one thing in this renderer that is silent rather than refused:
// a frame is not where a lifetime bug gets reported, because the frame after it would report the same
// one again and there would be nothing on screen in between.
GYRO_TEST(Blit, ANullOrStaleIdDrawsNothingAndSaysNothing)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Picture picture{ 2, 2 };
	picture.Set(0, 0, Rgb8(255, 0, 0));
	picture.Set(1, 0, Rgb8(255, 0, 0));
	picture.Set(0, 1, Rgb8(255, 0, 0));
	picture.Set(1, 1, Rgb8(255, 0, 0));

	Surface surface{ 4, 4 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(blit.Adopt(Logo, picture.Image()).has_value());

	const PixelRect<DeviceSpace> whole{ {}, { 4, 4 } };
	const Rect<DeviceSpace> square{ {}, { 4.0F, 4.0F } };

	// The same slot, a later generation: what a client destroying a buffer the frame thread still
	// holds resolves to. Not the image that took the slot, and not an error either.
	const std::array<DrawItem, 1> stale{ Textured(TextureId{ Logo.Index, Logo.Generation + 2 }, square) };
	GYRO_REQUIRE(blit.Record(Frame(stale, whole)).has_value());
	GYRO_CHECK_EQ(surface.At(2, 2), Black);

	const std::array<DrawItem, 1> null{ Textured(TextureId{}, square) };
	GYRO_REQUIRE(blit.Record(Frame(null, whole)).has_value());
	GYRO_CHECK_EQ(surface.At(2, 2), Black);

	// And an id that was forgotten is a stale id, which is what the console has after it hands its
	// grid back.
	const std::array<DrawItem, 1> live{ Textured(Logo, square) };
	GYRO_REQUIRE(blit.Record(Frame(live, whole)).has_value());
	GYRO_CHECK_EQ(surface.At(2, 2), Red);

	blit.Forget(Logo);
	blit.Forget(Logo);

	GYRO_REQUIRE(blit.Record(Frame(live, whole)).has_value());
	GYRO_CHECK_EQ(surface.At(2, 2), Black);
}

// The other end of the same rule: what cannot be sampled is refused at adoption, where there is a
// caller to report it to, rather than at the first frame where there is not.
GYRO_TEST(Blit, AnImageThisCannotSampleIsRefusedAtAdoption)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	const Picture picture{ 4, 4 };

	GYRO_CHECK_EQ(blit.Adopt(TextureId{}, picture.Image()).has_value(), false);

	SourceImage empty = picture.Image();
	empty.Pixels = nullptr;
	GYRO_CHECK_EQ(blit.Adopt(Logo, empty).has_value(), false);

	SourceImage planar = picture.Image();
	planar.Format = PixelFormat{ FormatNv12, 0, ModifierLinear };
	GYRO_CHECK_EQ(blit.Adopt(Logo, planar).has_value(), false);

	SourceImage tiled = picture.Image();
	tiled.Format = PixelFormat{ FormatXrgb8888, 0, 0x0100000000000001ULL };
	GYRO_CHECK_EQ(blit.Adopt(Logo, tiled).has_value(), false);

	SourceImage short_ = picture.Image();
	short_.Length -= 1;
	GYRO_CHECK_EQ(blit.Adopt(Logo, short_).has_value(), false);

	SourceImage narrow = picture.Image();
	narrow.Stride = 4;
	GYRO_CHECK_EQ(blit.Adopt(Logo, narrow).has_value(), false);

	// The bound is a stated refusal rather than an estimate of a working set, and re-adopting a live
	// id replaces what it names rather than spending another slot on it.
	for (std::uint32_t slot = 0; slot < Blit::MaxImages; ++slot)
	{
		GYRO_REQUIRE(blit.Adopt(TextureId{ slot, 2 }, picture.Image()).has_value());
	}

	GYRO_CHECK(blit.Adopt(TextureId{ 0, 2 }, picture.Image()).has_value());
	GYRO_CHECK_EQ(blit.Adopt(TextureId{ 99, 2 }, picture.Image()).has_value(), false);
}

// An image outlives the target set it was drawn into. A mode change rebinds targets, and the logo on
// them is the same logo — dropping it there would make a hotplug a silent loss of every texture in
// the scene.
GYRO_TEST(Blit, AnImageOutlivesTheTargetSet)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Picture picture{ 1, 1 };
	picture.Set(0, 0, Rgb8(255, 0, 0));

	Surface first{ 4, 4 };
	GYRO_REQUIRE(blit.BindTargets(first.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(blit.Adopt(Logo, picture.Image()).has_value());

	Surface second{ 8, 8 };
	GYRO_REQUIRE(blit.BindTargets(second.One(), ColorState::Srgb()).has_value());

	const std::array<DrawItem, 1> items{ Textured(Logo, { {}, { 8.0F, 8.0F } }) };
	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 8, 8 } })).has_value());

	GYRO_CHECK_EQ(second.At(4, 4), Red);
}

// An image drawn one to one comes back as itself, at every code there is. That is a claim about the
// two tables meeting in the middle — the decode is exact at a source code and the encode is
// interpolated — and it is the property the console rests on: a grid of text blitted at unit scale
// must be the grid that was rendered, not a version of it that has been through a curve twice.
GYRO_TEST(Blit, EveryCodeSurvivesTheRoundTripThroughLinearLight)
{
	const MonotonicClock clock;
	Blit blit{ clock };

	Picture picture{ 16, 16 };

	for (std::int32_t y = 0; y < 16; ++y)
	{
		for (std::int32_t x = 0; x < 16; ++x)
		{
			const auto code = static_cast<std::uint8_t>(y * 16 + x);

			picture.Set(x, y, Rgb8(code, static_cast<std::uint8_t>(255 - code), code));
		}
	}

	Surface surface{ 16, 16 };
	GYRO_REQUIRE(blit.BindTargets(surface.One(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(blit.Adopt(Logo, picture.Image()).has_value());

	std::array<DrawItem, 1> items{ Textured(Logo, { {}, { 16.0F, 16.0F } }) };
	items[0].Sampling = Sharp;

	GYRO_REQUIRE(blit.Record(Frame(items, PixelRect<DeviceSpace>{ {}, { 16, 16 } })).has_value());

	for (std::int32_t y = 0; y < 16; ++y)
	{
		for (std::int32_t x = 0; x < 16; ++x)
		{
			const auto code = static_cast<std::uint8_t>(y * 16 + x);

			GYRO_REQUIRE_EQ(surface.At(x, y), Rgb8(code, static_cast<std::uint8_t>(255 - code), code));
		}
	}
}
