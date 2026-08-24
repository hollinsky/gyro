#include "Render/Pipeline.h"

#include <cstring>
#include <optional>
#include <print>
#include <string_view>
#include <utility>

#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Render/Device.h"
#include "Render/Textures.h"
#include "Render/Vulkan.h"
#include "Testing/Test.h"

// The variant lattice, from both ends: what opens a variant, and whether `Prepare` built every one
// an item can ask for.
//
// **The second half is the one that matters and it is not a nicety.** Decision 62 forbids a frame
// blocking on compilation, so `For` has to be *total* over what a scene can name — a variant that is
// missing is a refused frame, and a refused frame during a transition is decision 35's miss. The
// only way to know it is total is to ask for everything, which is what the closed vocabulary makes
// possible and what this file does.
//
// Gated the way Render/Device.Test.cpp's are, for the same reason: a machine with no ICD is the
// ordinary state of a minimal container, and a green run that ran nothing is rot.

namespace
{
[[nodiscard]] std::optional<VulkanDevice> Available(std::string_view test)
{
	Result<VulkanDevice> device = VulkanDevice::Open({ .Class = DeviceClass::Software });

	if (!device)
	{
		std::println(
			"  skipped Pipeline.{}: {} ({})", test, device.error().Context(), std::strerror(device.error().Code())
		);

		return std::nullopt;
	}

	return std::optional<VulkanDevice>{ std::move(*device) };
}

// Everything Core/ColorState.h can say, minus the transfer function this renderer refuses. It is
// spelled out rather than looped over an enumerator count so that adding a primary set or a curve
// fails here — the list is what "enumerable" means, and a silent extension of it is the thing
// decision 109 says reopens the whole question.
constexpr std::array<TransferFunction, 3> Transfers{ TransferFunction::Srgb,
	                                                 TransferFunction::Linear,
	                                                 TransferFunction::Pq };
constexpr std::array<ColorPrimaries, 3> Primaries{ ColorPrimaries::Bt709,
	                                               ColorPrimaries::DciP3,
	                                               ColorPrimaries::Bt2020 };
} // namespace

// **A variant is a program, so two states that draw identically must name one.** The failure this
// prevents is quiet and expensive: a key that carried, say, the alpha mode would double the lattice
// without changing a single instruction, and the cost would show up as a slower binding that nobody
// could attribute to a field.
GYRO_TEST(Pipeline, OnlyWhatChangesTheProgramOpensAVariant)
{
	const ColorState output = ColorState::Srgb();

	// The same light, said two ways. A straight-alpha fill is premultiplied on the CPU.
	const ColorState straight{ ColorPrimaries::Bt709, TransferFunction::Srgb, AlphaMode::Straight, 0, 203.0F };

	GYRO_CHECK_EQ(QuadVariant::For(output, output, true), QuadVariant::For(straight, output, true));
	GYRO_CHECK_EQ(QuadVariant::For(output, output, false).Run, 0U);
	GYRO_CHECK_EQ(QuadVariant::For(output, output, true).Run, QuadRunCorner);

	// Different light, three ways, each of which has to convert.
	const ColorState dimmer{ ColorPrimaries::Bt709, TransferFunction::Srgb, AlphaMode::Premultiplied, 0, 100.0F };
	const ColorState wider{ ColorPrimaries::Bt2020, TransferFunction::Srgb, AlphaMode::Premultiplied, 0, 203.0F };

	GYRO_CHECK((QuadVariant::For(dimmer, output, false).Run & QuadRunConvert) != 0U);
	GYRO_CHECK((QuadVariant::For(wider, output, false).Run & QuadRunConvert) != 0U);
	GYRO_CHECK((QuadVariant::For(ColorState::Composite(), output, false).Run & QuadRunConvert) != 0U);

	// The two bits are independent: a rounded node that converts nothing and a square one that does
	// are different programs, and neither implies the other.
	GYRO_CHECK_EQ(QuadVariant::For(ColorState::Composite(), output, true).Run, QuadRunConvert | QuadRunCorner);
}

// **The chain meets at absolute light, which is what makes an absolute transfer function and a
// relative one composable at all.** PQ's numbers are cd/m² and sRGB's are a fraction of whatever the
// content called its own white, so a single ratio between the two reference luminances — the obvious
// first design — is right for every pair except the ones PQ is on one side of.
GYRO_TEST(Pipeline, LuminanceIsTwoFactorsBecausePqIsAbsolute)
{
	const ColorState sdr = ColorState::Srgb();
	const ColorState pq{ ColorPrimaries::Bt2020, TransferFunction::Pq, AlphaMode::Premultiplied, 0, 203.0F };

	// Relative to relative: decode into the source's white, encode out of the target's.
	GYRO_CHECK_EQ(QuadLuminance::For(sdr, sdr).Decode, 203.0F);
	GYRO_CHECK_EQ(QuadLuminance::For(sdr, sdr).Encode, 1.0F / 203.0F);

	// PQ owns whichever end it is on, and hands that end 1.0 because its own curve is already in nits.
	GYRO_CHECK_EQ(QuadLuminance::For(pq, sdr).Decode, 1.0F);
	GYRO_CHECK_EQ(QuadLuminance::For(pq, sdr).Encode, 1.0F / 203.0F);
	GYRO_CHECK_EQ(QuadLuminance::For(sdr, pq).Decode, 203.0F);
	GYRO_CHECK_EQ(QuadLuminance::For(sdr, pq).Encode, 1.0F);
}

// **`For` must be total, because a miss inside a frame is a frame gyro cannot draw.** This asks for
// every variant every colour state this renderer accepts can produce against one output, and every
// one of them has to already exist after a single `Prepare`.
GYRO_TEST(Pipeline, PrepareBuildsEveryVariantAnItemCanAskFor)
{
	std::optional<VulkanDevice> device = Available("PrepareBuildsEveryVariantAnItemCanAskFor");

	if (!device)
	{
		return;
	}

	VulkanTextures textures{ *device };
	GYRO_REQUIRE(textures.Status().has_value());

	QuadPipeline pipeline;
	GYRO_REQUIRE(pipeline.Create(*device, textures.SetLayout()).has_value());

	const ColorState output = ColorState::Srgb();
	constexpr VkFormat Format = VK_FORMAT_B8G8R8A8_UNORM;

	// **Printed rather than asserted, and printed because decision 116 rests on it.** That entry
	// chooses to be told the output's colour state instead of enumerating it, and the argument is
	// arithmetic over this number — so the number belongs somewhere a person can see it change when a
	// driver or a variant count does. An assertion would be a threshold on somebody else's machine.
	//
	// `MonotonicClock` rather than a `<chrono>` clock, because Core/Clock.cpp is the one reader of the
	// timebase and `CheckClockDiscipline.cmake` fails the build over it — decision 57, which caught
	// this line the first time it was written.
	const MonotonicClock clock;
	const Instant started = clock.Now();

	GYRO_REQUIRE(pipeline.Prepare(Format, output).has_value());

	std::println(
		"  {} variants: {} us",
		QuadVariantsPerBinding,
		std::chrono::duration_cast<std::chrono::microseconds>(clock.Now() - started).count()
	);

	for (const TransferFunction transfer : Transfers)
	{
		for (const ColorPrimaries primaries : Primaries)
		{
			for (const float luminance : { 100.0F, 203.0F })
			{
				const ColorState item{ primaries, transfer, AlphaMode::Premultiplied, 0, luminance };

				for (const bool rounded : { false, true })
				{
					GYRO_CHECK(pipeline.For(Format, QuadVariant::For(item, output, rounded)) != VK_NULL_HANDLE);
				}
			}
		}
	}

	// A second `Prepare` for the same binding builds nothing: the lattice is the set of programs, not
	// the number of times somebody asked for it. Without this the cap is reached by rebinding.
	GYRO_REQUIRE(pipeline.Prepare(Format, output).has_value());
}

// A format the renderer cannot name, and an output nothing can be encoded to. Both are refused at
// the binding, which is the call allowed to be slow and allowed to fail — rather than per item,
// where the answer would arrive as a black output with no explanation.
GYRO_TEST(Pipeline, WhatCannotBeBoundIsRefusedAtTheBinding)
{
	std::optional<VulkanDevice> device = Available("WhatCannotBeBoundIsRefusedAtTheBinding");

	if (!device)
	{
		return;
	}

	VulkanTextures textures{ *device };
	GYRO_REQUIRE(textures.Status().has_value());

	QuadPipeline pipeline;
	GYRO_REQUIRE(pipeline.Create(*device, textures.SetLayout()).has_value());

	GYRO_CHECK_EQ(pipeline.Prepare(VK_FORMAT_UNDEFINED, ColorState::Srgb()).has_value(), false);

	const ColorState hlg{ ColorPrimaries::Bt2020, TransferFunction::Hlg, AlphaMode::Premultiplied, 0, 203.0F };

	GYRO_CHECK_EQ(pipeline.Prepare(VK_FORMAT_B8G8R8A8_UNORM, hlg).has_value(), false);
}

// A pipeline nobody prepared for is a null handle rather than a stale one from another format, which
// is what `Record` checks before it opens a command buffer.
GYRO_TEST(Pipeline, AnUnpreparedFormatHasNoPipeline)
{
	std::optional<VulkanDevice> device = Available("AnUnpreparedFormatHasNoPipeline");

	if (!device)
	{
		return;
	}

	VulkanTextures textures{ *device };
	GYRO_REQUIRE(textures.Status().has_value());

	QuadPipeline pipeline;
	GYRO_REQUIRE(pipeline.Create(*device, textures.SetLayout()).has_value());
	GYRO_REQUIRE(pipeline.Prepare(VK_FORMAT_B8G8R8A8_UNORM, ColorState::Srgb()).has_value());

	const QuadVariant plain{};

	GYRO_CHECK(pipeline.For(VK_FORMAT_B8G8R8A8_UNORM, plain) != VK_NULL_HANDLE);
	GYRO_CHECK_EQ(pipeline.For(VK_FORMAT_A2R10G10B10_UNORM_PACK32, plain), VK_NULL_HANDLE);
}
