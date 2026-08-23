#include "Render/Backdrop.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <print>
#include <string_view>
#include <utility>

#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Vulkan.h"
#include "Seam/Dressing.h"
#include "Testing/Test.h"

// The chain a gathering material runs through, from the end that can be asserted without a picture.
//
// **What a picture would say is said in Integration/RenderImport.Test.cpp**, which draws a hard edge
// with glass over it and checks that the edge moved — the assertion that actually matters and the one
// that needs a target, a presenter, and a readback. What is here is the part that has to hold before
// a frame is recorded: that every pipeline the ladder can name exists, because decision 62 forbids a
// frame blocking on compilation and a variant an item asks for and does not find is a refused frame.
//
// Gated the way Render/Pipeline.Test.cpp's are, for the same reason: a machine with no ICD is the
// ordinary state of a minimal container, and a green run that ran nothing is rot.

namespace
{
[[nodiscard]] std::optional<VulkanDevice> Available(std::string_view test)
{
	Result<VulkanDevice> device = VulkanDevice::Open({ .Class = DeviceClass::Software });

	if (!device)
	{
		std::println(
			"  skipped Backdrop.{}: {} ({})", test, device.error().Context(), std::strerror(device.error().Code())
		);

		return std::nullopt;
	}

	return std::optional<VulkanDevice>{ std::move(*device) };
}

constexpr PixelSize<DeviceSpace> Resolution{ 1920, 1080 };
} // namespace

// **Every divisor the ladder can name, because `Dress` falls to the tint where one is missing.** That
// fallback is decision 34's third rung and it is the right answer to a device that cannot build a
// chain at all; it is the wrong answer to a tier whose pipeline nobody remembered to build, which
// would be a machine quietly rendering one rung below what it was told to. The two are
// indistinguishable on screen, so the difference has to be asserted here.
GYRO_TEST(Backdrop, EveryTierTheLadderCanNameHasAPipeline)
{
	std::optional<VulkanDevice> device = Available("EveryTierTheLadderCanNameHasAPipeline");

	if (!device)
	{
		return;
	}

	Backdrop backdrop;
	GYRO_REQUIRE(backdrop.Create(*device).has_value());

	const MonotonicClock clock;
	const Instant started = clock.Now();

	GYRO_REQUIRE(backdrop.Reserve(Resolution, VK_FORMAT_B8G8R8A8_UNORM, ColorState::Srgb(), 4).has_value());
	GYRO_REQUIRE(backdrop.IsReady());

	// Printed rather than asserted, for Render/Pipeline.Test.cpp's reason: a threshold here would be
	// an assertion about somebody else's machine. What it is worth watching is the sum against that
	// file's twenty quad variants, because both are paid at every binding — including the first one,
	// on a boot service whose whole point is being early.
	std::println(
		"  {} chain pipelines and two images: {} us",
		ChainPipelinesPerBinding,
		std::chrono::duration_cast<std::chrono::microseconds>(clock.Now() - started).count()
	);

	for (const Tier tier : { Tier::High, Tier::Medium, Tier::Low })
	{
		const ChainPlan plan = ChainPlan::For(Material::Glass, tier);

		GYRO_CHECK(plan.Passes > 0);
		GYRO_CHECK(backdrop.Extract(plan.Divisor) != VK_NULL_HANDLE);
	}

	GYRO_CHECK(backdrop.Blur() != VK_NULL_HANDLE);
	GYRO_CHECK(backdrop.Dress(true) != VK_NULL_HANDLE);
	GYRO_CHECK(backdrop.Dress(false) != VK_NULL_HANDLE);

	// The corner mask is the one axis decision 116 found survives as a real variant, so the two have
	// to be two programs rather than one handle handed back twice.
	GYRO_CHECK(backdrop.Dress(true) != backdrop.Dress(false));
}

// **Sized at the finest divisor and used in part below it**, which is what makes decision 34's *step
// down quickly* possible: a coarser tier writes into the corner of an image that is already there
// rather than waiting on an allocation at the moment the machine is already behind.
GYRO_TEST(Backdrop, TheImagesAreSizedForTheFinestTierAndTheCoarserOnesFitInside)
{
	std::optional<VulkanDevice> device = Available("TheImagesAreSizedForTheFinestTierAndTheCoarserOnesFitInside");

	if (!device)
	{
		return;
	}

	Backdrop backdrop;
	GYRO_REQUIRE(backdrop.Create(*device).has_value());
	GYRO_REQUIRE(backdrop.Reserve(Resolution, VK_FORMAT_B8G8R8A8_UNORM, ColorState::Srgb(), 4).has_value());

	const PixelSize<DeviceSpace> chain = backdrop.ChainSize();

	GYRO_CHECK_EQ(chain.Width, Resolution.Width / static_cast<std::int32_t>(FinestDivisor));
	GYRO_CHECK_EQ(chain.Height, Resolution.Height / static_cast<std::int32_t>(FinestDivisor));

	for (const Tier tier : { Tier::High, Tier::Medium, Tier::Low })
	{
		const std::uint32_t divisor = ChainPlan::For(Material::Glass, tier).Divisor;

		GYRO_CHECK(divisor >= FinestDivisor);
		GYRO_CHECK(Resolution.Width / static_cast<std::int32_t>(divisor) <= chain.Width);
		GYRO_CHECK(Resolution.Height / static_cast<std::int32_t>(divisor) <= chain.Height);
	}

	// Two images and a ping-pong, and they have to be two: a separable pass reads the whole
	// neighbourhood of the texel it writes, so writing into its own source is a partially blurred
	// image being blurred again.
	GYRO_CHECK(backdrop.ChainImage(0) != backdrop.ChainImage(1));
	GYRO_CHECK(backdrop.ChainSet(0) != backdrop.ChainSet(1));
}

// Refused at the reservation for Render/Pipeline.h's reason one pass over: BT.2100's scene-to-display
// step needs the display's peak luminance and Core/ColorState.h carries a reference white instead, so
// an implementation without one has picked a peak on the user's behalf. Every implementation that has
// done so produced a film that is subtly the wrong contrast on one panel and right on another, which
// nobody traces back to a shader.
GYRO_TEST(Backdrop, AnOutputThisChainCannotEncodeIsRefusedAtTheReservation)
{
	std::optional<VulkanDevice> device = Available("AnOutputThisChainCannotEncodeIsRefusedAtTheReservation");

	if (!device)
	{
		return;
	}

	Backdrop backdrop;
	GYRO_REQUIRE(backdrop.Create(*device).has_value());

	const ColorState hlg{ ColorPrimaries::Bt2020, TransferFunction::Hlg, AlphaMode::Premultiplied, 0, 203.0F };

	GYRO_CHECK_EQ(backdrop.Reserve(Resolution, VK_FORMAT_B8G8R8A8_UNORM, hlg, 4).has_value(), false);
	GYRO_CHECK(!backdrop.IsReady());

	// And a reservation that failed leaves nothing half-built, which matters because the caller does
	// not check: Render/Renderer.cpp treats a failure here as a tier rather than an error, so a
	// backdrop that reported failure and kept a pipeline would draw with it.
	GYRO_CHECK(backdrop.Blur() == VK_NULL_HANDLE);
	GYRO_CHECK(backdrop.ChainImage(0) == VK_NULL_HANDLE);
}

// A chain that was never reserved hands back nothing rather than something stale, and re-reserving
// replaces rather than accumulating — the two halves of an output being reconfigured under a renderer
// that keeps running, which decision 41 has happening on every boot.
GYRO_TEST(Backdrop, ReleasingAndReservingAgainIsAWholeReplacement)
{
	std::optional<VulkanDevice> device = Available("ReleasingAndReservingAgainIsAWholeReplacement");

	if (!device)
	{
		return;
	}

	Backdrop backdrop;
	GYRO_REQUIRE(backdrop.Create(*device).has_value());
	GYRO_CHECK(!backdrop.IsReady());

	GYRO_REQUIRE(backdrop.Reserve(Resolution, VK_FORMAT_B8G8R8A8_UNORM, ColorState::Srgb(), 4).has_value());

	const PixelSize<DeviceSpace> first = backdrop.ChainSize();

	GYRO_REQUIRE(backdrop.Reserve({ 640, 480 }, VK_FORMAT_B8G8R8A8_UNORM, ColorState::Srgb(), 2).has_value());
	GYRO_CHECK(backdrop.IsReady());
	GYRO_CHECK(backdrop.ChainSize().Width < first.Width);

	backdrop.Release();

	GYRO_CHECK(!backdrop.IsReady());
	GYRO_CHECK(backdrop.ChainImage(0) == VK_NULL_HANDLE);
	GYRO_CHECK(backdrop.Blur() == VK_NULL_HANDLE);
}

// An output with no extent, which is what a mode set half way through looks like from here.
GYRO_TEST(Backdrop, AReservationAgainstNoExtentIsRefused)
{
	std::optional<VulkanDevice> device = Available("AReservationAgainstNoExtentIsRefused");

	if (!device)
	{
		return;
	}

	Backdrop backdrop;
	GYRO_REQUIRE(backdrop.Create(*device).has_value());

	GYRO_CHECK_EQ(backdrop.Reserve({}, VK_FORMAT_B8G8R8A8_UNORM, ColorState::Srgb(), 4).has_value(), false);
	GYRO_CHECK_EQ(backdrop.Reserve(Resolution, VK_FORMAT_UNDEFINED, ColorState::Srgb(), 4).has_value(), false);
	GYRO_CHECK(!backdrop.IsReady());
}
