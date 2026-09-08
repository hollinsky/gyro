#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Renderer.h"
#include "Render/Textures.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"
#include "Virtual/Buffer.h"
#include "Virtual/Output.h"
#include "Virtual/Pixels.h"
#include "Virtual/Udmabuf.h"
#include "World/Elevation.h"
#include "World/Material.h"

// The renderer against a real dmabuf, all the way to the bytes.
//
// **It is here rather than in either module because neither may name the other.** `Render` is a
// renderer and `Virtual` is a presenter; the composition root is what wires them, and
// Docs/Structure.md's graph has no edge in either direction. `Integration` is the home for a test
// that names two modules no module may — which is what it already does for `Publication` and
// `Animation`.
//
// **What this covers that nothing else can.** A `SimulatedRenderer` binds heap pages, and
// `Udmabuf.Test.cpp` allocates a dmabuf nobody draws into. Between them sits the half that actually
// breaks: a descriptor the renderer did not allocate becoming a `VkImage` under a modifier the
// allocator chose, and the pixels coming back out through a mapping the driver never saw. Decision
// 102 is the entry that put a real buffer within reach of the renderer, and this is the test it was
// for.
//
// **It is also where the quad pipeline is read rather than described.** A shader is the one part of
// this codebase a compiler cannot check: the push constant block's layout, the winding of the six
// vertices, the perspective weight going back into `w`, and the corner field are each a picture that
// comes out plausible and wrong. What answers them is Virtual/Pixels.h's predicates over a real
// composite — where the fill landed, to the pixel, and what the corner cut away.
//
// **Both gates are skips rather than failures**, for the reasons Virtual/Udmabuf.Test.cpp and
// Render/Device.Test.cpp each give: `/dev/udmabuf` is behind a `uaccess` ACL a container will not
// have, and a minimal image may carry no ICD at all. A skip prints its own name; Docs/Open.md
// carries what CI should do about a run where everything skipped.

namespace
{
constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelSize<DeviceSpace> Resolution{ 64, 32 };

// What the mapping is filled with before a composite runs. Any value the renderer would not
// produce works; this one is picked because it differs in every channel from the opaque black an
// empty scene composites to, so a partial write is visible rather than half-right.
constexpr std::byte Untouched{ 0xAB };

// Everything the two gates need, held together so a test body reads as the thing being tested.
//
// Constructed rather than returned by value in pieces because `VulkanRenderer` is an `IRenderer`
// and therefore neither copyable nor movable — it has to be built where it will live.
class Fixture
{
public:
	explicit Fixture(Fd device, VulkanDevice&& vulkan)
		: m_Allocator{ std::move(device) }, m_Vulkan{ std::move(vulkan) },
		  m_Output{
			  m_Clock,
			  m_Allocator,
			  OutputConfiguration{ .Resolution = Resolution, .Period = PeriodFromHertz(60.0), .Format = Linear }
		  },
		  m_Textures{ m_Vulkan }, m_Renderer{ m_Clock, m_Vulkan, m_Textures }
	{}

	[[nodiscard]] VirtualOutput& Output() noexcept { return m_Output; }

	[[nodiscard]] VulkanRenderer& Renderer() noexcept { return m_Renderer; }

	[[nodiscard]] VulkanTextures& Textures() noexcept { return m_Textures; }

	[[nodiscard]] VulkanDevice& Device() noexcept { return m_Vulkan; }

	[[nodiscard]] ManualClock& Clock() noexcept { return m_Clock; }

	// Every target's pixels set to a value the renderer will not write, so that "was not drawn
	// into" is a claim the bytes can answer. Called *after* `BindTargets`: the import's one-time
	// layout transition out of `VK_IMAGE_LAYOUT_UNDEFINED` is the moment a driver is entitled to
	// discard what an image held, and pre-filling before it would be testing that entitlement
	// rather than the damage region.
	void Prefill()
	{
		for (std::uint32_t index = 0; index < m_Output.FreeTargets(); ++index)
		{
			const DmabufBuffer* buffer = m_Output.Buffer(index);

			if (buffer != nullptr)
			{
				std::ranges::fill(buffer->Pixels(), Untouched);
			}
		}
	}

private:
	ManualClock m_Clock{ Instant{ Duration::zero() } };
	UdmabufAllocator m_Allocator;
	VulkanDevice m_Vulkan;
	VirtualOutput m_Output;

	// Before the renderer, so it is destroyed after it: a renderer detaches from this in its
	// destructor, and the two orderings differ by whether that call lands on a live object.
	VulkanTextures m_Textures;
	VulkanRenderer m_Renderer;
};

// `Software` by default, for Render/Device.Test.cpp's reason: the machine this was written on has
// a GPU beside lavapipe, and it is the floor tier whose import path must keep working. The
// parameter exists for the one test below that wants the *other* answer to decision 108's
// question, which no software device can give.
[[nodiscard]] std::optional<Fixture> Available(std::string_view test, DeviceClass wanted = DeviceClass::Software)
{
	Result<Fd> udmabuf = OpenAndProbeUdmabuf();

	if (!udmabuf)
	{
		std::println(
			"  skipped RenderImport.{}: {} ({})", test, udmabuf.error().Context(), std::strerror(udmabuf.error().Code())
		);

		return std::nullopt;
	}

	Result<VulkanDevice> device = VulkanDevice::Open({ .Class = wanted });

	if (!device)
	{
		std::println(
			"  skipped RenderImport.{}: {} ({})", test, device.error().Context(), std::strerror(device.error().Code())
		);

		return std::nullopt;
	}

	return std::optional<Fixture>{ std::in_place, std::move(*udmabuf), std::move(*device) };
}

// The BGRA quad at a pixel, read through the mapping the allocator handed out.
[[nodiscard]] std::uint32_t PixelAt(const DmabufBuffer& buffer, std::int32_t x, std::int32_t y)
{
	const std::span<std::byte> pixels = buffer.Pixels();
	const std::size_t offset = static_cast<std::size_t>(y) * buffer.Stride() + static_cast<std::size_t>(x) * 4U;

	std::uint32_t value = 0;
	std::memcpy(&value, pixels.data() + offset, sizeof value);

	return value;
}

// One request, every field named. `-Wmissing-field-initializers` wants all of them and the tests
// want to vary two, so the naming happens once here rather than eight times below.
[[nodiscard]] RecordRequest Composite(
	std::uint32_t target,
	const Region<DeviceSpace>& damage = {},
	std::span<const DrawItem> items = {},
	RenderMode mode = RenderMode::Planned,
	std::uint32_t generation = 0
)
{
	return RecordRequest{ .Target = target,
		                  .Mode = mode,
		                  .Quality = Tier::High,
		                  .CostGeneration = generation,
		                  .Damage = damage,
		                  .Items = items,
		                  .Captures = {} };
}

// How many descriptors this process holds. Counted from `/proc` rather than tracked, because what is
// being watched is a driver's bookkeeping as much as gyro's — `vkAllocateMemory` takes ownership of
// an imported descriptor on success and leaves it to the caller on failure, and only the kernel's own
// count sees both sides of that.
[[nodiscard]] std::size_t OpenDescriptors()
{
	std::size_t count = 0;

	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{ "/proc/self/fd" })
	{
		static_cast<void>(entry);
		++count;
	}

	return count;
}

// Opaque black in an `XR24` buffer's little-endian word: B, G, R in the low three bytes and the
// fourth channel set. Spelled as the word rather than as four comparisons because that is what a
// failure prints legibly.
constexpr std::uint32_t Black = 0xFF000000;
constexpr std::uint32_t Filled = 0xABABABAB;

// A solid over a rectangle, every field named for the same reason `Composite` names its own: an
// item with a default nobody chose is how a node ends up invisible or opaque by accident.
[[nodiscard]] DrawItem Solid(Rect<DeviceSpace> where, DrawSolid fill, float opacity = 1.0F, float radius = 0.0F)
{
	return DrawItem{ .Content = fill,
		             .Shape = Quad::FromRect(where),
		             .Extent = { where.Extent.Width, where.Extent.Height },
		             .Opacity = opacity,
		             .Radius = radius,
		             .Dress = Material::None,
		             .Lift = {},
		             .Color = ColorState::Srgb(),
		             .Sampling = {} };
}

// An image a caller keeps alive, and the item that samples it.
//
// **The pixels are a member and not a temporary, because Seam/Importer.h borrows.** The seam is
// explicit that the memory stays the caller's until `Forget` returns, and a test that handed over a
// temporary would be exercising a use-after-free that a driver reports as a corrupt picture rather
// than as a crash — which is exactly the failure the contract exists to prevent, arriving in the one
// place it would be blamed on the renderer.
class Picture
{
public:
	// A quartered card: red, blue, half-alpha green, and transparent. Small enough to assert every
	// region by hand and asymmetric enough that a flipped axis or a swapped channel is a failure
	// rather than a coincidence.
	Picture(std::int32_t side, AlphaMode alpha)
		: m_Words(static_cast<std::size_t>(side) * static_cast<std::size_t>(side)), m_Side{ side }
	{
		for (std::int32_t y = 0; y < side; ++y)
		{
			for (std::int32_t x = 0; x < side; ++x)
			{
				const bool right = x >= side / 2;
				const bool bottom = y >= side / 2;

				std::uint32_t word =
					right ? (bottom ? 0x00000000U : 0xFF0000FFU) : (bottom ? Half(alpha) : 0xFFFF0000U);

				m_Words[static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)] =
					word;
			}
		}
	}

	[[nodiscard]] TextureSource Source() const noexcept
	{
		return TextureSource{ .Size = { m_Side, m_Side },
			                  .Format = PixelFormat{ FormatArgb8888, 0, ModifierLinear },
			                  .Memory = MappedPixels{ .Pixels = reinterpret_cast<const std::byte*>(m_Words.data()),
			                                          .Stride = static_cast<std::uint32_t>(m_Side) * 4U,
			                                          .Reserved = 0,
			                                          .Length = m_Words.size() * sizeof(std::uint32_t) } };
	}

private:
	// Half-alpha green, spelled both ways. The two are the same light and differ only in whether the
	// alpha has already been applied to the components — which is the whole of what
	// `QuadRunPremultiply` selects between, and the reason the two spellings have to composite to one
	// picture. 0x80 rather than 0x7F because premultiplying 1.0 by 128/255 lands back on 128 exactly,
	// so the comparison is about the fold rather than about a rounding step.
	[[nodiscard]] static std::uint32_t Half(AlphaMode alpha) noexcept
	{
		return alpha == AlphaMode::Straight ? 0x8000FF00U : 0x80008000U;
	}

	std::vector<std::uint32_t> m_Words;
	std::int32_t m_Side = 0;
};

// A textured item over a rectangle, every field named for `Solid`'s reason.
[[nodiscard]] DrawItem Textured(
	TextureId id,
	Rect<DeviceSpace> where,
	AlphaMode alpha = AlphaMode::Premultiplied,
	Rect<BufferSpace> source = {}
)
{
	ColorState state = ColorState::Srgb();
	state.Alpha = alpha;

	return DrawItem{ .Content = DrawTexture{ .Texture = id, .Source = source },
		             .Shape = Quad::FromRect(where),
		             .Extent = { where.Extent.Width, where.Extent.Height },
		             .Opacity = 1.0F,
		             .Radius = 0.0F,
		             .Dress = Material::None,
		             .Lift = {},
		             .Color = state,
		             .Sampling = {} };
}

// Everything the target holds, decoded. `BufferReader` is what holds the kernel's cache maintenance
// open across the look, which is why the view is never taken over `Pixels()` directly.
constexpr Rgba16 Red = Rgb8(255, 0, 0);
constexpr Rgba16 Blue = Rgb8(0, 0, 255);
constexpr Rgba16 Prefilled = Rgb8(0xAB, 0xAB, 0xAB);

// How far apart decision 62's two executions of one chain are allowed to be: one eight-bit code
// point of the output's encoding, which `FromEightBit` spells in the sixteen-bit depth `Rgba16`
// compares in.
//
// **A perceptual figure rather than a format one.** An eight-bit sRGB step is roughly where a
// difference stops being visible in a gradient — that is why eight bits is marginal and ten fixes
// it — so *within one step* is a claim about vision that holds whatever depth the output is
// configured for. Decision 118's arithmetic says a half-float intermediate lands about a quarter of
// a step out over this chain, so the margin is a factor of four and the printed figure is what says
// it still is.
constexpr std::uint16_t Threshold = FromEightBit(1);

// Where the fill in the two placement tests below has to have landed. Shared so that the floor tier
// and a real driver are held to one claim rather than to two that can drift apart.
void CheckThePlacement(const ImageView& image)
{
	const PixelRect<DeviceSpace> expected{ { 16, 8 }, { 24, 12 } };

	// Opaque, and to the bit. An item at full opacity with no radius has coverage of exactly one at
	// every pixel it covers, which is what makes this an equality rather than a tolerance.
	GYRO_CHECK(image.IsUniform(expected, Red));
	GYRO_CHECK_EQ(image.BoundsOfDiffering(OpaqueBlack), std::optional{ expected });

	// One pixel outside each edge, because an off-by-one in the projection is exactly what a bound
	// computed from a shape that is one pixel wrong would still report correctly if the fill bled.
	GYRO_CHECK_EQ(image.At(15, 12), OpaqueBlack);
	GYRO_CHECK_EQ(image.At(40, 12), OpaqueBlack);
	GYRO_CHECK_EQ(image.At(28, 7), OpaqueBlack);
	GYRO_CHECK_EQ(image.At(28, 20), OpaqueBlack);
}

// **The scene decision 62's oracle draws, and every element of the chain is in it.** One scene rather
// than four is deliberate: fusion is about a *run* of elements, so the case worth comparing is a list
// whose items have different runs and overlap each other, not four items each exercising one thing in
// isolation.
//
// - An opaque square that converts nothing and is not rounded, which is the shortest chain there is.
// - A rounded, half-opacity square laid over it, so that the corner mask, the scalar and the `over`
//   against something already drawn are all in one item.
// - A linear-light fill, which is the conversion on its own.
// - A rounded, three-quarter-opacity fill in Rec.2020 primaries, which is the whole chain at once and
//   the only item that runs all four passes.
//
// The two opacities are exact in every floating-point format involved, so a disagreement is the
// intermediate's rounding rather than an argument about how `0.7` was stored.
[[nodiscard]] std::array<DrawItem, 4> TheWholeChain()
{
	const ColorState linear{ ColorPrimaries::Bt709, TransferFunction::Linear, AlphaMode::Premultiplied, 0, 203.0F };
	const ColorState wide{ ColorPrimaries::Bt2020, TransferFunction::Srgb, AlphaMode::Premultiplied, 0, 203.0F };

	std::array<DrawItem, 4> items{
		Solid({ { 2.0F, 2.0F }, { 12.0F, 12.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }),
		Solid({ { 8.0F, 6.0F }, { 20.0F, 20.0F } }, DrawSolid{ 0.0F, 0.0F, 1.0F, 1.0F }, 0.5F, 6.0F),
		Solid({ { 34.0F, 4.0F }, { 24.0F, 12.0F } }, DrawSolid{ 0.5F, 0.5F, 0.5F, 1.0F }),
		Solid({ { 40.0F, 14.0F }, { 20.0F, 14.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }, 0.75F, 5.0F),
	};

	items[2].Color = linear;
	items[3].Color = wide;

	return items;
}

// The exported dmabuf, wrapped in the owner Virtual/Buffer.h already has, so that reading it back is
// the same instrument every other test in this file uses: the mapping, the kernel's cache maintenance
// around it, and Virtual/Pixels.h's predicates over the result.
//
// **A duplicate of the descriptor rather than the descriptor**, because `ExportedImage` owns its own
// and `DmabufBuffer` owns what it is handed — which is the same `dup` the renderer's import does, for
// the same reason, one layer up.
//
// `nullopt` where the buffer cannot be mapped, which is a real answer rather than a failure: a
// descriptor a GPU exported is not obliged to be CPU-visible, and what that costs here is the
// readback rather than the round trip.
[[nodiscard]] std::optional<DmabufBuffer> Readable(const ExportedImage& exported)
{
	const RenderTarget& target = exported.Target();
	const DmabufImage* image = target.AsDmabuf();

	// One plane at offset zero, which is what a linear export is and all `Mapping` can express — it
	// owns the address it unmaps, so a plane that started partway into the buffer would need the
	// mapping and the pixels to be two different pointers.
	if (image == nullptr || image->PlaneCount != 1 || image->Planes[0].Offset != 0)
	{
		return std::nullopt;
	}

	const std::size_t length =
		static_cast<std::size_t>(image->Planes[0].Stride) * static_cast<std::size_t>(target.Size.Height);
	void* pixels = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, exported.Descriptor().Value, 0);

	if (pixels == MAP_FAILED)
	{
		return std::nullopt;
	}

	Fd duplicated{ ::fcntl(exported.Descriptor().Value, F_DUPFD_CLOEXEC, 0) };

	if (!duplicated.IsValid())
	{
		::munmap(pixels, length);

		return std::nullopt;
	}

	return std::optional<DmabufBuffer>{
		std::in_place, std::move(duplicated),   target.Size,
		target.Format, image->Planes[0].Stride, Mapping{ static_cast<std::byte*>(pixels), length }
	};
}

// **Decision 62's oracle: one scene, both executions, and the difference between them measured
// rather than asserted away.**
//
// The fused path keeps every intermediate of a pointwise chain in a register; the separate-pass path
// writes each one into an offscreen and reads it back, rounding at every boundary. Decision 118
// picks the format that rounding happens at, and the claim being checked here is that a run of it
// stays inside one eight-bit code point — which is about where a difference stops being something a
// person could see in a gradient, and is what Docs/Experience.md#the-picture-is-correct means by the
// image not changing when the machine changes how it draws it.
//
// **The worst pixel is printed whether or not it passed**, which is most of why this test is worth
// having twice. A passing run that prints `0.31 code points` is evidence the assertion is still
// measuring something; a passing run that prints `0.98` is a driver upgrade about to cross the
// threshold, and nobody finds that from a green tick.
//
// **Neither side is a golden image and that is the point.** A checked-in frame states every pixel,
// including the ones nobody meant to promise, and goes stale the first time a driver rounds a corner
// arc differently. The reference here is the other execution of the same scene on the same device in
// the same run — it cannot go stale, and what it asserts is exactly the property decision 62 needs
// and nothing else.
void CheckTheTwoPathsAgree(Fixture& fixture, std::string_view label)
{
	VulkanRenderer reference{ fixture.Clock(), fixture.Device(), fixture.Textures(), Fusion::Separate };
	GYRO_REQUIRE(reference.Status().has_value());
	GYRO_REQUIRE_EQ(reference.Fuses(), Fusion::Separate);

	// Both bound before either draws, because an import transitions an image out of
	// `VK_IMAGE_LAYOUT_UNDEFINED` and a driver is entitled to discard what it held across that — so
	// binding the second renderer after the first had drawn would clear the first frame away.
	GYRO_REQUIRE(fixture.Renderer().BindTargets(fixture.Output().Targets(), ColorState::Srgb()).has_value());
	GYRO_REQUIRE(reference.BindTargets(fixture.Output().Targets(), ColorState::Srgb()).has_value());
	fixture.Prefill();

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const std::array<DrawItem, 4> items = TheWholeChain();

	const std::optional<std::uint32_t> fused = fixture.Output().AcquireTarget();
	GYRO_REQUIRE(fused.has_value());

	const Result<Submission> first = fixture.Renderer().Record(Composite(*fused, damage, items));
	GYRO_REQUIRE_EQ(first.has_value(), true);

	const std::optional<std::uint32_t> separate = fixture.Output().AcquireTarget();
	GYRO_REQUIRE(separate.has_value());
	GYRO_REQUIRE(*separate != *fused);

	const Result<Submission> second = reference.Record(Composite(*separate, damage, items));
	GYRO_REQUIRE_EQ(second.has_value(), true);

	// A poll rather than a wait, which is what the frame loop does. On the floor tier both points
	// are already immediate — decision 108 — and this spins zero times.
	while (!fixture.Renderer().IsComplete(first->Point) || !reference.IsComplete(second->Point))
	{
		// Deliberately empty, for the reason the hardware placement test above gives.
	}

	const DmabufBuffer* left = fixture.Output().Buffer(*fused);
	const DmabufBuffer* right = fixture.Output().Buffer(*separate);
	GYRO_REQUIRE(left != nullptr);
	GYRO_REQUIRE(right != nullptr);

	const BufferReader lens{ *left };
	const BufferReader other{ *right };
	GYRO_REQUIRE(lens.IsValid());
	GYRO_REQUIRE(other.IsValid());

	// Neither path may have quietly drawn nothing. Two empty composites agree perfectly, and an
	// oracle that could pass that way would be reporting the clear rather than the chain.
	GYRO_REQUIRE(lens.Image().BoundsOfDiffering(OpaqueBlack).has_value());
	GYRO_REQUIRE(other.Image().BoundsOfDiffering(OpaqueBlack).has_value());

	const ImageDifference difference = lens.Image().Compare(other.Image(), lens.Image().Extent(), Threshold);

	std::println("  {}: {}", label, difference);

	GYRO_CHECK(difference.Agrees());
}
// One frame's worth of decision 29's `C`, GPU half, checked the way the frame loop reads it.
//
// **The mode and the generation are the assertions that carry weight.** Neither is knowable at
// collection time — by then the renderer may have drawn other frames in other modes against an
// output reconfigured underneath both — so `GpuCost` carries them from the request, and a renderer
// that filled them in from its current state rather than from the submission would pass every
// timing assertion here and quietly file a floor frame's cost against the planned mark.
void CheckTheGpuCostIsReported(Fixture& fixture)
{
	GYRO_REQUIRE(fixture.Renderer().BindTargets(fixture.Output().Targets(), ColorState::Srgb()).has_value());

	const std::optional<std::uint32_t> acquired = fixture.Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	static constexpr std::uint32_t Generation = 7;
	const Result<Submission> submission =
		fixture.Renderer().Record(Composite(*acquired, damage, {}, RenderMode::Floor, Generation));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	// Polled rather than waited on, which is what the frame loop does with this — and bounded, so a
	// driver that never resolves a query fails the test instead of hanging the suite.
	std::array<GpuCost, 4> costs{};
	std::size_t collected = 0;

	for (int attempt = 0; attempt < 2000 && collected == 0; ++attempt)
	{
		collected = fixture.Renderer().CollectCosts(costs);

		if (collected == 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
		}
	}

	// A device whose graphics family cannot timestamp reports nothing forever, which is the same
	// honest answer `Blit` gives rather than a stub — so what is asserted is the implication and not
	// the answer.
	if (!fixture.Device().Description().MeasuresGpuTime())
	{
		GYRO_CHECK_EQ(collected, std::size_t{ 0 });

		return;
	}

	GYRO_REQUIRE_EQ(collected, std::size_t{ 1 });

	// Printed because the number is the thing a reader wants and no assertion can state: a composite
	// this size should read as microseconds, and a figure fifty-two times too small is a tick count
	// that never met the period.
	std::println("  gpu cost {}", costs[0].Cost);

	GYRO_CHECK_EQ(costs[0].Generation, Generation);
	GYRO_CHECK(costs[0].Mode == RenderMode::Floor);
	GYRO_CHECK(costs[0].Cost.count() > 0);

	// **The upper bound is the assertion that earns its place.** A composite of this size is
	// microseconds on any device that has one; what a whole second catches is the failure that has
	// nothing to do with speed — a raw subtraction across a timestamp counter narrower than sixty-four
	// bits, which on the Intel part below wraps every fifty-nine minutes and would yield an hour-long
	// frame, held as the mark for the next window's worth of frames.
	GYRO_CHECK(costs[0].Cost < std::chrono::seconds{ 1 });

	// Taken once. A sample filed twice is a mark that cannot come back down for a window.
	GYRO_CHECK_EQ(fixture.Renderer().CollectCosts(costs), std::size_t{ 0 });
}

} // namespace

// A screen that has come up can already draw a fade for the first window closed on it.
//
// **The claim is about *when*, not whether.** Dynamic rendering bakes the attachment's format into
// the pipeline, so the programs a snapshot is drawn under have to exist before a window closes — and
// the only place they can be built is the bind, because a frame that compiled one would spend a
// tenth of a second on the thread that owes the next refresh. What a person would see otherwise is
// the very first window they ever close taking six refreshes to begin leaving, on a machine that is
// otherwise keeping every frame.
GYRO_TEST(RenderImport, TheFirstWindowClosedOnAScreenFadesLikeEveryOneAfterIt)
{
	std::optional<Fixture> fixture = Available("TheFirstWindowClosedOnAScreenFadesLikeEveryOneAfterIt");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Output().Status().has_value());
	GYRO_REQUIRE(fixture->Renderer().Status().has_value());

	// Nothing is proven against an output nobody has bound, which is the same answer this gives after
	// a set is released.
	GYRO_CHECK_EQ(fixture->Renderer().DrawsSnapshots(), false);

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	GYRO_CHECK(fixture->Renderer().DrawsSnapshots());
}

// The whole path, in one test: allocate, import, draw the damage, read the bytes back.
GYRO_TEST(RenderImport, DamageIsDrawnAndTheRestSurvives)
{
	std::optional<Fixture> fixture = Available("DamageIsDrawnAndTheRestSurvives");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Output().Status().has_value());
	GYRO_REQUIRE(fixture->Renderer().Status().has_value());

	const std::span<const RenderTarget> targets = fixture->Output().Targets();
	GYRO_REQUIRE(!targets.empty());

	const Result<void> bound = fixture->Renderer().BindTargets(targets, ColorState::Srgb());
	GYRO_REQUIRE_EQ(bound.has_value(), true);
	GYRO_CHECK_EQ(fixture->Renderer().BoundTargets(), static_cast<std::uint32_t>(targets.size()));

	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	// Two rectangles rather than one, because a single rect is also what a whole-target repaint
	// looks like and would not distinguish a scissor from a clear.
	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ { 4, 4 }, { 8, 8 } });
	damage.Add(PixelRect<DeviceSpace>{ { 40, 20 }, { 8, 8 } });
	GYRO_REQUIRE_EQ(damage.Rects().size(), std::size_t{ 2 });

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	// The composite has to have landed before the bytes are read, and on the floor tier it already
	// has: decision 108 has a device that cannot export a timeline finish inside `Record`, so the
	// point comes back immediate. On a device that can, this is where the caller waits.
	GYRO_CHECK(fixture->Renderer().IsComplete(submission->Point));

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);
	GYRO_REQUIRE(buffer->IsMapped());

	const DmabufRead read{ *buffer };

	// Inside each rectangle: what an empty scene composites to.
	GYRO_CHECK_EQ(PixelAt(*buffer, 6, 6), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 11, 11), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 42, 22), Black);

	// Outside both: the previous contents, which is the whole of what a damage region promises. A
	// renderer that repainted the target would pass every assertion above and fail these three.
	GYRO_CHECK_EQ(PixelAt(*buffer, 30, 15), Filled);
	GYRO_CHECK_EQ(PixelAt(*buffer, 0, 0), Filled);
	GYRO_CHECK_EQ(PixelAt(*buffer, 63, 31), Filled);

	// One pixel outside each edge of the first rectangle, because an off-by-one in the scissor is the
	// defect this shape of test exists to catch and it is invisible at a distance.
	GYRO_CHECK_EQ(PixelAt(*buffer, 3, 6), Filled);
	GYRO_CHECK_EQ(PixelAt(*buffer, 12, 6), Filled);
	GYRO_CHECK_EQ(PixelAt(*buffer, 6, 3), Filled);
	GYRO_CHECK_EQ(PixelAt(*buffer, 6, 12), Filled);
}

// A frame that reaches the consumer, which is what the presenter is for. The renderer's part is the
// same; what this adds is that a composited target survives being presented and released.
GYRO_TEST(RenderImport, ACompositedFrameReachesTheConsumer)
{
	std::optional<Fixture> fixture = Available("ACompositedFrameReachesTheConsumer");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	// Every field named, which `-Wmissing-field-initializers` requires and which is the right rule
	// here: a layer with a default nobody chose is how a plane ends up blending when it should not.
	const PresentLayer layer{ .Target = *acquired,
		                      .Blend = BlendMode::Opaque,
		                      .Reserved = {},
		                      .Acquire = submission->Point,
		                      .Source = {},
		                      .Destination = PixelRect<DeviceSpace>{ {}, Resolution },
		                      .Damage = damage,
		                      .Color = ColorState::Srgb() };
	GYRO_REQUIRE_EQ(fixture->Output().Present({ &layer, 1 }).has_value(), true);

	// Past the frame boundary, so the presenter reports and the consumer is handed the image.
	fixture->Clock().Advance(PeriodFromHertz(60.0) * 2);
	fixture->Output().Advance(fixture->Clock().Now());

	const std::optional<VirtualFrame> presented = fixture->Output().PresentedFrame();
	GYRO_REQUIRE(presented.has_value());
	GYRO_CHECK_EQ(presented->Target, *acquired);

	const DmabufBuffer* buffer = fixture->Output().Buffer(presented->Target);
	GYRO_REQUIRE(buffer != nullptr);

	const DmabufRead read{ *buffer };
	GYRO_CHECK_EQ(PixelAt(*buffer, 32, 16), Black);

	fixture->Output().Release(presented->Target);
	GYRO_CHECK(!fixture->Output().PresentedFrame().has_value());
}

// **The picture a client posts, arriving on the glass.** Every other test in this file draws
// something the renderer invented; this is the first that draws something that came from outside it,
// and what it asserts is the chain end to end — a mapping becomes a `VkImage` on the dispatch thread,
// a descriptor set reaches a fragment, and the texels land where the item's corners say.
//
// **Drawn at one texel per pixel and asserted exactly, which is a claim about the filter.** The
// sampler is bilinear, and bilinear at unit scale is exact: a fragment centre maps to a texel centre,
// both weights come out zero, and the result is the texel itself. That is the property decision 56's
// sharpness path rests on and the reason a still window is not softened by being composited — so it
// is asserted as an equality rather than a tolerance, and a half-pixel error in the coordinate
// arithmetic is a failure here rather than a blur somebody notices on a font six months later.
GYRO_TEST(RenderImport, AnImportedImageLandsTexelForTexel)
{
	std::optional<Fixture> fixture = Available("AnImportedImageLandsTexelForTexel");

	if (!fixture)
	{
		return;
	}

	if (!fixture->Device().Description().CopiesFromHost)
	{
		std::println("  skipped RenderImport.AnImportedImageLandsTexelForTexel: no VK_EXT_host_image_copy");

		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	constexpr std::int32_t Side = 16;
	const Picture picture{ Side, AlphaMode::Premultiplied };
	const TextureId id{ 1, 1 };

	GYRO_REQUIRE(fixture->Textures().Adopt(id, picture.Source()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const DrawItem item = Textured(id, { { 8.0F, 8.0F }, { Side, Side } });

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 }));
	GYRO_REQUIRE_EQ(submission.has_value(), true);
	GYRO_REQUIRE(fixture->Renderer().IsComplete(submission->Point));

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const DmabufRead read{ *buffer };

	// The four quarters, at the centre of each. Red top-left, blue top-right, half-alpha green
	// bottom-left over black, and nothing at all bottom-right.
	GYRO_CHECK_EQ(PixelAt(*buffer, 12, 12), 0xFFFF0000U);
	GYRO_CHECK_EQ(PixelAt(*buffer, 20, 12), 0xFF0000FFU);
	GYRO_CHECK_EQ(PixelAt(*buffer, 12, 20), 0xFF008000U);
	GYRO_CHECK_EQ(PixelAt(*buffer, 20, 20), Black);

	// **And nothing outside the quad**, which is what says the extent reached the shader rather than
	// the image being stretched over whatever the rasterizer covered. One pixel out on each side, on
	// the three edges the transparent quarter does not touch.
	GYRO_CHECK_EQ(PixelAt(*buffer, 7, 12), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 24, 12), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 12, 7), Black);

	fixture->Textures().Forget(id);
}

// **The two spellings of one image compositing to one picture**, which
// Docs/Architecture.md#premultiplied-alpha-is-the-sharp-edge is the reason to be able to put side by
// side and which `QuadRunPremultiply` is the whole of the renderer's answer to.
//
// **This is the check that would have caught the premise that did not survive.** Render/Pipeline.h
// used to say the alpha mode costs a fragment nothing, and its argument — *a solid is four numbers* —
// is true of a fill and false of a texture. A renderer that carried that assumption forward would
// draw a straight-alpha client's translucent regions far too bright, on the one kind of content
// nobody authors by hand and everybody notices.
GYRO_TEST(RenderImport, StraightAndPremultipliedSpellingsOfOneImageAgree)
{
	std::optional<Fixture> fixture = Available("StraightAndPremultipliedSpellingsOfOneImageAgree");

	if (!fixture || !fixture->Device().Description().CopiesFromHost)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	constexpr std::int32_t Side = 16;
	const Picture folded{ Side, AlphaMode::Premultiplied };
	const Picture straight{ Side, AlphaMode::Straight };

	GYRO_REQUIRE(fixture->Textures().Adopt(TextureId{ 1, 1 }, folded.Source()).has_value());
	GYRO_REQUIRE(fixture->Textures().Adopt(TextureId{ 2, 1 }, straight.Source()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	// Side by side in one frame, over the same ground, so the comparison is between two draws of one
	// composite rather than between two frames a driver could have handled differently.
	const std::array<DrawItem, 2> items{
		Textured(TextureId{ 1, 1 }, { { 4.0F, 8.0F }, { Side, Side } }, AlphaMode::Premultiplied),
		Textured(TextureId{ 2, 1 }, { { 40.0F, 8.0F }, { Side, Side } }, AlphaMode::Straight)
	};

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage, items));
	GYRO_REQUIRE_EQ(submission.has_value(), true);
	GYRO_REQUIRE(fixture->Renderer().IsComplete(submission->Point));

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const DmabufRead read{ *buffer };

	// Every quarter, at the same offset within each copy. The opaque three are a control — they would
	// agree even if the fold were skipped — and the translucent one is the assertion.
	for (const std::int32_t x : { 4, 12 })
	{
		for (const std::int32_t y : { 12, 20 })
		{
			GYRO_CHECK_EQ(PixelAt(*buffer, 4 + x, y), PixelAt(*buffer, 40 + x, y));
		}
	}

	// Named outright as well, so a failure says *which* answer is wrong rather than only that the two
	// differ: half-alpha green over black is half green, in the output's own encoding.
	GYRO_CHECK_EQ(PixelAt(*buffer, 8, 20), 0xFF008000U);
	GYRO_CHECK_EQ(PixelAt(*buffer, 44, 20), 0xFF008000U);

	fixture->Textures().Forget(TextureId{ 1, 1 });
	fixture->Textures().Forget(TextureId{ 2, 1 });
}

// **A source rectangle naming part of an image samples that part and no other**, which is
// `wp_viewport`'s `src` reaching a fragment and is also how decision 99's overview draws one window
// at two sizes from one texture.
GYRO_TEST(RenderImport, ASourceRectangleSamplesOnlyWhatItNames)
{
	std::optional<Fixture> fixture = Available("ASourceRectangleSamplesOnlyWhatItNames");

	if (!fixture || !fixture->Device().Description().CopiesFromHost)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	constexpr std::int32_t Side = 16;
	const Picture picture{ Side, AlphaMode::Premultiplied };
	const TextureId id{ 3, 1 };

	GYRO_REQUIRE(fixture->Textures().Adopt(id, picture.Source()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	// The top-right quarter alone, drawn at its own size somewhere else on the target. Blue
	// throughout, and nothing of the other three quarters anywhere in it.
	const DrawItem item = Textured(
		id,
		{ { 8.0F, 8.0F }, { 8.0F, 8.0F } },
		AlphaMode::Premultiplied,
		Rect<BufferSpace>{ { 8.0F, 0.0F }, { 8.0F, 8.0F } }
	);

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 }));
	GYRO_REQUIRE_EQ(submission.has_value(), true);
	GYRO_REQUIRE(fixture->Renderer().IsComplete(submission->Point));

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const DmabufRead read{ *buffer };

	for (std::int32_t y = 8; y < 16; ++y)
	{
		for (std::int32_t x = 8; x < 16; ++x)
		{
			GYRO_CHECK_EQ(PixelAt(*buffer, x, y), 0xFF0000FFU);
		}
	}

	fixture->Textures().Forget(id);
}

// **Where a fill lands, to the pixel.** The quad is axis-aligned on the device grid, which decision
// 67 makes the settled case and therefore the one that has to be exact: the interior is uniformly
// the fill, everything else in the damage is the empty-scene black, and the bound of what differs is
// the rectangle the caller asked for and not one pixel more.
//
// A bound rather than a golden image, for Virtual/Pixels.h's reason — this is the assertion a driver
// that rounds a filtered edge differently still passes, and a renderer that placed the quad half a
// pixel off still fails.
GYRO_TEST(RenderImport, ASolidLandsExactlyWhereItsCornersSay)
{
	std::optional<Fixture> fixture = Available("ASolidLandsExactlyWhereItsCornersSay");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const DrawItem item = Solid({ { 16.0F, 8.0F }, { 24.0F, 12.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F });
	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 }));
	GYRO_REQUIRE_EQ(submission.has_value(), true);
	GYRO_REQUIRE(fixture->Renderer().IsComplete(submission->Point));

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	CheckThePlacement(reader.Image());
}

// **A closing window's picture is kept, and it is the picture that was on the glass.**
//
// The whole of decision 20 in one test: a window is drawn, its last frame is kept in the rectangle
// decision 46 reserved for it, and the frame after — with the window itself gone from the list — the
// same pixels come back out of that rectangle. What a person sees if this fails is a window that
// vanishes at the instant it is closed instead of leaving.
//
// **Two colours in one window, because one would not catch the interesting failure.** A snapshot is
// drawn from a run of items rather than copied out of one buffer, so a window made of more than one
// thing — which is every window, a toplevel being a container (111) — has to come back with its parts
// in the same places and the same order. A single fill would pass while the second surface was
// silently dropped.
GYRO_TEST(RenderImport, AClosingWindowLeavesWithThePictureItHadOnTheScreen)
{
	std::optional<Fixture> fixture = Available("AClosingWindowLeavesWithThePictureItHadOnTheScreen");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	// The rectangle held for one closing window, at the far corner of an atlas that is bigger than it
	// — because a slot at the origin would pass whether or not the offset was applied at all.
	const TextureId atlas{ 1, 1 };
	constexpr PixelRect<BufferSpace> Slot{ { 8, 4 }, { 16, 8 } };
	GYRO_REQUIRE(fixture->Textures().Reserve(atlas, PixelSize<BufferSpace>{ 32, 16 }).has_value());

	// Where the window is on the screen. The slot agrees with it in size, which is what makes the
	// snapshot a translation rather than a resample.
	constexpr Rect<DeviceSpace> Window{ { 24.0F, 12.0F }, { 16.0F, 8.0F } };

	const std::array<DrawItem, 2> window{
		Solid(Window, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }),
		Solid({ { 32.0F, 12.0F }, { 8.0F, 8.0F } }, DrawSolid{ 0.0F, 0.0F, 1.0F, 1.0F })
	};
	const SnapshotCapture capture{ .Into = atlas, .Slot = Slot, .Source = Window, .First = 0, .Count = 2 };

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	RecordRequest taking = Composite(*acquired, damage, window);
	taking.Captures = { &capture, 1 };

	const Result<Submission> took = fixture->Renderer().Record(taking);
	GYRO_REQUIRE_EQ(took.has_value(), true);
	GYRO_REQUIRE(fixture->Renderer().IsComplete(took->Point));

	// The renderer says what it wrote, and a caller may not believe a picture it did not.
	GYRO_CHECK_EQ(took->Captured, 1U);

	// The next frame, with the window gone from the scene and its rectangle drawn in its place —
	// which is what an exit animation does on every frame of a fade.
	acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	const DrawItem fading = Textured(
		atlas,
		{ { 4.0F, 4.0F }, { 16.0F, 8.0F } },
		AlphaMode::Premultiplied,
		Rect<BufferSpace>{ { 8.0F, 4.0F }, { 16.0F, 8.0F } }
	);

	const Result<Submission> drawn = fixture->Renderer().Record(Composite(*acquired, damage, { &fading, 1 }));
	GYRO_REQUIRE_EQ(drawn.has_value(), true);
	GYRO_REQUIRE(fixture->Renderer().IsComplete(drawn->Point));

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const DmabufRead read{ *buffer };

	// The left half of the window was red and the right half blue, and both are where they were
	// relative to the window rather than to the screen. One pixel inside each corner of where the
	// snapshot landed, because an offset lost on the way into the slot or back out of it moves this
	// rectangle without changing anything about what is inside it.
	GYRO_CHECK_EQ(PixelAt(*buffer, 5, 5), 0xFFFF0000U);
	GYRO_CHECK_EQ(PixelAt(*buffer, 5, 10), 0xFFFF0000U);
	GYRO_CHECK_EQ(PixelAt(*buffer, 18, 5), 0xFF0000FFU);
	GYRO_CHECK_EQ(PixelAt(*buffer, 18, 10), 0xFF0000FFU);

	// **And nothing above or below it.** Both colours run the full height of the window, so a
	// vertical slip would carry the picture off its rectangle without recolouring a single pixel of
	// it — the four corners alone would not notice.
	GYRO_CHECK_EQ(PixelAt(*buffer, 6, 3), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 6, 12), Black);

	fixture->Textures().Forget(atlas);
}

// **The same claim on a real driver.** Everything above runs on lavapipe, which is decision 40's
// floor tier and is the device this suite defaults to; a shader is the one thing in the tree whose
// answer can differ between a software rasterizer and hardware — pixel centres, the rounding of a
// unorm write, whether a derivative is taken at all. Skipped where there is no GPU, which is the
// same shape `AnExportingDeviceHandsOutAWaitablePoint` already has and for the same reason.
GYRO_TEST(RenderImport, ASolidLandsInTheSamePlaceOnHardware)
{
	std::optional<Fixture> fixture = Available("ASolidLandsInTheSamePlaceOnHardware", DeviceClass::Hardware);

	if (!fixture)
	{
		return;
	}

	std::println("  {}", fixture->Device().Description());

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const DrawItem item = Solid({ { 16.0F, 8.0F }, { 24.0F, 12.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F });
	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 }));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	while (!fixture->Renderer().IsComplete(submission->Point))
	{
		// Deliberately empty, for the reason the timeline test below gives: a poll rather than a
		// wait is what the frame loop does, and here the point is only that it eventually flips.
	}

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	CheckThePlacement(reader.Image());
}

// **The export path, round-tripped against the import path that already works.**
//
// A nested output has no GBM device and no swapchain, so nothing on the machine allocates its targets
// for it and it has to ask the rendering device — `VulkanDevice::Export`. What that produces is a
// dmabuf, which is the same thing `udmabuf` produces one file over, so the claim worth testing is
// that the two are interchangeable: the descriptor the device handed out goes straight back in
// through `BindTargets`, the renderer imports it under the modifier the export chose, and the solid
// lands exactly where it lands in every other test in this file.
//
// **The oracle is `CheckThePlacement`, shared with the allocated case on purpose.** Two assertions
// that could drift apart would let an exported target be *nearly* right — half a pixel off, or read
// at a stride the export reported and the import ignored — and pass. One function, two sources of
// memory, one picture.
//
// **Skipped rather than failed where the device will not export**, which is the same shape decision
// 108's timeline test has and for a related reason: a driver may render into a modifier and decline
// to hand out a descriptor for it, and lavapipe's dmabuf support is a Mesa build option rather than a
// promise. The skip prints what the device said.
GYRO_TEST(RenderImport, AnExportedTargetImportsBackAndDraws)
{
	std::optional<Fixture> fixture = Available("AnExportedTargetImportsBackAndDraws");

	if (!fixture)
	{
		return;
	}

	// Linear alone, because this is the modifier the readback below can make sense of — a tiled
	// export would import and draw correctly and produce bytes no CPU predicate can read.
	constexpr std::array<std::uint64_t, 1> Candidates{ ModifierLinear };

	const Result<ExportedImage> exported = fixture->Device().Export(Resolution, FormatXrgb8888, Candidates);

	if (!exported)
	{
		std::println(
			"  skipped RenderImport.AnExportedTargetImportsBackAndDraws: {} ({})",
			exported.error().Context(),
			std::strerror(exported.error().Code())
		);

		return;
	}

	GYRO_REQUIRE(exported->IsValid());
	GYRO_CHECK_EQ(exported->Format(), Linear);
	GYRO_CHECK(exported->Descriptor().IsValid());

	const RenderTarget& target = exported->Target();
	GYRO_REQUIRE(target.IsValid());

	const DmabufImage* image = target.AsDmabuf();
	GYRO_REQUIRE(image != nullptr);
	GYRO_CHECK_EQ(image->PlaneCount, 1U);
	GYRO_CHECK_EQ(image->Planes[0].Descriptor, exported->Descriptor());

	// The stride is the driver's and may be padded past the width; what it may not be is smaller than
	// a row, which is the failure a layout read back through the wrong aspect produces.
	GYRO_CHECK(image->Planes[0].Stride >= static_cast<std::uint32_t>(Resolution.Width) * 4U);

	// The round trip: a descriptor this device allocated, imported by this device.
	GYRO_REQUIRE(fixture->Renderer().BindTargets({ &target, 1 }, ColorState::Srgb()).has_value());
	GYRO_CHECK_EQ(fixture->Renderer().BoundTargets(), 1U);

	std::optional<DmabufBuffer> readable = Readable(*exported);

	if (!readable)
	{
		std::println("  this device's exported buffer cannot be mapped; the placement is not checked");

		return;
	}

	// After the bind, for `Fixture::Prefill`'s reason: the import transitions the image out of
	// `VK_IMAGE_LAYOUT_UNDEFINED`, and a driver is entitled to discard whatever it held across that.
	{
		const DmabufRead write{ *readable };
		std::ranges::fill(readable->Pixels(), Untouched);
	}

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const DrawItem item = Solid({ { 16.0F, 8.0F }, { 24.0F, 12.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F });
	const Result<Submission> submission = fixture->Renderer().Record(Composite(0, damage, { &item, 1 }));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	while (!fixture->Renderer().IsComplete(submission->Point))
	{
		// Deliberately empty, for the reason the hardware placement test gives: a poll rather than a
		// wait is what the frame loop does.
	}

	const BufferReader reader{ *readable };
	GYRO_REQUIRE(reader.IsValid());

	CheckThePlacement(reader.Image());

	// **The descriptor is closed with the image, which is the ownership claim the header makes and the
	// one a sanitizer cannot see.** Repeated, because one stranded descriptor is invisible and a few
	// hundred is a process that cannot open a file — the same instrument, and the same reasoning, as
	// `AHalfImportedSetLeavesNothingBehind` uses on the import side.
	const std::size_t before = OpenDescriptors();

	for (int attempt = 0; attempt < 256; ++attempt)
	{
		const Result<ExportedImage> again = fixture->Device().Export(Resolution, FormatXrgb8888, Candidates);
		GYRO_REQUIRE_EQ(again.has_value(), true);
	}

	GYRO_CHECK_EQ(OpenDescriptors(), before);
}

// The same claim on a real driver, which is where an export is anything other than a formality:
// lavapipe's memory is the heap and a GPU's is not, so the modifier the device picks, the stride it
// lays the rows out at, and whether the descriptor can be mapped at all are only real questions here.
GYRO_TEST(RenderImport, AnExportedTargetImportsBackOnHardware)
{
	std::optional<Fixture> fixture = Available("AnExportedTargetImportsBackOnHardware", DeviceClass::Hardware);

	if (!fixture)
	{
		return;
	}

	std::println("  {}", fixture->Device().Description());

	constexpr std::array<std::uint64_t, 1> Candidates{ ModifierLinear };

	const Result<ExportedImage> exported = fixture->Device().Export(Resolution, FormatXrgb8888, Candidates);

	if (!exported)
	{
		std::println(
			"  skipped RenderImport.AnExportedTargetImportsBackOnHardware: {} ({})",
			exported.error().Context(),
			std::strerror(exported.error().Code())
		);

		return;
	}

	GYRO_REQUIRE(exported->IsValid());
	GYRO_CHECK_EQ(exported->Format(), Linear);

	const RenderTarget& target = exported->Target();
	GYRO_REQUIRE(fixture->Renderer().BindTargets({ &target, 1 }, ColorState::Srgb()).has_value());

	std::optional<DmabufBuffer> readable = Readable(*exported);

	if (!readable)
	{
		std::println("  this device's exported buffer cannot be mapped; the placement is not checked");

		return;
	}

	{
		const DmabufRead write{ *readable };
		std::ranges::fill(readable->Pixels(), Untouched);
	}

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const DrawItem item = Solid({ { 16.0F, 8.0F }, { 24.0F, 12.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F });
	const Result<Submission> submission = fixture->Renderer().Record(Composite(0, damage, { &item, 1 }));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	while (!fixture->Renderer().IsComplete(submission->Point))
	{
		// Deliberately empty.
	}

	const BufferReader reader{ *readable };
	GYRO_REQUIRE(reader.IsValid());

	CheckThePlacement(reader.Image());
}

GYRO_TEST(RenderImport, TheFusedAndUnfusedChainsDrawTheSamePicture)
{
	std::optional<Fixture> fixture = Available("TheFusedAndUnfusedChainsDrawTheSamePicture");

	if (!fixture)
	{
		return;
	}

	CheckTheTwoPathsAgree(*fixture, "lavapipe");
}

// **The same claim on a real driver, and this is the pair that matters most in the file.** Everything
// else here runs on the floor tier, where both executions are the same software rasterizer rounding
// the same way; a real driver is where the two paths have a genuine chance to disagree — a different
// half-float rounding mode on the intermediate, a different order of operations after the fragment
// shader, a derivative estimated over a different quad. Two paths agreeing on lavapipe and
// disagreeing on hardware is exactly what decision 62's oracle exists to catch, and it can only
// catch it if it runs on both.
GYRO_TEST(RenderImport, TheFusedAndUnfusedChainsAgreeOnHardware)
{
	std::optional<Fixture> fixture = Available("TheFusedAndUnfusedChainsAgreeOnHardware", DeviceClass::Hardware);

	if (!fixture)
	{
		return;
	}

	std::println("  {}", fixture->Device().Description());

	CheckTheTwoPathsAgree(*fixture, "hardware");
}

// **The corner cut, and the two things about it that are easy to get backwards.** A radius removes
// the corners and leaves the middle of every edge alone; an inverted field does the opposite and
// looks, at a glance, like a rounded rectangle too. So the assertions are the extreme corner gone,
// the centre present, and the midpoint of an edge present — the third being what separates a corner
// radius from a shrunken quad.
GYRO_TEST(RenderImport, ARadiusCutsTheCornersAndNothingElse)
{
	std::optional<Fixture> fixture = Available("ARadiusCutsTheCornersAndNothingElse");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const DrawItem item = Solid({ { 8.0F, 4.0F }, { 24.0F, 24.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }, 1.0F, 8.0F);
	GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 })).has_value(), true);

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	// The four extreme corners, which a radius of eight puts well outside the arc.
	GYRO_CHECK_EQ(reader.Image().At(8, 4), OpaqueBlack);
	GYRO_CHECK_EQ(reader.Image().At(31, 4), OpaqueBlack);
	GYRO_CHECK_EQ(reader.Image().At(8, 27), OpaqueBlack);
	GYRO_CHECK_EQ(reader.Image().At(31, 27), OpaqueBlack);

	// The centre and the middle of each edge, which it does not touch.
	GYRO_CHECK_EQ(reader.Image().At(20, 16), Red);
	GYRO_CHECK_EQ(reader.Image().At(20, 4), Red);
	GYRO_CHECK_EQ(reader.Image().At(20, 27), Red);
	GYRO_CHECK_EQ(reader.Image().At(8, 16), Red);
	GYRO_CHECK_EQ(reader.Image().At(31, 16), Red);

	// And the bound is still the whole quad, because the arcs meet the edges at their ends. A field
	// that had eaten the edges as well would report a smaller one and pass every check above.
	GYRO_CHECK_EQ(
		reader.Image().BoundsOfDiffering(OpaqueBlack), std::optional{ PixelRect<DeviceSpace>{ { 8, 4 }, { 24, 24 } } }
	);
}

// **The first gathering material, and what it has to be true of is that the backdrop moved.**
//
// A hard red-to-blue edge down the middle of the output with a `Material::Glass` panel across it.
// Inside the panel the chain has pulled blue across into the red side and red across into the blue
// side, because a blur is a weighted sum over a neighbourhood; outside it the edge is exactly where
// it was. Stated as *more of the other colour than the same frame at the floor* rather than as a
// number, because the number is the tint and the sigma and belongs to Seam/Dressing.h.
//
// **The comparison against `RenderMode::Floor` is the assertion that matters.** Decision 34's third
// rung is *the material is not rendered — an opaque or simply tinted fill*, so the floored frame
// paints the same tint over an edge that is still hard. One scene, two modes, and the difference
// between them is exactly the chain. That is also the first behaviour either renderer has ever
// attached to `RenderMode`, which until now was carried and ignored.
GYRO_TEST(RenderImport, GlassBlursWhatIsBehindItAndTheFloorTintDoesNot)
{
	std::optional<Fixture> fixture = Available("GlassBlursWhatIsBehindItAndTheFloorTintDoesNot");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	// Left half red, right half blue, and a panel across the seam between them.
	DrawItem panel = Solid({ { 16.0F, 8.0F }, { 32.0F, 16.0F } }, DrawSolid{});
	panel.Content = DrawDressing{};
	panel.Dress = Material::Glass;

	const std::array<DrawItem, 3> items{
		Solid({ { 0.0F, 0.0F }, { 32.0F, 32.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }),
		Solid({ { 32.0F, 0.0F }, { 32.0F, 32.0F } }, DrawSolid{ 0.0F, 0.0F, 1.0F, 1.0F }),
		panel,
	};

	const auto draw = [&](RenderMode mode) -> std::optional<std::array<Rgba16, 4>> {
		const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();

		if (!acquired)
		{
			return std::nullopt;
		}

		if (!fixture->Renderer().Record(Composite(*acquired, damage, items, mode)))
		{
			return std::nullopt;
		}

		const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);

		if (buffer == nullptr)
		{
			return std::nullopt;
		}

		const BufferReader reader{ *buffer };

		if (!reader.IsValid())
		{
			return std::nullopt;
		}

		// Two points inside the panel, eight pixels either side of the seam, and two outside it.
		return std::array<Rgba16, 4>{
			reader.Image().At(24, 16), reader.Image().At(40, 16), reader.Image().At(24, 2), reader.Image().At(40, 2)
		};
	};

	const std::optional<std::array<Rgba16, 4>> planned = draw(RenderMode::Planned);
	GYRO_REQUIRE(planned.has_value());

	const std::optional<std::array<Rgba16, 4>> floored = draw(RenderMode::Floor);
	GYRO_REQUIRE(floored.has_value());

	// Outside the panel, the edge is untouched in both — which is what says the chain wrote where it
	// was asked to and nowhere else, and is the half that would fail if the dressing's quad were
	// placed by different arithmetic from the content's.
	GYRO_CHECK_EQ((*planned)[2], Red);
	GYRO_CHECK_EQ((*planned)[3], Blue);
	GYRO_CHECK_EQ((*floored)[2], Red);
	GYRO_CHECK_EQ((*floored)[3], Blue);

	// Inside it, eight pixels into the red side there is measurably more blue than the floor's tint
	// put there, and eight pixels into the blue side measurably more red. Both directions, because a
	// chain that only ran horizontally in one direction would pass one of them.
	GYRO_CHECK((*planned)[0].Blue > (*floored)[0].Blue);
	GYRO_CHECK((*planned)[1].Red > (*floored)[1].Red);

	// And the floored frame still shows the seam it was drawn over, which is what makes the two
	// readings a comparison rather than one of them being blank.
	GYRO_CHECK((*floored)[0].Red > (*floored)[1].Red);
	GYRO_CHECK((*floored)[1].Blue > (*floored)[0].Blue);

	// **`Smoke` is the same chain and a different obligation.** Decision 103's difference between the
	// two is not weight, it is what each must survive: `Smoke` sits over content gyro did not choose,
	// so its opacity comes from a worst-case contrast floor rather than from taste. What that has to
	// look like here is a panel that hides more of what is behind it than `Glass` does, at the same
	// place in the same scene — which is an arithmetic difference between two table rows and not a
	// second mechanism.
	std::array<DrawItem, 3> smoked{ items[0], items[1], items[2] };
	smoked[2].Dress = Material::Smoke;

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());
	GYRO_REQUIRE(fixture->Renderer().Record(Composite(*acquired, damage, smoked)).has_value());

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	const Rgba16 smoke = reader.Image().At(24, 16);

	GYRO_CHECK(smoke.Red < (*planned)[0].Red);
	GYRO_CHECK(smoke.Green < (*planned)[0].Green);
	GYRO_CHECK(smoke.Blue < (*planned)[0].Blue);

	// Still blurred, though, which is what says the heavier tint did not simply paint over the chain:
	// there is more blue eight pixels into the red side than the floored tint ever put there.
	GYRO_CHECK(smoke.Blue > (*floored)[0].Blue);
}

// **The list is the painter's order, and opacity is `over`.** Two items that overlap: the later one
// wins where they meet, which is decision 55's strict tree order arriving at the renderer as nothing
// more than the order of a span. The half-opaque third is what says the blend is `over` with a
// premultiplied source rather than a replace — a source factor of one on components that were not
// premultiplied comes out saturated instead of half.
GYRO_TEST(RenderImport, ItemsPaintInListOrderAndOpacityBlends)
{
	std::optional<Fixture> fixture = Available("ItemsPaintInListOrderAndOpacityBlends");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const std::array<DrawItem, 3> items{
		Solid({ { 0.0F, 0.0F }, { 32.0F, 32.0F } }, DrawSolid{ 0.0F, 0.0F, 1.0F, 1.0F }),
		Solid({ { 16.0F, 0.0F }, { 16.0F, 32.0F } }, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }),
		// Premultiplied at half: the components are already scaled by the alpha they carry, which is
		// what `ColorState::Srgb()` says of them.
		Solid({ { 40.0F, 0.0F }, { 16.0F, 32.0F } }, DrawSolid{ 0.5F, 0.0F, 0.0F, 0.5F }),
	};

	GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, items)).has_value(), true);

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	// Blue where only the first reached, red where the second covered it. Reversed order would pass
	// the first of these and fail the second.
	GYRO_CHECK_EQ(reader.Image().At(8, 16), Blue);
	GYRO_CHECK_EQ(reader.Image().At(24, 16), Red);

	// Half red over the empty scene's black. One least-significant bit of tolerance, because the
	// rounding of 0.5 into eight bits is the driver's and both answers are correct.
	GYRO_CHECK(reader.Image().IsUniform(PixelRect<DeviceSpace>{ { 44, 8 }, { 8, 16 } }, Rgb8(128, 0, 0), 300));
}

// **The colour-state conversion, checked against arithmetic rather than against itself.** Chain.glsl
// argues that the fused and unfused paths must be built from one set of functions, which buys a real
// oracle for composition and precision and buys nothing at all for the elements themselves: a curve
// written once is wrong once, identically on both sides. So the elements are checked the only way a
// single implementation can be — against the numbers the standards state — and this is that check.
//
// Three conversions, each isolating one stage. Two least significant bits of tolerance, because
// `pow` is the driver's and a compositor that demanded the last bit of it would fail on hardware
// that is drawing the right picture.
GYRO_TEST(RenderImport, AColourStateIsConvertedByTheArithmeticTheStandardsState)
{
	std::optional<Fixture> fixture = Available("AColourStateIsConvertedByTheArithmeticTheStandardsState");

	if (!fixture)
	{
		return;
	}

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const Rect<DeviceSpace> where{ { 0.0F, 0.0F }, { 32.0F, 32.0F } };
	const PixelRect<DeviceSpace> inside{ { 4, 4 }, { 24, 24 } };

	// Two least significant bits of an eight-bit channel, in the sixteen-bit space `Rgba16` compares
	// in — 257 per bit, and `pow` is where the drivers differ.
	constexpr std::uint32_t Tolerance = 600;

	// **The transfer function alone.** Linear light at 0.5 under Bt709 primaries, onto an sRGB output
	// of the same primaries and the same reference white: the matrix is the identity, the two
	// luminance factors cancel, and what is left is the sRGB encode of one half — 187.5, which is what
	// a person sees as mid grey and what naive `0.5 * 255` gets wrong by fifty-nine levels.
	{
		GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

		const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
		GYRO_REQUIRE(acquired.has_value());

		DrawItem item = Solid(where, DrawSolid{ 0.5F, 0.5F, 0.5F, 1.0F });
		item.Color = ColorState{ ColorPrimaries::Bt709, TransferFunction::Linear, AlphaMode::Premultiplied, 0, 203.0F };

		GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 })).has_value(), true);

		const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
		GYRO_REQUIRE(buffer != nullptr);

		const BufferReader reader{ *buffer };
		GYRO_REQUIRE(reader.IsValid());
		GYRO_CHECK(reader.Image().IsUniform(inside, Rgb8(188, 188, 188), Tolerance));
	}

	// **The primaries matrix alone.** Bt709's own red, in linear light, onto an output at Bt2020
	// primaries: the same light needs less of a wider gamut's red and a little of its green and blue,
	// which is the whole content of the matrix. Getting it transposed produces a colour rather than an
	// obvious failure, which is why the check is three channels and not one.
	{
		const ColorState wide{ ColorPrimaries::Bt2020, TransferFunction::Srgb, AlphaMode::Premultiplied, 0, 203.0F };

		GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), wide).has_value());

		const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
		GYRO_REQUIRE(acquired.has_value());

		DrawItem item = Solid(where, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F });
		item.Color = ColorState{ ColorPrimaries::Bt709, TransferFunction::Linear, AlphaMode::Premultiplied, 0, 203.0F };

		GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 })).has_value(), true);

		const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
		GYRO_REQUIRE(buffer != nullptr);

		const BufferReader reader{ *buffer };
		GYRO_REQUIRE(reader.IsValid());
		GYRO_CHECK(reader.Image().IsUniform(inside, Rgb8(208, 74, 34), Tolerance));
	}

	// **The reference white alone**, which is the case a comparison of the two enumerators would call
	// identical. Content that calls 406 nits its own 1.0, shown on an output that calls 203 nits its
	// own, is twice as bright — and the doubling happens in linear light, so mid grey lands at 175
	// rather than at the 255 a naive multiply in the encoding would saturate to.
	{
		GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

		const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
		GYRO_REQUIRE(acquired.has_value());

		DrawItem item = Solid(where, DrawSolid{ 0.5F, 0.5F, 0.5F, 1.0F });
		item.Color = ColorState{ ColorPrimaries::Bt709, TransferFunction::Srgb, AlphaMode::Premultiplied, 0, 406.0F };

		GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 })).has_value(), true);

		const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
		GYRO_REQUIRE(buffer != nullptr);

		const BufferReader reader{ *buffer };
		GYRO_REQUIRE(reader.IsValid());
		GYRO_CHECK(reader.Image().IsUniform(inside, Rgb8(175, 175, 175), Tolerance));
	}
}

// **The refusals, which are what keep a half-built renderer from lying about a scene.** Each of these
// composites successfully under a renderer that ignored what it could not draw, and each is a
// different picture from the one the scene described — so the seam's `EINVAL` is the only honest
// answer until the pipeline covering it exists.
GYRO_TEST(RenderImport, WhatTheQuadPipelineCannotExpressIsRefused)
{
	std::optional<Fixture> fixture = Available("WhatTheQuadPipelineCannotExpressIsRefused");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const Rect<DeviceSpace> where{ { 8.0F, 8.0F }, { 16.0F, 16.0F } };
	const DrawItem drawable = Solid(where, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F });

	// The baseline, so that the refusals below are about what changed rather than about the fixture.
	GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &drawable, 1 })).has_value(), true);

	std::array<DrawItem, 3> refused{ drawable, drawable, drawable };
	refused[0].Content = DrawGroup{ .Count = 0 };

	// **A lifted node is no longer refused; a lifted node *tilted out of the plane* is.** Whether a
	// flipping card's shadow stretches away from its near edge, and whether it softens across itself,
	// is a question Docs/Open.md leaves to the first transition that turns a node — so a quad that has
	// stopped being a rectangle is refused rather than drawn as whichever answer was easier to write.
	// A perspective weight is what makes this one a tilt rather than a spin, and decision 133 is that
	// the two are different questions.
	refused[1].Lift = Cast(Elevation::Resting);
	refused[1].Shape.Weights[1] = 0.8F;
	refused[1].Shape.Weights[2] = 0.8F;

	// **The colour-state refusal is now one transfer function rather than every conversion**, and HLG
	// is the one because converting it needs a display peak luminance `ColorState` does not carry.
	// Everything else — a solid authored in linear light at wide primaries, drawn onto an sRGB
	// output — is a specialization constant and a matrix that exist, and the check below is that it
	// draws rather than that it is refused.
	refused[2].Color = ColorState{ ColorPrimaries::Bt2020, TransferFunction::Hlg, AlphaMode::Premultiplied, 0, 203.0F };

	for (const DrawItem& item : refused)
	{
		const Result<Submission> answer = fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 }));
		GYRO_REQUIRE_EQ(answer.has_value(), false);
		GYRO_CHECK_EQ(answer.error().Code(), EINVAL);
	}

	// **A texture left this list and did not become a refusal somewhere else** *(2026-08-23)*. It used
	// to be here because nothing sampled; now that something does, an item naming an id the table does
	// not hold draws *nothing* and reports success — which is Core/Texture.h's rule and not a
	// concession. The id in a published snapshot is stale exactly when a client destroyed its buffer
	// while a frame was in flight, and that is a race the dispatch thread has already resolved by the
	// time this frame lands; refusing the whole composite would take every other window on the panel
	// down with it, once per frame, until the next snapshot crossed.
	DrawItem vanished = drawable;
	vanished.Content = DrawTexture{};

	GYRO_CHECK_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &vanished, 1 })).has_value(), true);

	// **A material is no longer among them**, and it is the third branch to leave this list rather
	// than the first. `Material::Glass` reads the composite back, blurs it, and composites the result
	// — Render/Backdrop.h — and where the device or the target's modifier will not have that, it draws
	// decision 34's third rung instead of refusing. So the refusal that used to be here is a picture
	// that is *dimmer* than it should be rather than a frame that never reached the glass, which is
	// the trade decision 35 makes everywhere else.
	DrawItem dressed = drawable;
	dressed.Dress = Material::Glass;

	GYRO_CHECK_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &dressed, 1 })).has_value(), true);

	// And the elevation from the same side: a node square to the screen draws at either lifted level,
	// because the shadow it casts is arithmetic over its own rect with nothing read and no pass of its
	// own.
	for (const Elevation level : AllElevations)
	{
		DrawItem lifted = drawable;
		lifted.Lift = Cast(level);

		GYRO_CHECK_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &lifted, 1 })).has_value(), true);
	}

	// **A node spun in the plane draws too, and that is decision 133's whole point.** It is a
	// photograph lying at an angle rather than a card mid-flip: still a rectangle, so the shape turns,
	// the light does not, and there is nothing open about it. Refusing this alongside the tilt would
	// black the screen for an arrangement whose answer nobody disputes.
	DrawItem spun = drawable;
	spun.Lift = Cast(Elevation::Floating);

	for (std::size_t corner = 0; corner < 4; ++corner)
	{
		const float horizontal = corner == 1 || corner == 2 ? 8.0F : -8.0F;
		const float vertical = corner == 2 || corner == 3 ? 8.0F : -8.0F;
		const float across = std::cos(0.4F);
		const float rise = std::sin(0.4F);

		spun.Shape.Corners[corner] = { 16.0F + horizontal * across - vertical * rise,
			                           16.0F + horizontal * rise + vertical * across };
	}

	GYRO_CHECK_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &spun, 1 })).has_value(), true);

	// The branch that used to be here, from the other side: an item whose light is not the output's
	// is drawn rather than refused, because `Prepare` built the variant that converts it.
	DrawItem converted = drawable;
	converted.Color = ColorState::Composite();

	GYRO_CHECK_EQ(fixture->Renderer().Record(Composite(*acquired, damage, { &converted, 1 })).has_value(), true);

	// **A list whose last item is refused records none of the ones in front of it.** The check runs
	// before anything is recorded, so a refusal leaves the target exactly as it was rather than
	// half-composited — which is the same shape `BindTargets` already has and the only one a caller
	// can reason about.
	fixture->Prefill();

	const std::array<DrawItem, 2> mixed{ drawable, refused[0] };
	GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, mixed)).has_value(), false);

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK_EQ(reader.Image().At(16, 16), Prefilled);
}

// The miswirings Seam/RenderTarget.h names, each one a branch somebody wrote rather than a cast that
// happens to work. A Vulkan device handed a CPU mapping is `Blit`'s target set arriving here.
GYRO_TEST(RenderImport, MiswiredTargetSetsAreRefused)
{
	std::optional<Fixture> fixture = Available("MiswiredTargetSetsAreRefused");

	if (!fixture)
	{
		return;
	}

	std::array<std::byte, 64 * 32 * 4> pixels{};
	const RenderTarget mapped{ .Size = Resolution,
		                       .Format = Linear,
		                       .Memory =
		                           MappedImage{ .Pixels = pixels.data(), .Stride = 64 * 4, .Length = pixels.size() } };

	const Result<void> refusedMapping = fixture->Renderer().BindTargets({ &mapped, 1 }, ColorState::Srgb());
	GYRO_REQUIRE_EQ(refusedMapping.has_value(), false);
	GYRO_CHECK_EQ(refusedMapping.error().Code(), EINVAL);
	GYRO_CHECK_EQ(fixture->Renderer().BoundTargets(), 0U);

	// A half-built description, which is what a backend that filled in a size and forgot a format
	// produces. Worth catching where it is consumed rather than as a black screen later.
	const RenderTarget empty{};
	GYRO_CHECK_EQ(fixture->Renderer().BindTargets({ &empty, 1 }, ColorState::Srgb()).has_value(), false);

	// Recording against a target set that was refused names an unbound target, which is the caller's
	// bug and the seam's `EINVAL`.
	const Result<Submission> unbound = fixture->Renderer().Record(Composite(0));
	GYRO_REQUIRE_EQ(unbound.has_value(), false);
	GYRO_CHECK_EQ(unbound.error().Code(), EINVAL);
}

// A set that fails on its *second* target, which is the path that leaks if the bookkeeping is wrong.
//
// **Partial success is not expressible, so the first target's image has to be destroyed too**, and
// the slot the failure happened in is half-built rather than empty — an image created, memory
// possibly imported, no view. It is worth a test of its own because the failure is invisible: the
// call correctly reports `EINVAL`, nothing misbehaves, and a `VkDeviceMemory` holding a duplicated
// descriptor is leaked per attempt until the process runs out of descriptors.
GYRO_TEST(RenderImport, AHalfImportedSetLeavesNothingBehind)
{
	std::optional<Fixture> fixture = Available("AHalfImportedSetLeavesNothingBehind");

	if (!fixture)
	{
		return;
	}

	const std::span<const RenderTarget> targets = fixture->Output().Targets();
	GYRO_REQUIRE(targets.size() >= 2);

	// A descriptor that is a perfectly good file and not a dmabuf. The format is one the device
	// renders into, so `Supports` passes and `vkCreateImage` succeeds — and the import then fails at
	// `vkGetMemoryFdPropertiesKHR`, which is *after* the slot holds a `VkImage`. A format the device
	// refuses outright would not do: that is caught on Import's first line, leaves the slot empty,
	// and would pass whether or not the bookkeeping below is right.
	const Fd notADmabuf{ ::dup(STDERR_FILENO) };
	GYRO_REQUIRE(notADmabuf.IsValid());

	std::array<RenderTarget, 2> mixed{ targets[0], targets[1] };
	DmabufImage* image = std::get_if<DmabufImage>(&mixed[1].Memory);
	GYRO_REQUIRE(image != nullptr);
	image->Planes[0].Descriptor = notADmabuf.Borrow();

	// **Two things are being watched and they need different instruments.** The stranded `VkImage` is
	// heap the sanitizer build sees and an ordinary build does not, so `GYRO_SANITIZE=address` is what
	// actually fails on a regression there. The descriptor count below is the half a plain build can
	// answer, and it guards the other member of the same family: `Import` duplicates the presenter's
	// descriptor because `vkAllocateMemory` consumes it, and every failure path after that `dup` owes
	// a `close`.
	const std::size_t before = OpenDescriptors();

	// Repeated, because one stranded descriptor is invisible and a few hundred is a process that
	// cannot open a file. Well past any per-process soft limit divided by what one attempt strands.
	for (int attempt = 0; attempt < 512; ++attempt)
	{
		const Result<void> refused = fixture->Renderer().BindTargets(mixed, ColorState::Srgb());
		GYRO_REQUIRE_EQ(refused.has_value(), false);
		GYRO_REQUIRE_EQ(refused.error().Code(), EINVAL);
		GYRO_REQUIRE_EQ(fixture->Renderer().BoundTargets(), 0U);
	}

	GYRO_CHECK_EQ(OpenDescriptors(), before);

	// And the renderer is still usable afterwards, which is what says the failures released rather
	// than merely reported.
	GYRO_CHECK_EQ(fixture->Renderer().BindTargets(targets, ColorState::Srgb()).has_value(), true);
}

// Releasing and rebinding, which is what `TargetsInvalidated` and `Reconfigured` do in that order.
// The images have to go before the new set exists, because the descriptors in the old one are the
// presenter's and are about to be closed.
GYRO_TEST(RenderImport, TargetsSurviveAReconfiguration)
{
	std::optional<Fixture> fixture = Available("TargetsSurviveAReconfiguration");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	GYRO_CHECK(fixture->Renderer().BoundTargets() > 0);

	fixture->Renderer().ReleaseTargets();
	GYRO_CHECK_EQ(fixture->Renderer().BoundTargets(), 0U);

	// After release, `Record` refuses until something is bound — the seam says so in as many words.
	GYRO_CHECK_EQ(fixture->Renderer().Record(Composite(0)).has_value(), false);

	fixture->Output().Reconfigure({ .Resolution = { 128, 64 }, .Period = PeriodFromHertz(60.0), .Format = Linear });
	fixture->Output().Advance(fixture->Clock().Now());
	fixture->Output().Advance(fixture->Clock().Now());

	GYRO_REQUIRE(fixture->Output().Status().has_value());

	const std::span<const RenderTarget> reconfigured = fixture->Output().Targets();
	GYRO_REQUIRE(!reconfigured.empty());
	GYRO_CHECK_EQ(reconfigured.front().Size, (PixelSize<DeviceSpace>{ 128, 64 }));
	GYRO_CHECK_EQ(fixture->Renderer().BindTargets(reconfigured, ColorState::Srgb()).has_value(), true);

	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ { 100, 50 }, { 20, 20 } });

	// Deliberately past the target's right and bottom edges, which is what a damage region
	// accumulated across a reconfiguration that shrank an output looks like. Clipped rather than
	// handed to the driver, which would be entitled to reject the whole command buffer.
	GYRO_CHECK_EQ(fixture->Renderer().Record(Composite(*acquired, damage)).has_value(), true);

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	// The rectangle spans x 100..120 and y 50..70, and the target is 64 rows tall — so the bottom
	// eight rows of the damage are off the end. What must happen is that the *rest* still draws:
	// the last row inside the target is composited, and the columns past the rectangle are not.
	const DmabufRead read{ *buffer };
	GYRO_CHECK_EQ(PixelAt(*buffer, 110, 55), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 119, 63), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 100, 63), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 120, 55), Filled);
	GYRO_CHECK_EQ(PixelAt(*buffer, 127, 63), Filled);
	GYRO_CHECK_EQ(PixelAt(*buffer, 50, 20), Filled);
}

// An empty region means nothing changed, which the seam distinguishes from the caller skipping the
// frame. Honouring it costs a submission rather than saving one, and the target must come back
// untouched.
GYRO_TEST(RenderImport, EmptyDamageDrawsNothing)
{
	std::optional<Fixture> fixture = Available("EmptyDamageDrawsNothing");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired));
	GYRO_REQUIRE_EQ(submission.has_value(), true);
	GYRO_CHECK(submission->Point.IsImmediate());

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const DmabufRead read{ *buffer };
	GYRO_CHECK_EQ(PixelAt(*buffer, 32, 16), Filled);
}

// Decision 108, from the caller's side. What is asserted is the *implication* rather than the
// answer: a device that cannot export hands out immediate points, and one that can hands out points
// on its own timeline. Which of those this machine is is the machine's business.
GYRO_TEST(RenderImport, SyncPointsMatchWhatTheDeviceCanExport)
{
	std::optional<Fixture> fixture = Available("SyncPointsMatchWhatTheDeviceCanExport");

	if (!fixture)
	{
		return;
	}

	GYRO_CHECK_EQ(fixture->Renderer().ExportsTimeline(), fixture->Device().Description().ExportsTimeline);

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	if (fixture->Renderer().ExportsTimeline())
	{
		GYRO_CHECK(!submission->Point.IsImmediate());
		GYRO_CHECK(submission->Point.Value > 0);
	}
	else
	{
		// Immediate because the frame is *finished*, not because there was nothing to wait for. The
		// distinction is the whole of decision 108 and the reason the wait is inside `Record`.
		GYRO_CHECK(submission->Point.IsImmediate());
	}

	// Either way the caller's poll agrees, which is what the frame loop reads before starting a
	// frame. A point this renderer never issued answers complete rather than stalling the loop.
	GYRO_CHECK(fixture->Renderer().IsComplete(submission->Point));
	GYRO_CHECK(fixture->Renderer().IsComplete(SyncPoint{ RawFd{ 0 }, 99999 }));

	GYRO_CHECK(submission->RecordCost.count() >= 0);
}

GYRO_TEST(RenderImport, AFrameReportsWhatItCostTheGpu)
{
	std::optional<Fixture> fixture = Available("AFrameReportsWhatItCostTheGpu");

	if (!fixture)
	{
		return;
	}

	CheckTheGpuCostIsReported(*fixture);
}

// **The same claim on a real driver, and here the numbers are the point rather than the plumbing.**
// lavapipe reports a one-nanosecond tick through all sixty-four bits, which is the one configuration
// where reading a timestamp wrong still gives the right answer. A GPU does not: the Intel part this
// was written against ticks every 52.083 nanoseconds and reports thirty-six valid bits, so a raw
// count is off by a factor of fifty-two and a raw subtraction is off by an hour once an hour. This is
// the only test that can tell.
GYRO_TEST(RenderImport, AFrameReportsWhatItCostTheGpuOnHardware)
{
	std::optional<Fixture> fixture = Available("AFrameReportsWhatItCostTheGpuOnHardware", DeviceClass::Hardware);

	if (!fixture)
	{
		return;
	}

	std::println("  {}", fixture->Device().Description());

	CheckTheGpuCostIsReported(*fixture);
}

// The other side of decision 108, on a machine that has one.
//
// **Skipped rather than failed where there is no GPU, and that is the honest shape.** lavapipe can
// never take this branch — it cannot create an exportable semaphore at all — so a suite that only
// ran the floor tier would leave the asynchronous path with no coverage anywhere. This is the test
// that runs on a developer's workstation and in CI only if CI has a device.
GYRO_TEST(RenderImport, AnExportingDeviceHandsOutAWaitablePoint)
{
	std::optional<Fixture> fixture = Available("AnExportingDeviceHandsOutAWaitablePoint", DeviceClass::Hardware);

	if (!fixture)
	{
		return;
	}

	std::println("  {}", fixture->Device().Description());

	if (!fixture->Renderer().ExportsTimeline())
	{
		std::println("  this hardware device exports no timeline either; nothing more to check");

		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());
	fixture->Prefill();

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ { 8, 8 }, { 16, 16 } });

	const Result<Submission> submission = fixture->Renderer().Record(Composite(*acquired, damage));
	GYRO_REQUIRE_EQ(submission.has_value(), true);

	// A real point on a real timeline, which is what a DRM presenter would program as `IN_FENCE_FD`.
	// Seam/SyncPoint.h's premise, holding where the log says it holds.
	GYRO_REQUIRE(!submission->Point.IsImmediate());
	GYRO_CHECK(submission->Point.Timeline.IsValid());
	GYRO_CHECK_EQ(submission->Point.Value, std::uint64_t{ 1 });

	// A poll rather than a wait, which is what the frame loop does. It may not have landed yet, and
	// that is the whole difference from the floor tier — so the loop spins here where a frame thread
	// would go and do something else.
	while (!fixture->Renderer().IsComplete(submission->Point))
	{
		// Deliberately empty. A `Record` on a second target would be the realistic thing to do and is
		// what the frame loop does; here the point is only that the answer eventually flips.
	}

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const DmabufRead read{ *buffer };
	GYRO_CHECK_EQ(PixelAt(*buffer, 16, 16), Black);
	GYRO_CHECK_EQ(PixelAt(*buffer, 40, 24), Filled);

	// The timeline advances rather than restarting, so two composites are two points and a presenter
	// holding the older one is not told the newer one has landed.
	const std::optional<std::uint32_t> second = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(second.has_value());

	const Result<Submission> next = fixture->Renderer().Record(Composite(*second, damage));
	GYRO_REQUIRE_EQ(next.has_value(), true);
	GYRO_CHECK_EQ(next->Point.Value, std::uint64_t{ 2 });
	GYRO_CHECK_EQ(next->Point.Timeline, submission->Point.Timeline);
}

// **Decision 104's shadow, and what has to be true of it is that it has a direction.**
//
// A white field with one lifted node on it. Below the node the field is darker than it was; the same
// distance above, it is darker by less. That difference *is* the one light — a shadow with no offset
// is a glow, and a shadow whose offset came from where the node sits on the screen would be a
// different picture for the same node moved sideways, which is the collage of toolkit shadows decision
// 104 exists to end. Stated as an ordering rather than as numbers because the numbers are
// Seam/Dressing.h's two constants and Docs/Open.md has not tuned them yet.
GYRO_TEST(RenderImport, AShadowFallsBelowTheLiftedNodeAndNotEquallyAbove)
{
	std::optional<Fixture> fixture = Available("AShadowFallsBelowTheLiftedNodeAndNotEquallyAbove");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	// A field to cast onto, because the target is cleared to nothing and a shadow on black is not a
	// picture anybody can check. The node sits above the middle so that the offset has room under it.
	const Rect<DeviceSpace> field{ {}, { 64.0F, 32.0F } };
	const Rect<DeviceSpace> where{ { 24.0F, 8.0F }, { 16.0F, 10.0F } };

	std::array<DrawItem, 2> items{ Solid(field, DrawSolid{ 1.0F, 1.0F, 1.0F, 1.0F }),
		                           Solid(where, DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F }) };
	items[1].Lift = Cast(Elevation::Resting);

	GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, items)).has_value(), true);

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	constexpr Rgba16 White = Rgb8(255, 255, 255);

	// Four pixels outside the node on the axis the light works along, at equal distance either side.
	const Rgba16 below = reader.Image().At(32, 22);
	const Rgba16 above = reader.Image().At(32, 4);

	GYRO_CHECK(below.Red < White.Red);
	GYRO_CHECK(above.Red < White.Red);

	// The whole of the direction claim: nearer the light's own displacement is darker. A shadow drawn
	// centred on the node would make these two equal and pass every other check here.
	GYRO_CHECK(below.Red < above.Red);

	// Grey rather than tinted, because the shadow is premultiplied black — the one fill that needs no
	// colour conversion, and the reason the shadow pipeline has no variant lattice behind it. A
	// conversion applied to it by accident would show up here as a cast.
	GYRO_CHECK_EQ(below.Red, below.Green);
	GYRO_CHECK_EQ(below.Red, below.Blue);

	// And it runs out. The corner is further from the node than Seam/Dressing.h's expansion reaches,
	// so what is left there is below what the target can hold and the field is untouched.
	GYRO_CHECK_EQ(reader.Image().At(0, 31), White);
}

// **A node does not stand on its own shadow**, which decision 104 spends a section on because the
// symptom does not look like elevation. Within one item the material samples the target as of before
// the item began, and a shadow left underneath breaks that from the other side: whatever is drawn over
// it that is not fully opaque — a glass panel, a window mid-fade, decision 34's floored tint — is
// darkened at its own edges by its own shadow.
//
// The node here draws nothing at all, so what is under it is only ever the shadow. Its rect comes back
// exactly as the field was, and the pixels just outside do not.
GYRO_TEST(RenderImport, ALiftedNodeIsNotDarkenedByItsOwnShadow)
{
	std::optional<Fixture> fixture = Available("ALiftedNodeIsNotDarkenedByItsOwnShadow");

	if (!fixture)
	{
		return;
	}

	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
	GYRO_REQUIRE(acquired.has_value());

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, Resolution });

	const Rect<DeviceSpace> field{ {}, { 64.0F, 32.0F } };
	const Rect<DeviceSpace> where{ { 24.0F, 8.0F }, { 16.0F, 12.0F } };

	std::array<DrawItem, 2> items{ Solid(field, DrawSolid{ 1.0F, 1.0F, 1.0F, 1.0F }),
		                           Solid(where, DrawSolid{ 0.0F, 0.0F, 0.0F, 0.0F }) };
	items[1].Lift = Cast(Elevation::Floating);

	GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, items)).has_value(), true);

	const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
	GYRO_REQUIRE(buffer != nullptr);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	constexpr Rgba16 White = Rgb8(255, 255, 255);

	// Inside the node's own rect, well clear of the edge the mask feathers over.
	GYRO_CHECK(reader.Image().IsUniform(PixelRect<DeviceSpace>{ { 26, 10 }, { 12, 8 } }, White));

	// And immediately below it, where the shadow is at its darkest, so that the check above is about
	// the punch-out rather than about a shadow that never drew.
	GYRO_CHECK(reader.Image().At(32, 22).Red < White.Red);
}

namespace
{
// **The shadow written down a second time, in double precision and by a different rule.**
//
// Decision 132 claims the renderer's shadow is exact to a fraction of an eight-bit code point, and a
// claim like that is only worth what it is checked against. This is the same *decomposition* — an
// exact separable rectangle less four corner deficits — evaluated with `std::erf` rather than an
// approximation and with a dense composite Simpson rather than six Gauss-Legendre nodes over two
// panels. What it therefore catches is everything between the arithmetic and the glass: the rule's
// own convergence, the error function's approximation, single precision, the premultiply, the blend,
// and the frame a spun node is resolved into.
struct ShadowModel
{
	double CentreX = 0.0;
	double CentreY = 0.0;
	double HalfWidth = 0.0;
	double HalfHeight = 0.0;

	// The node's own axes on the glass. The identity for a node square to the screen.
	double AcrossX = 1.0;
	double AcrossY = 0.0;
	double DownX = 0.0;
	double DownY = 1.0;

	double Radius = 0.0;
	double Offset = 0.0;
	double Sigma = 1.0;
	double Alpha = 0.0;
};

[[nodiscard]] double Normal(double x)
{
	return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

// One corner's deficit: inside the rectangle's corner, outside the arc, blurred. Composite Simpson
// over the whole quarter arc, which needs no clipping because it is not trying to be cheap.
[[nodiscard]] double ReferenceDeficit(double qx, double qy, const ShadowModel& model)
{
	if (model.Radius <= 0.0)
	{
		return 0.0;
	}

	// Simpson over an integrand that is analytic in the angle, which is the same property decision 132
	// leans on — sixty-four intervals is machine precision here and the reference is still a different
	// rule from the six-node Gauss-Legendre it is checking.
	constexpr int Intervals = 64;

	const double edge = Normal((model.HalfWidth - qx) / model.Sigma);
	const double step = (std::numbers::pi / 2.0) / Intervals;
	double total = 0.0;

	for (int index = 0; index <= Intervals; ++index)
	{
		const double angle = index * step;
		const double row = model.HalfHeight - model.Radius + model.Radius * std::sin(angle);
		const double arc = model.HalfWidth - model.Radius + model.Radius * std::cos(angle);
		const double mass = std::exp(-(row - qy) * (row - qy) / (2.0 * model.Sigma * model.Sigma)) /
		                    (model.Sigma * std::sqrt(2.0 * std::numbers::pi));

		const double weight = index == 0 || index == Intervals ? 1.0 : (index % 2 == 1 ? 4.0 : 2.0);

		total += weight * model.Radius * std::cos(angle) * mass * (edge - Normal((arc - qx) / model.Sigma));
	}

	return total * step / 3.0;
}

// How much light one device pixel loses to this shadow.
[[nodiscard]] double ReferenceDarkening(double px, double py, const ShadowModel& model)
{
	// **The light is taken in device space and the frame afterwards**, which is decision 133: the shape
	// turns with the node and the displacement does not turn with it.
	const double offsetX = px - model.CentreX;
	const double offsetY = py - model.CentreY - model.Offset;

	const double x = offsetX * model.AcrossX + offsetY * model.AcrossY;
	const double y = offsetX * model.DownX + offsetY * model.DownY;

	double coverage = (Normal((model.HalfWidth - x) / model.Sigma) - Normal((-model.HalfWidth - x) / model.Sigma)) *
	                  (Normal((model.HalfHeight - y) / model.Sigma) - Normal((-model.HalfHeight - y) / model.Sigma));

	for (const int horizontal : { 1, -1 })
	{
		for (const int vertical : { 1, -1 })
		{
			coverage -= ReferenceDeficit(horizontal * x, vertical * y, model);
		}
	}

	return model.Alpha * std::clamp(coverage, 0.0, 1.0);
}

// Whether a device point is far enough outside the node that the punch-out is not part of the answer.
[[nodiscard]] bool Outside(double px, double py, const ShadowModel& model, double margin)
{
	const double offsetX = px - model.CentreX;
	const double offsetY = py - model.CentreY;

	const double x = std::abs(offsetX * model.AcrossX + offsetY * model.AcrossY);
	const double y = std::abs(offsetX * model.DownX + offsetY * model.DownY);

	return x > model.HalfWidth + margin || y > model.HalfHeight + margin;
}
} // namespace

// **Decision 132's claim, held to a number.** A white field with one lifted node on it, sampled
// everywhere the shadow reaches and compared against the model above.
//
// Two nodes, and the second is the one that would have gone unnoticed: the first is square to the
// screen, and the second is spun in the plane, which decision 133 draws rather than refuses. Matching
// one reference in both configurations is what says the shape turned and the light did not — a
// renderer that swung the light round with the node passes every test that only looks at a node
// square to the screen.
GYRO_TEST(RenderImport, TheShadowMatchesItsClosedFormToWithinACodePoint)
{
	std::optional<Fixture> fixture = Available("TheShadowMatchesItsClosedFormToWithinACodePoint");

	if (!fixture)
	{
		return;
	}

	constexpr PixelSize<DeviceSpace> Wide{ 256, 160 };

	fixture->Renderer().ReleaseTargets();
	fixture->Output().Reconfigure({ .Resolution = Wide, .Period = PeriodFromHertz(60.0), .Format = Linear });
	fixture->Output().Advance(fixture->Clock().Now());
	fixture->Output().Advance(fixture->Clock().Now());

	GYRO_REQUIRE(fixture->Output().Status().has_value());
	GYRO_REQUIRE(fixture->Renderer().BindTargets(fixture->Output().Targets(), ColorState::Srgb()).has_value());

	// A radius that is a real fraction of the node, so the corner deficits are most of what is being
	// checked rather than a rounding on the end of a rectangle.
	constexpr float Radius = 16.0F;
	constexpr float HalfWidth = 44.0F;
	constexpr float HalfHeight = 30.0F;

	for (const float angle : { 0.0F, 0.5F })
	{
		const float across = std::cos(angle);
		const float rise = std::sin(angle);
		const Shadow lift = Cast(Elevation::Resting);

		const ShadowModel model{ .CentreX = 128.0,
			                     .CentreY = 78.0,
			                     .HalfWidth = HalfWidth,
			                     .HalfHeight = HalfHeight,
			                     .AcrossX = across,
			                     .AcrossY = rise,
			                     .DownX = -rise,
			                     .DownY = across,
			                     .Radius = Radius,
			                     .Offset = lift.Offset,
			                     .Sigma = lift.Softness,
			                     .Alpha = lift.Opacity };

		Region<DeviceSpace> damage;
		damage.Add(PixelRect<DeviceSpace>{ {}, Wide });

		// The node draws nothing of its own, so every pixel outside it is the shadow and the field and
		// nothing else. Its quad is built by hand because this is the one test that wants a projection
		// the evaluator has no way to author yet.
		std::array<DrawItem, 2> items{
			Solid({ {}, { 256.0F, 160.0F } }, DrawSolid{ 1.0F, 1.0F, 1.0F, 1.0F }),
			Solid({ {}, { 2.0F * HalfWidth, 2.0F * HalfHeight } }, DrawSolid{ 0.0F, 0.0F, 0.0F, 0.0F }, 1.0F, Radius)
		};

		const float horizontal[4]{ -HalfWidth, HalfWidth, HalfWidth, -HalfWidth };
		const float vertical[4]{ -HalfHeight, -HalfHeight, HalfHeight, HalfHeight };

		for (std::size_t corner = 0; corner < 4; ++corner)
		{
			items[1].Shape.Corners[corner] = {
				static_cast<float>(model.CentreX) + horizontal[corner] * across - vertical[corner] * rise,
				static_cast<float>(model.CentreY) + horizontal[corner] * rise + vertical[corner] * across
			};
		}

		items[1].Lift = lift;

		const std::optional<std::uint32_t> acquired = fixture->Output().AcquireTarget();
		GYRO_REQUIRE(acquired.has_value());
		GYRO_REQUIRE_EQ(fixture->Renderer().Record(Composite(*acquired, damage, items)).has_value(), true);

		const DmabufBuffer* buffer = fixture->Output().Buffer(*acquired);
		GYRO_REQUIRE(buffer != nullptr);

		const BufferReader reader{ *buffer };
		GYRO_REQUIRE(reader.IsValid());

		double worst = 0.0;
		PixelPoint<DeviceSpace> where{};
		std::size_t sampled = 0;

		for (std::int32_t y = 0; y < Wide.Height; ++y)
		{
			for (std::int32_t x = 0; x < Wide.Width; ++x)
			{
				// The pixel's own centre, which is what the fragment stage was asked about, and a margin
				// clear of the node so that the punch-out's one-pixel feather is not in the comparison.
				const double px = x + 0.5;
				const double py = y + 0.5;

				if (!Outside(px, py, model, 1.0))
				{
					continue;
				}

				const double expected = ReferenceDarkening(px, py, model);
				const double actual = 1.0 - reader.Image().At(x, y).Red / 65535.0;
				const double apart = std::abs(actual - expected) * 255.0;

				++sampled;

				if (apart > worst)
				{
					worst = apart;
					where = { x, y };
				}
			}
		}

		GYRO_REQUIRE(sampled > 1000);
		std::printf(
			"  %s: worst %.3f code points at (%d, %d) over %zu pixels\n",
			angle == 0.0F ? "square to the screen" : "spun in the plane",
			worst,
			where.X,
			where.Y,
			sampled
		);

		// One code point, which is a quantised eight-bit target's own step: the shadow is as exact as
		// the buffer it lands in can record. Decision 132's measurement is the claim and this is where
		// it stops being one.
		GYRO_CHECK(worst <= 1.0);
	}
}
