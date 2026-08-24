#include "Render/Textures.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Seam/Importer.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The texture table, against whatever Vulkan this machine has.
//
// **What is tested here is the bookkeeping, and the pixels are Integration/RenderImport.Test.cpp's.**
// A table has three properties nothing about a picture can check: an id resolves to what was adopted
// against it and to nothing after a generation moves on, a replaced image is retired rather than
// leaked, and — the one this module exists to get right — a forgotten image is *held* until every
// renderer that might be reading it says otherwise. None of those is visible in a frame, which is
// exactly why the seam had one of them wrong for as long as `Blit` was the only implementation.
//
// Gated the way Render/Device.Test.cpp's are and asking for `Software` for the same reason: it is the
// floor tier whose failures matter, and asking for it by name is what makes this run and CI's the
// same run.

namespace
{
[[nodiscard]] std::optional<VulkanDevice> Available(std::string_view test)
{
	Result<VulkanDevice> device = VulkanDevice::Open({ .Class = DeviceClass::Software });

	if (!device)
	{
		std::println("  skipped Textures.{}: {}", test, device.error().Context());

		return std::nullopt;
	}

	return std::optional<VulkanDevice>{ std::move(*device) };
}

constexpr PixelFormat Argb{ FormatArgb8888, 0, ModifierLinear };
constexpr std::int32_t Side = 8;

// Pixels a caller keeps alive, which is the seam's contract and the thing a test is most likely to
// get wrong by accident: `Adopt` borrows, so a temporary here would be a use-after-free the driver
// reports as a corrupt image rather than as a crash.
class Bytes
{
public:
	explicit Bytes(std::uint32_t fill) : m_Words(static_cast<std::size_t>(Side) * Side, fill) {}

	[[nodiscard]] TextureSource Source() const noexcept
	{
		return TextureSource{ .Size = { Side, Side },
			                  .Format = Argb,
			                  .Memory = MappedPixels{ .Pixels = reinterpret_cast<const std::byte*>(m_Words.data()),
			                                          .Stride = static_cast<std::uint32_t>(Side) * 4U,
			                                          .Reserved = 0,
			                                          .Length = m_Words.size() * sizeof(std::uint32_t) } };
	}

private:
	std::vector<std::uint32_t> m_Words;
};

// A renderer that has not finished, until it is told it has.
//
// **The fake is what makes the deferral testable at all on the floor tier.** Decision 108 has a
// device with no exportable timeline finish its frame inside `Record`, so a real renderer on
// lavapipe reports every submission complete the instant it is made — and a rule about *work still
// in flight* asserted against a renderer that is never in flight would pass without ever exercising
// the branch. What is being tested is the importer's arithmetic over an answer it is given, so
// giving it the answer directly is the honest instrument rather than a shortcut.
class HeldFence final : public ITextureFence
{
public:
	[[nodiscard]] std::uint64_t Submitted() const noexcept override { return m_Submitted; }

	[[nodiscard]] bool Reached(std::uint64_t value) const noexcept override { return value <= m_Landed; }

	void Submit() noexcept { ++m_Submitted; }

	void Land() noexcept { m_Landed = m_Submitted; }

private:
	std::uint64_t m_Submitted = 0;
	std::uint64_t m_Landed = 0;
};
} // namespace

GYRO_TEST(Textures, AnAdoptedIdResolvesAndAStaleOneDoesNot)
{
	std::optional<VulkanDevice> device = Available("AnAdoptedIdResolvesAndAStaleOneDoesNot");

	if (!device)
	{
		return;
	}

	VulkanTextures textures{ *device };
	GYRO_REQUIRE(textures.Status().has_value());

	if (!device->Description().CopiesFromHost)
	{
		std::println("  skipped Textures.AnAdoptedIdResolvesAndAStaleOneDoesNot: no VK_EXT_host_image_copy");

		return;
	}

	const Bytes pixels{ 0xFF204060 };
	const TextureId id{ 3, 1 };

	GYRO_REQUIRE(textures.Adopt(id, pixels.Source()).has_value());
	GYRO_CHECK_EQ(textures.Held(), 1U);

	const BoundTexture bound = textures.Find(id);
	GYRO_CHECK(bound.IsValid());
	GYRO_CHECK_EQ(bound.Size.Width, Side);

	// **The whole of Core/Texture.h's generational argument, in one line.** A client destroying a
	// buffer while a published snapshot still names it leaves the index live and the generation moved
	// on, and the difference between *resolves to nothing* and *resolves to whatever took the slot* is
	// a window showing somebody else's pixels.
	GYRO_CHECK(!textures.Find(TextureId{ 3, 2 }).IsValid());
	GYRO_CHECK(!textures.Find(TextureId{ 4, 1 }).IsValid());
	GYRO_CHECK(!textures.Find(TextureId{}).IsValid());

	// A device this device wrote itself never left, so there is no ownership for a frame to reacquire
	// — which is what keeps the barrier batch empty on a machine whose clients are all software.
	GYRO_CHECK_EQ(bound.Foreign, VK_NULL_HANDLE);
}

// **Seam/Importer.h's second half, which is the rule this module was written to correct.** `Forget`
// returns having promised to release rather than having released: the watermark says the scene has
// stopped naming the id, and only the renderer knows whether its queue has stopped reading it.
GYRO_TEST(Textures, AForgottenImageIsHeldUntilEveryRendererHasFinishedWithIt)
{
	std::optional<VulkanDevice> device = Available("AForgottenImageIsHeldUntilEveryRendererHasFinishedWithIt");

	if (!device || !device->Description().CopiesFromHost)
	{
		return;
	}

	VulkanTextures textures{ *device };
	GYRO_REQUIRE(textures.Status().has_value());

	HeldFence first;
	HeldFence second;

	textures.Attach(first);
	textures.Attach(second);

	const Bytes pixels{ 0xFF000000 };
	const TextureId id{ 1, 1 };

	GYRO_REQUIRE(textures.Adopt(id, pixels.Source()).has_value());

	// Both renderers record a frame naming it, and neither has landed. This is the moment the
	// dispatch thread learns the watermark has passed the snapshot: the scene has stopped naming the
	// id and two command buffers are still reading it.
	first.Submit();
	second.Submit();

	textures.Forget(id);

	GYRO_CHECK_EQ(textures.Held(), 0U);
	GYRO_REQUIRE_EQ(textures.Retiring(), 1U);

	// One renderer landing is not enough, which is the entire reason the stamp is one value per
	// renderer rather than a single high-water mark: a two-monitor machine has two timelines, and
	// 4000 on one says nothing about 4000 on the other.
	first.Land();

	GYRO_REQUIRE(textures.Adopt(TextureId{ 2, 1 }, pixels.Source()).has_value());
	GYRO_CHECK_EQ(textures.Retiring(), 1U);

	second.Land();

	// The sweep runs at the top of both dispatch-side verbs, so the next ordinary piece of work is
	// what reclaims it — no third channel, no doorbell, and nothing on the frame path.
	textures.Forget(TextureId{ 9, 9 });
	GYRO_CHECK_EQ(textures.Retiring(), 0U);

	textures.Detach(first);
	textures.Detach(second);
}

// **A replacing `Adopt` is the same hazard under a different verb**, which the seam says and which is
// only true if the image being replaced retires the way a forgotten one does.
GYRO_TEST(Textures, ReAdoptingALiveIdRetiresWhatItReplaced)
{
	std::optional<VulkanDevice> device = Available("ReAdoptingALiveIdRetiresWhatItReplaced");

	if (!device || !device->Description().CopiesFromHost)
	{
		return;
	}

	VulkanTextures textures{ *device };
	GYRO_REQUIRE(textures.Status().has_value());

	HeldFence fence;
	textures.Attach(fence);

	const Bytes original{ 0xFF102030 };
	const Bytes replacement{ 0xFF405060 };
	const TextureId id{ 5, 1 };

	GYRO_REQUIRE(textures.Adopt(id, original.Source()).has_value());

	const VkDescriptorSet before = textures.Find(id).Set;

	fence.Submit();

	GYRO_REQUIRE(textures.Adopt(id, replacement.Source()).has_value());

	// One id, one live image, and the old one waiting on the frame that was reading it.
	GYRO_CHECK_EQ(textures.Held(), 1U);
	GYRO_CHECK_EQ(textures.Retiring(), 1U);
	GYRO_CHECK(textures.Find(id).Set != before);

	fence.Land();
	textures.Detach(fence);
}

// **A refused import never reaches a frame**, and what this checks is the half of that sentence the
// seam cannot state: that a refusal leaves the table exactly as it was. A client's next buffer
// arriving malformed must not be the reason its current one stops drawing.
GYRO_TEST(Textures, ARefusedSourceLeavesTheTableUntouched)
{
	std::optional<VulkanDevice> device = Available("ARefusedSourceLeavesTheTableUntouched");

	if (!device || !device->Description().CopiesFromHost)
	{
		return;
	}

	VulkanTextures textures{ *device };
	GYRO_REQUIRE(textures.Status().has_value());

	const Bytes pixels{ 0xFF808080 };
	const TextureId id{ 7, 1 };

	GYRO_REQUIRE(textures.Adopt(id, pixels.Source()).has_value());

	const VkDescriptorSet original = textures.Find(id).Set;

	GYRO_CHECK_EQ(textures.Adopt(TextureId{}, pixels.Source()).error().Code(), EINVAL);
	GYRO_CHECK_EQ(textures.Adopt(TextureId{ 8, 1 }, TextureSource{}).error().Code(), EINVAL);

	// A mapping shorter than the image it claims, which is the check `MappedPixels::Length` exists
	// for: a stride that is right and a length that is one row short reads past the end of a client's
	// pool, and the driver would do the reading.
	TextureSource truncated = pixels.Source();
	std::get<MappedPixels>(truncated.Memory).Length /= 2;

	GYRO_CHECK_EQ(textures.Adopt(TextureId{ 8, 1 }, truncated).error().Code(), EINVAL);

	// A format with no Vulkan meaning. `NV12` is the honest example rather than a made-up fourcc,
	// because it is a real client format and a planar one — which is a question about sampler
	// conversions rather than about plumbing, and is refused here rather than half-answered.
	TextureSource planar = pixels.Source();
	planar.Format = PixelFormat{ FormatNv12, 0, ModifierLinear };

	GYRO_CHECK_EQ(textures.Adopt(TextureId{ 8, 1 }, planar).error().Code(), EINVAL);

	GYRO_CHECK_EQ(textures.Held(), 1U);
	GYRO_CHECK_EQ(textures.Find(id).Set, original);
}
