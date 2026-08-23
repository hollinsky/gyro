#include "Render/Unfused.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <print>
#include <string_view>
#include <utility>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Vulkan.h"
#include "Testing/Test.h"

// Decision 62's reference execution, from the end that can be asserted without a picture.
//
// **The picture is Integration/RenderImport.Test.cpp's**, which draws one scene through both
// executions and measures how far apart they land — the assertion that matters and the one that
// needs a target, a presenter and a readback. What is here is the part that has to hold before that
// comparison means anything, and it is a sharper requirement than the equivalent one for a gather:
// an element with no pipeline behind it is an element the reference would *skip*, and a reference
// that skipped the same element the fused program skipped would agree perfectly and prove nothing.
// A missing pipeline is invisible on screen and fatal to the oracle, which is exactly the shape of
// thing that has to be checked here rather than there.
//
// Gated the way Render/Backdrop.Test.cpp's are, and for the same reason: a machine with no ICD is
// the ordinary state of a minimal container, and a green run that ran nothing is rot.

namespace
{
[[nodiscard]] std::optional<VulkanDevice> Available(std::string_view test)
{
	Result<VulkanDevice> device = VulkanDevice::Open({ .Class = DeviceClass::Software });

	if (!device)
	{
		std::println(
			"  skipped Unfused.{}: {} ({})", test, device.error().Context(), std::strerror(device.error().Code())
		);

		return std::nullopt;
	}

	return std::optional<VulkanDevice>{ std::move(*device) };
}

constexpr PixelSize<DeviceSpace> Resolution{ 1920, 1080 };
} // namespace

GYRO_TEST(Unfused, EveryElementOfTheChainHasAPipeline)
{
	std::optional<VulkanDevice> device = Available("EveryElementOfTheChainHasAPipeline");

	if (!device)
	{
		return;
	}

	Unfused unfused;
	GYRO_REQUIRE(unfused.Create(*device).has_value());
	GYRO_REQUIRE(!unfused.IsReady());

	const MonotonicClock clock;
	const Instant started = clock.Now();

	GYRO_REQUIRE(unfused.Reserve(Resolution, VK_FORMAT_B8G8R8A8_UNORM).has_value());
	GYRO_REQUIRE(unfused.IsReady());

	// Printed rather than asserted, for Render/Pipeline.Test.cpp's reason: a threshold here would be
	// an assertion about somebody else's machine. What is worth watching is the figure against that
	// file's twenty quad variants — this is the whole reference path and it costs a quarter of what
	// the lattice costs, which is the claim Render/Unfused.h makes about selectors as data.
	std::println(
		"  {} element pipelines and two intermediates: {} us",
		UnfusedPipelinesPerBinding,
		std::chrono::duration_cast<std::chrono::microseconds>(Elapsed(started, clock.Now())).count()
	);

	for (std::size_t index = 0; index < Elements; ++index)
	{
		GYRO_CHECK(unfused.For(static_cast<Element>(index)) != VK_NULL_HANDLE);
	}

	// Two images, and the two are distinct. A ping-pong whose halves were the same image would read
	// the pixel it is writing, which produces a plausible picture on some drivers and a corrupt one
	// on others — and either way not the reference.
	GYRO_CHECK(unfused.Image(0) != VK_NULL_HANDLE);
	GYRO_CHECK(unfused.Image(1) != VK_NULL_HANDLE);
	GYRO_CHECK(unfused.Image(0) != unfused.Image(1));
	GYRO_CHECK(unfused.Set(0) != unfused.Set(1));

	// Reserving again releases and rebuilds rather than accumulating, which is what a mode set does
	// to an output — and a second reservation that leaked the first would leak two full-resolution
	// images per reconfiguration.
	GYRO_REQUIRE(unfused.Reserve({ 640, 480 }, VK_FORMAT_B8G8R8A8_UNORM).has_value());
	GYRO_CHECK(unfused.IsReady());

	unfused.Release();
	GYRO_CHECK(!unfused.IsReady());
	GYRO_CHECK_EQ(unfused.For(Element::Emit), VkPipeline{ VK_NULL_HANDLE });
}

// A format the renderer cannot name and an extent nothing can be drawn into are refused rather than
// producing a chain that is `IsReady` and has no images. `Backdrop` answers the same two the same
// way; the difference is what a caller does about it, and Render/Unfused.h says why this one fails
// a binding where that one falls to a tint.
GYRO_TEST(Unfused, AReservationItCannotHonourIsRefusedRatherThanHalfBuilt)
{
	std::optional<VulkanDevice> device = Available("AReservationItCannotHonourIsRefusedRatherThanHalfBuilt");

	if (!device)
	{
		return;
	}

	Unfused unfused;
	GYRO_REQUIRE(unfused.Create(*device).has_value());

	GYRO_CHECK(!unfused.Reserve(Resolution, VK_FORMAT_UNDEFINED).has_value());
	GYRO_CHECK(!unfused.IsReady());

	GYRO_CHECK(!unfused.Reserve({ 0, 0 }, VK_FORMAT_B8G8R8A8_UNORM).has_value());
	GYRO_CHECK(!unfused.IsReady());

	// And the one that was refused does not poison the one that follows.
	GYRO_CHECK(unfused.Reserve(Resolution, VK_FORMAT_B8G8R8A8_UNORM).has_value());
	GYRO_CHECK(unfused.IsReady());
}
