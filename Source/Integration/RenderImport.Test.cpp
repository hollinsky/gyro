#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Renderer.h"
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
		  m_Renderer{ m_Clock, m_Vulkan }
	{}

	[[nodiscard]] VirtualOutput& Output() noexcept { return m_Output; }

	[[nodiscard]] VulkanRenderer& Renderer() noexcept { return m_Renderer; }

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
[[nodiscard]] RecordRequest
Composite(std::uint32_t target, const Region<DeviceSpace>& damage = {}, std::span<const DrawItem> items = {})
{
	return RecordRequest{
		.Target = target, .Mode = RenderMode::Planned, .CostGeneration = 0, .Damage = damage, .Items = items
	};
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
		             .Lift = Elevation::None,
		             .Color = ColorState::Srgb(),
		             .Sampling = {} };
}

// Everything the target holds, decoded. `BufferReader` is what holds the kernel's cache maintenance
// open across the look, which is why the view is never taken over `Pixels()` directly.
constexpr Rgba16 Red = Rgb8(255, 0, 0);
constexpr Rgba16 Blue = Rgb8(0, 0, 255);
constexpr Rgba16 Prefilled = Rgb8(0xAB, 0xAB, 0xAB);

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
} // namespace

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

	const DmabufBuffer::CpuRead read{ *buffer };

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

	const DmabufBuffer::CpuRead read{ *buffer };
	GYRO_CHECK_EQ(PixelAt(*buffer, 32, 16), Black);

	fixture->Output().Release(presented->Target);
	GYRO_CHECK(!fixture->Output().PresentedFrame().has_value());
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

	std::array<DrawItem, 5> refused{ drawable, drawable, drawable, drawable, drawable };
	refused[0].Content = DrawTexture{};
	refused[1].Content = DrawGroup{ .Count = 0 };
	refused[2].Dress = Material::Glass;
	refused[3].Lift = Elevation::Resting;

	// **The colour-state refusal is now one transfer function rather than every conversion**, and HLG
	// is the one because converting it needs a display peak luminance `ColorState` does not carry.
	// Everything else — a solid authored in linear light at wide primaries, drawn onto an sRGB
	// output — is a specialization constant and a matrix that exist, and the check below is that it
	// draws rather than that it is refused.
	refused[4].Color = ColorState{ ColorPrimaries::Bt2020, TransferFunction::Hlg, AlphaMode::Premultiplied, 0, 203.0F };

	for (const DrawItem& item : refused)
	{
		const Result<Submission> answer = fixture->Renderer().Record(Composite(*acquired, damage, { &item, 1 }));
		GYRO_REQUIRE_EQ(answer.has_value(), false);
		GYRO_CHECK_EQ(answer.error().Code(), EINVAL);
	}

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
	const DmabufBuffer::CpuRead read{ *buffer };
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

	const DmabufBuffer::CpuRead read{ *buffer };
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

	// The GPU half of `C` is not measured yet, and a stub that invented one would be worse than a
	// gap. Frame/Budget.h reads what it is given.
	std::array<GpuCost, 4> costs{};
	GYRO_CHECK_EQ(fixture->Renderer().CollectCosts(costs), std::size_t{ 0 });
	GYRO_CHECK(submission->RecordCost >= Duration::zero());
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

	const DmabufBuffer::CpuRead read{ *buffer };
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
