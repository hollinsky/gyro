#include "Seam/RenderTarget.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>

#include "Core/Fd.h"
#include "Geometry/Space.h"
#include "Testing/Test.h"

// Two memory kinds meeting one interface is the claim this file makes, so the tests are about the
// boundary between them: a writer asks which kind it was handed and gets a null pointer rather than a
// plausible-looking reinterpretation of the other one.

namespace
{
[[nodiscard]] RenderTarget Scanout()
{
	DmabufImage image;
	image.Planes[0] = { RawFd{ 9 }, 0, 7680 };
	image.PlaneCount = 1;

	return { { 1920, 1080 }, { FormatXrgb8888, 0, ModifierLinear }, image };
}

alignas(std::max_align_t) std::array<std::byte, 4096> DumbBuffer{};

[[nodiscard]] RenderTarget Dumb()
{
	const MappedImage image{ DumbBuffer.data(), 128, 0, DumbBuffer.size() };

	return { { 32, 32 }, { FormatXrgb8888, 0, ModifierLinear }, image };
}
} // namespace

GYRO_TEST(RenderTarget, ADmabufTargetAnswersOnlyAsADmabuf)
{
	const RenderTarget target = Scanout();

	GYRO_REQUIRE(target.AsDmabuf() != nullptr);
	GYRO_CHECK(target.AsMapped() == nullptr);
	GYRO_CHECK(!target.IsMapped());
	GYRO_CHECK_EQ(target.AsDmabuf()->Used().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(target.AsDmabuf()->Used().front().Stride, std::uint32_t{ 7680 });
}

GYRO_TEST(RenderTarget, AMappedTargetAnswersOnlyAsMapped)
{
	const RenderTarget target = Dumb();

	GYRO_REQUIRE(target.AsMapped() != nullptr);
	GYRO_CHECK(target.AsDmabuf() == nullptr);
	GYRO_CHECK(target.IsMapped());
	GYRO_CHECK_EQ(target.AsMapped()->Bytes().size(), DumbBuffer.size());
}

// A half-built description is what a backend produces when a probe failed partway, and it must not
// reach a commit. IsValid is where that is asked, and it asks a different question of each kind.
GYRO_TEST(RenderTarget, AnIncompleteDescriptionIsNotValid)
{
	GYRO_CHECK(Scanout().IsValid());
	GYRO_CHECK(Dumb().IsValid());

	RenderTarget noExtent = Scanout();
	noExtent.Size = { 0, 1080 };
	GYRO_CHECK(!noExtent.IsValid());

	RenderTarget noFormat = Scanout();
	noFormat.Format = {};
	GYRO_CHECK(!noFormat.IsValid());

	RenderTarget noPlanes = Scanout();
	noPlanes.Memory = DmabufImage{};
	GYRO_CHECK(!noPlanes.IsValid());

	RenderTarget noMapping = Dumb();
	noMapping.Memory = MappedImage{};
	GYRO_CHECK(!noMapping.IsValid());
}

// A modifier is part of the format rather than a hint beside it: the same four-character code under
// two modifiers imports under one and fails under the other, which is exactly what changes when
// device migration replaces the driver underneath.
GYRO_TEST(RenderTarget, TwoModifiersAreTwoFormats)
{
	const PixelFormat linear{ FormatXrgb8888, 0, ModifierLinear };
	const PixelFormat unknown{ FormatXrgb8888, 0, ModifierInvalid };

	GYRO_CHECK(linear != unknown);
	GYRO_CHECK(linear.IsValid() && unknown.IsValid());
}

GYRO_TEST(RenderTarget, FormatsForALog)
{
	GYRO_CHECK_EQ(std::format("{}", PixelFormat{ FormatXrgb8888, 0, ModifierLinear }), std::string{ "XR24 mod 0x0" });
	GYRO_CHECK_EQ(std::format("{}", PixelFormat{ FormatNv12, 0, ModifierInvalid }), std::string{ "NV12 mod invalid" });
	GYRO_CHECK_EQ(std::format("{}", PixelFormat{}), std::string{ "none mod invalid" });

	GYRO_CHECK_EQ(std::format("{}", Scanout()), std::string{ "device(1920x1080) XR24 mod 0x0 dmabuf x1" });
	GYRO_CHECK_EQ(std::format("{}", Dumb()), std::string{ "device(32x32) XR24 mod 0x0 mapped 4096B" });
}
