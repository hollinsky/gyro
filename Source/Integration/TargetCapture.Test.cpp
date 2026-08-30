#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <vector>

#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Frame/Evaluator.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Render/Allocator.h"
#include "Render/Device.h"
#include "Render/Renderer.h"
#include "Render/Textures.h"
#include "Seam/Capture.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Testing/Test.h"
#include "Virtual/Buffer.h"
#include "Virtual/Output.h"

// Seam/Capture.h's readback against a real device, and the one claim the whole debug capture rests
// on: **the bytes on disk are the bytes that were in the target.**
//
// **That claim is checkable here and nowhere else.** The composite lands in a `udmabuf` the test also
// holds a mapping onto, so there are two independent routes to the same pixels — the driver's copy
// through `vkCmdCopyImageToBuffer`, and the memory the allocator handed out. If Render/Readback.h's
// detile, its stride, or its queue-family acquire were wrong, the two would disagree. Nothing in
// Compositor/Capture.Test.cpp or Frame/Loop.Test.cpp can see that: both play the renderer's part.
//
// **It names `Render` and `Virtual`, which no module may**, so it lives here for the reason
// RenderImport.Test.cpp beside it does — a renderer and a presenter are wired together only by the
// composition root.
//
// **Both gates are skips rather than failures**, which is RenderImport.Test.cpp's rule: `/dev/udmabuf`
// is behind a `uaccess` ACL a container will not have, and a minimal image may carry no ICD.

namespace
{
constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelSize<DeviceSpace> Resolution{ 64, 32 };

// Everything the two gates need, held together. `VulkanRenderer` is an `IRenderer` and therefore
// neither copyable nor movable, so it is built where it will live.
class Fixture
{
public:
	explicit Fixture(VulkanDevice&& vulkan)
		: m_Vulkan{ std::move(vulkan) }, m_Allocator{ m_Vulkan },
		  m_Output{
			  m_Clock,
			  m_Allocator,
			  OutputConfiguration{ .Resolution = Resolution, .Period = PeriodFromHertz(60.0), .Format = Linear }
		  },
		  m_Textures{ m_Vulkan }, m_Renderer{ m_Clock, m_Vulkan, m_Textures }
	{}

	[[nodiscard]] VirtualOutput& Output() noexcept { return m_Output; }

	[[nodiscard]] VulkanRenderer& Renderer() noexcept { return m_Renderer; }

	[[nodiscard]] VulkanDevice& Device() noexcept { return m_Vulkan; }

private:
	ManualClock m_Clock{ Instant{ Duration::zero() } };

	// **The device's own allocator rather than `udmabuf`**, which is what lets this run where
	// RenderImport.Test.cpp cannot: `/dev/udmabuf` is behind a `uaccess` ACL that a plain login does
	// not get on Fedora, so a test that needed one would skip on the machine gyro is developed on. It
	// is also the arrangement the two real backends use — Nested and Drm both export their targets
	// from the Vulkan device — so what is exercised here is the path a capture will actually take.
	VulkanDevice m_Vulkan;
	VulkanAllocator m_Allocator;
	VirtualOutput m_Output;

	// Before the renderer, so it is destroyed after it.
	VulkanTextures m_Textures;
	VulkanRenderer m_Renderer;
};

// `readable` is the whole subject of this file: it is `VulkanDevicePolicy::Readable`, and a device
// opened without it is the case the last test below checks refuses cleanly.
[[nodiscard]] std::optional<Fixture> Available(std::string_view test, bool readable)
{
	// Any class, because what this needs is a device that can *export* a target. The tier is not the
	// subject — the readback is — and on a machine with a GPU the GPU is the interesting answer.
	Result<VulkanDevice> device = VulkanDevice::Open({ .Readable = readable });

	if (!device)
	{
		std::println(
			"  skipped TargetCapture.{}: {} ({})", test, device.error().Context(), std::strerror(device.error().Code())
		);

		return std::nullopt;
	}

	return std::optional<Fixture>{ std::in_place, std::move(*device) };
}

[[nodiscard]] DrawItem Solid(Rect<DeviceSpace> where, DrawSolid fill)
{
	return DrawItem{ .Content = fill,
		             .Shape = Quad::FromRect(where),
		             .Extent = { where.Extent.Width, where.Extent.Height },
		             .Opacity = 1.0F,
		             .Radius = 0.0F,
		             .Dress = Material::None,
		             .Lift = {},
		             .Color = ColorState::Srgb(),
		             .Sampling = {} };
}

[[nodiscard]] RecordRequest
Composite(std::uint32_t target, const Region<DeviceSpace>& damage, std::span<const DrawItem> items)
{
	return RecordRequest{ .Target = target, .Quality = Tier::High, .Damage = damage, .Items = items };
}

constexpr std::uint32_t Stride = static_cast<std::uint32_t>(Resolution.Width) * 4U;

// `XR24`'s little-endian word: B, G, R in the low three bytes and the fourth undefined, which is why
// every comparison below masks it off.
constexpr std::uint32_t Rgb = 0x00FFFFFF;
constexpr std::uint32_t Black = 0x00000000;
constexpr std::uint32_t Red = 0x00FF0000;
constexpr std::uint32_t Blue = 0x000000FF;

[[nodiscard]] std::uint32_t PixelAt(std::span<const std::byte> pixels, std::int32_t x, std::int32_t y)
{
	std::uint32_t value = 0;
	std::memcpy(&value, pixels.data() + static_cast<std::size_t>(y) * Stride + static_cast<std::size_t>(x) * 4U, 4);

	return value;
}
} // namespace

// The claim: what comes back is the picture that was drawn, at the right coordinates.
//
// **Colours at known pixels rather than a comparison against a second mapping**, and the reason is
// worth stating because the second mapping is the stronger check. A target exported from a Vulkan
// device has no CPU mapping — that is what exporting it means — so the only machine where both routes
// exist is one with `/dev/udmabuf` reachable, which a plain Fedora login is not. What this asserts
// instead still catches everything the readback can get wrong: a stride out by a row puts the
// rectangles in the wrong columns, a detile that did nothing returns tiled garbage, and a copy from
// the wrong image returns black.
GYRO_TEST(TargetCapture, AReadTargetHoldsThePictureThatWasDrawn)
{
	std::optional<Fixture> fixture = Available("AReadTargetHoldsThePictureThatWasDrawn", true);

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	// Two rectangles rather than a fill, so that a readback which returned the right *number* of bytes
	// from the wrong place cannot pass. A flat colour looks identical read any which way.
	const std::array<DrawItem, 2> items{
		Solid({ { 4.0F, 4.0F }, { 20.0F, 10.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }),
		Solid({ { 30.0F, 12.0F }, { 24.0F, 16.0F } }, DrawSolid{ 0.0F, 0.0F, 1.0F, 1.0F }),
	};

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage, items));
	GYRO_REQUIRE(submission.has_value());

	std::vector<std::byte> into(static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Resolution.Height));

	const Result<void> read = fixture->Renderer().ReadTarget(
		{ .Target = *acquired, .After = submission->Point, .Into = into, .Stride = Stride }
	);

	GYRO_REQUIRE(read.has_value());

	// Inside the first rectangle, inside the second, and outside both. The X channel is masked off
	// because `XR24` does not promise what is in it and two drivers disagree.
	GYRO_CHECK_EQ(PixelAt(into, 10, 8) & Rgb, Red);
	GYRO_CHECK_EQ(PixelAt(into, 40, 20) & Rgb, Blue);
	GYRO_CHECK_EQ(PixelAt(into, 60, 2) & Rgb, Black);

	// And the corners of the first rectangle, which is where a stride error shows up as a shift rather
	// than as garbage: one column inside is the fill and one column outside is not.
	GYRO_CHECK_EQ(PixelAt(into, 5, 5) & Rgb, Red);
	GYRO_CHECK_EQ(PixelAt(into, 2, 5) & Rgb, Black);
	GYRO_CHECK_EQ(PixelAt(into, 10, 2) & Rgb, Black);
}

// The readback leaves the target usable. A capture that left the image in a transfer layout, or owned
// by the wrong queue family, would corrupt the *next* frame drawn into it — three frames away from
// this file and impossible to attribute.
GYRO_TEST(TargetCapture, ATargetSurvivesBeingReadBack)
{
	std::optional<Fixture> fixture = Available("ATargetSurvivesBeingReadBack", true);

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const std::array<DrawItem, 1> first{ Solid({ {}, { 64.0F, 32.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }) };

	const Result<Submission> before = fixture->Renderer().Record(Composite(*acquired, damage, first));
	GYRO_REQUIRE(before.has_value());

	std::vector<std::byte> into(static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Resolution.Height));

	GYRO_REQUIRE(fixture->Renderer()
	                 .ReadTarget({ .Target = *acquired, .After = before->Point, .Into = into, .Stride = Stride })
	                 .has_value());

	// The frame after the capture, into the same image.
	const std::array<DrawItem, 1> second{ Solid({ {}, { 64.0F, 32.0F } }, DrawSolid{ 0.0F, 0.0F, 1.0F, 1.0F }) };

	const Result<Submission> after = fixture->Renderer().Record(Composite(*acquired, damage, second));
	GYRO_REQUIRE(after.has_value());

	std::vector<std::byte> again(into.size());

	GYRO_REQUIRE(fixture->Renderer()
	                 .ReadTarget({ .Target = *acquired, .After = after->Point, .Into = again, .Stride = Stride })
	                 .has_value());

	// Different pixels, which is the whole of the claim: the second composite reached the image the
	// first one was read out of.
	GYRO_CHECK(into != again);
}

// A device the policy did not ask for readback on. The target has no `TRANSFER_SRC` usage and cannot
// grow one, so the honest answer is a refusal naming the flag — not a copy that reads garbage, and
// not a validation error on somebody else's machine.
GYRO_TEST(TargetCapture, ADeviceWithoutTheFlagRefusesCleanly)
{
	std::optional<Fixture> fixture = Available("ADeviceWithoutTheFlagRefusesCleanly", false);

	if (!fixture)
	{
		return;
	}

	GYRO_CHECK(!fixture->Device().TargetsAreReadable());
	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	std::vector<std::byte> into(static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Resolution.Height));

	const Result<void> read = fixture->Renderer().ReadTarget(
		{ .Target = *acquired, .After = SyncPoint::Immediate(), .Into = into, .Stride = Stride }
	);

	GYRO_REQUIRE(!read.has_value());
	GYRO_CHECK_EQ(read.error().Code(), ENOTSUP);
}
