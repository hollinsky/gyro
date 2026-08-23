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
// exactly as it was, because the refusal happens before the first pixel.
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

	DrawItem textured{};
	textured.Content = DrawTexture{};
	textured.Shape = Quad::FromRect(square);
	GYRO_CHECK_EQ(refused(textured), false);

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
