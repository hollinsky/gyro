#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "Blit/Blit.h"
#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Pixel.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Testing/Test.h"
#include "Virtual/Buffer.h"
#include "Virtual/Device.h"
#include "Virtual/Heap.h"
#include "Virtual/Output.h"
#include "Virtual/Pam.h"
#include "Virtual/Pixels.h"
#include "Virtual/Sink.h"
#include "World/Elevation.h"
#include "World/Material.h"

// The CPU renderer against a real presenter's images, all the way to a consumer.
//
// **It is here rather than in either module because neither may name the other.** `Blit` is a
// renderer and `Virtual` is a presenter; the composition root is what wires them, and
// Docs/Structure.md's graph has no edge in either direction. This is the file
// [decision 110](../../Docs/Decisions.md) names as owed — the pairing
// RenderImport.Test.cpp is for the Vulkan renderer, done for the floor beneath it.
//
// **What this covers that neither of the two files either side of it does.** Blit.Test.cpp drives
// `Record` by hand against memory it owns: a `RenderTarget` the test wrote, one target, a mapping
// whose length is exactly the image, and nothing on the far side. VirtualComposite.Test.cpp drives
// a real presenter through the frame loop, but with the flat sketch painter in front of it, and
// decision 110 keeps it that way on purpose — its four tests are about frame delivery, so a
// coverage-antialiased edge would fail assertions that are about neither. Between them is the seam:
// **a renderer that refuses dmabufs, bound to a presenter that can hand out either face of the same
// image**, drawing into a ring it does not own, across a reconfiguration that unmaps the memory it
// is holding.
//
// **It runs everywhere, and for `Blit` that is not a convenience.** The allocator is
// `HeapAllocator` and the renderer is the CPU one, so nothing here needs a GPU, a seat, a Vulkan
// ICD or `/dev/udmabuf` — which is what makes both gates in RenderImport.Test.cpp unnecessary. This
// is the renderer that runs when there is nothing else to run, so a test of it that skips on the
// machine with no GPU would skip on exactly the machine it exists for.
//
// **Nothing here is a golden image**, for Virtual/Pixels.h's reason. What is asserted is where an
// edge landed, that an interior is exactly the colour asked for, that what was outside the damage
// did not move, and which image of the ring a frame arrived in.

namespace
{
constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelSize<DeviceSpace> Resolution{ 64, 32 };
constexpr Duration Period = PeriodFromHertz(60.0);

constexpr Rgba16 Red = Rgb8(255, 0, 0);
constexpr Rgba16 Blue = Rgb8(0, 0, 255);

// What a mapping is filled with before a composite runs. Any value the renderer would not produce
// works; this one differs in every channel from the opaque black an empty damage rectangle clears
// to, so a partial write is visible rather than half-right.
constexpr std::byte Untouched{ 0xAB };

// A solid over a rectangle, every field named. `-Wmissing-field-initializers` wants all of them and
// it is the right rule anyway: an item with a default nobody chose is how a node ends up invisible
// or opaque by accident.
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

[[nodiscard]] DrawSolid Colour(float red, float green, float blue)
{
	return DrawSolid{ .Red = red, .Green = green, .Blue = blue, .Alpha = 1.0F };
}

// One request, every field named. `-Wmissing-field-initializers` wants all of them and the callers
// want to vary two, so the naming happens once here.
[[nodiscard]] RecordRequest
Composite(std::uint32_t target, const Region<DeviceSpace>& damage = {}, std::span<const DrawItem> items = {})
{
	return RecordRequest{
		.Target = target,
		.Mode = RenderMode::Planned,
		.CostGeneration = 0,
		.Damage = damage,
		.Items = items,
		.Captures = {}
	};
}

[[nodiscard]] Region<DeviceSpace> Whole(PixelSize<DeviceSpace> size)
{
	Region<DeviceSpace> region;
	region.Add(PixelRect<DeviceSpace>{ {}, size });

	return region;
}

// Everything a composition root wires together for a machine with no GPU, held in one object so a
// test body is about what was drawn.
//
// Built in place rather than returned, because both an `IRenderer` and an `IPresenter` are neither
// copyable nor movable — they own signals and observers hold their addresses.
class Fixture
{
public:
	// The sink comes from the caller because the two consumers below want different things: a
	// capture is sized once at construction from one resolution, so the reconfiguration test cannot
	// use one and reads the presenter's buffer directly instead.
	explicit Fixture(
		IFrameSink& sink,
		PixelSize<DeviceSpace> resolution = Resolution,
		std::uint32_t targets = DefaultVirtualTargets
	)
	{
		const OutputConfiguration configuration{ .Resolution = resolution, .Period = Period, .Format = Linear };

		// The one part of the policy this file never varies. A CPU renderer takes the mapped face of
		// the same images a Vulkan one would import as dmabufs — see Virtual/Output.h's `TargetFace`
		// — and the *other* face of those same images is what the second test below reaches for.
		//
		// The renderer is handed to the device as the completion oracle: decision 108 has a device
		// that cannot export a timeline finish inside `Record`, and this is the one that never had a
		// timeline to export, so every point is immediate and no frame ever waits a drain.
		m_Output = m_Device.Add(
			configuration,
			m_Allocator,
			sink,
			&m_Blit,
			VirtualOutputPolicy{ .Targets = targets, .Face = TargetFace::Mapped }
		);

		GYRO_REQUIRE(m_Output != nullptr);
		GYRO_REQUIRE(m_Output->Status().has_value());

		m_OnInvalidated.ConnectTo<&Fixture::OnTargetsInvalidated>(m_Output->TargetsInvalidated, *this);
	}

	[[nodiscard]] VirtualOutput& Output() noexcept { return *m_Output; }

	[[nodiscard]] Blit& Blitter() noexcept { return m_Blit; }

	[[nodiscard]] VirtualDevice& Device() noexcept { return m_Device; }

	[[nodiscard]] ManualClock& Clock() noexcept { return m_Clock; }

	// How many times the presenter has told the renderer to let go.
	[[nodiscard]] std::uint64_t Releases() const noexcept { return m_Releases; }

	[[nodiscard]] Result<void> Bind() { return m_Blit.BindTargets(m_Output->Targets(), ColorState::Srgb()); }

	// Every image's mapping set to a value the renderer will not write, so that "was not drawn into"
	// is a claim the bytes can answer. Unlike RenderImport.Test.cpp's, this may run before the bind:
	// there is no import and no layout transition, so nothing is entitled to discard what an image
	// held.
	void Prefill()
	{
		for (std::uint32_t index = 0; index < m_Output->Targets().size(); ++index)
		{
			if (const DmabufBuffer* buffer = m_Output->Buffer(index); buffer != nullptr)
			{
				std::ranges::fill(buffer->Pixels(), Untouched);
			}
		}
	}

	// One frame, the way a composition root takes one: acquire an image, composite the damage into
	// it, hand it to the presenter, and let the clock reach the boundary the commit was latched to
	// so the consumer is called. False at the first step that refuses.
	[[nodiscard]] bool Frame(std::span<const DrawItem> items, const Region<DeviceSpace>& damage)
	{
		const std::optional<std::uint32_t> acquired = m_Output->AcquireTarget();

		if (!acquired)
		{
			return false;
		}

		const Result<Submission> submission = m_Blit.Record(Composite(*acquired, damage, items));

		if (!submission)
		{
			return false;
		}

		// Every field named, for `Solid`'s reason: a layer with a default nobody chose is how a plane
		// ends up blending when it should not.
		const PresentLayer layer{ .Target = *acquired,
			                      .Blend = BlendMode::Opaque,
			                      .Reserved = {},
			                      .Acquire = submission->Point,
			                      .Source = {},
			                      .Destination = PixelRect<DeviceSpace>{ {}, m_Output->Configuration().Resolution },
			                      .Damage = damage,
			                      .Color = ColorState::Srgb() };

		if (!m_Output->Present({ &layer, 1 }))
		{
			return false;
		}

		// Set to the boundary rather than advanced by a period, so that a test never has to know how
		// the timeline rounds a commit onto it.
		m_Clock.Set(m_Output->NextEvent());

		return m_Device.Drain().has_value();
	}

private:
	// What a composition root does with `TargetsInvalidated`, and it has to happen here rather than
	// after the new set exists.
	//
	// **A renderer that missed this does not draw a wrong picture.** The descriptions `Blit` is
	// holding are raw pointers into a mapping the presenter `munmap`s on the next line, so the next
	// `Record` writes through a dangling one — a crash on a good day and somebody else's memory on a
	// bad one. That is the whole reason the seam has two signals rather than one, and it is why the
	// reconfiguration test below is worth running under `GYRO_SANITIZE=address`.
	void OnTargetsInvalidated()
	{
		m_Blit.ReleaseTargets();
		++m_Releases;
	}

	ManualClock m_Clock{};
	HeapAllocator m_Allocator{};
	Blit m_Blit{ m_Clock };
	VirtualDevice m_Device{ m_Clock };
	VirtualOutput* m_Output = nullptr;

	std::uint64_t m_Releases = 0;
	Connection<> m_OnInvalidated{};
};

// A picture on disk when an assertion about one fails, and only then. Virtual/Pixels.h has the
// argument for why nothing compares against it.
void DumpOnFailure(const ImageView& image, std::uint64_t sequence)
{
	if (const std::string_view directory = FrameDumpDirectory(); !directory.empty())
	{
		(void)DumpFrame(image, directory, sequence);
	}
}
} // namespace

// The whole crossing: a composite the CPU wrote into an image the presenter allocated, read back by
// a consumer through the codec the renderer encoded with.
GYRO_TEST(BlitPresent, ACompositeReachesTheConsumerThroughTheMappedFace)
{
	CapturingSink sink{ Resolution, Linear };
	Fixture fixture{ sink };

	fixture.Prefill();
	GYRO_REQUIRE(fixture.Bind().has_value());

	const std::array<DrawItem, 2> items{ Solid({ { 4.0F, 4.0F }, { 16.0F, 8.0F } }, Colour(1.0F, 0.0F, 0.0F)),
		                                 Solid({ { 40.0F, 20.0F }, { 8.0F, 8.0F } }, Colour(0.0F, 0.0F, 1.0F)) };

	GYRO_REQUIRE(fixture.Frame(items, Whole(Resolution)));

	// In the same drain the frame was presented in, which is decision 108's immediate point arriving
	// where it matters: `Blit` finishes on the CPU before `Record` returns, so Virtual/Device.h's
	// completion gate never holds a frame back and the consumer pays no extra period.
	GYRO_REQUIRE_EQ(sink.Copied(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(sink.Skipped(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(fixture.Device().Awaiting(), std::size_t{ 0 });

	const CapturingSink::Capture capture = sink.Newest();
	GYRO_REQUIRE(capture.Image.IsValid());

	DumpOnFailure(capture.Image, capture.Frame.Sequence);

	// Opaque and to the bit. A whole-pixel rectangle at full opacity has coverage of exactly one
	// everywhere it covers, which is what makes this an equality rather than a tolerance — and the
	// round trip through the target's encoding is lossless at the ends of the range.
	GYRO_CHECK(capture.Image.IsUniform(PixelRect<DeviceSpace>{ { 4, 4 }, { 16, 8 } }, Red));
	GYRO_CHECK(capture.Image.IsUniform(PixelRect<DeviceSpace>{ { 40, 20 }, { 8, 8 } }, Blue));

	// One pixel past each edge is the clear, checked at the boundary rather than from a distance: an
	// off-by-one in the span arithmetic is invisible from the middle of a rectangle.
	GYRO_CHECK_EQ(capture.Image.At(3, 4), OpaqueBlack);
	GYRO_CHECK_EQ(capture.Image.At(20, 4), OpaqueBlack);
	GYRO_CHECK_EQ(capture.Image.At(4, 3), OpaqueBlack);
	GYRO_CHECK_EQ(capture.Image.At(4, 12), OpaqueBlack);

	// And the image the consumer read is one the presenter owns rather than one the renderer kept:
	// released here, free again, and the ring can turn.
	GYRO_CHECK_EQ(fixture.Output().FreeTargets(), DefaultVirtualTargets);
}

// The seam, in one test: a `udmabuf` image is honestly both a descriptor and a mapping, and which
// face an output shows is the presenter's to decide. Handed the wrong one, this renderer says so.
//
// Virtual/Buffer.h calls the two faces *one of two true answers*, and Seam/RenderTarget.h carries
// discriminated memory precisely so that the mismatch is a branch somebody wrote. It is worth
// asserting against the *same image* rather than against a hand-built descriptor, because what a
// miswired composition root actually produces is this: the right buffer, described the other way.
GYRO_TEST(BlitPresent, TheOtherFaceOfTheSameImageIsRefused)
{
	DiscardingSink sink;
	Fixture fixture{ sink };

	const DmabufBuffer* buffer = fixture.Output().Buffer(0);
	GYRO_REQUIRE(buffer != nullptr);

	const RenderTarget dmabuf = buffer->Describe();
	const RenderTarget mapped = buffer->DescribeMapped();

	// One image, so the two descriptions agree about everything except how the memory is reached.
	GYRO_REQUIRE_EQ(dmabuf.Size, mapped.Size);
	GYRO_REQUIRE_EQ(dmabuf.Format, mapped.Format);
	GYRO_REQUIRE(dmabuf.AsMapped() == nullptr);
	GYRO_REQUIRE(mapped.AsMapped() != nullptr);

	const Result<void> refused = fixture.Blitter().BindTargets({ &dmabuf, 1 }, ColorState::Srgb());
	GYRO_REQUIRE_EQ(refused.has_value(), false);
	GYRO_CHECK_EQ(refused.error().Code(), EINVAL);

	// And nothing is bound afterwards, so a caller that ignored the error records into nothing rather
	// than into whatever the last successful bind left behind.
	GYRO_CHECK_EQ(fixture.Blitter().Record(Composite(0)).has_value(), false);

	// The set the presenter was actually configured to hand out is the other face of those same
	// images, and it draws.
	fixture.Prefill();
	GYRO_REQUIRE(fixture.Bind().has_value());

	const std::array<DrawItem, 1> items{ Solid({ { 8.0F, 8.0F }, { 8.0F, 8.0F } }, Colour(1.0F, 0.0F, 0.0F)) };
	GYRO_REQUIRE(fixture.Frame(items, Whole(Resolution)));

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK(reader.Image().IsUniform(PixelRect<DeviceSpace>{ { 8, 8 }, { 8, 8 } }, Red));
}

// The presenter's mapping is longer than the image in it, and the renderer writes rows rather than
// bytes.
//
// **This is the claim Blit.Test.cpp structurally cannot make.** Its `Surface` allocates exactly
// `height * stride`, so a composite that ran off the end of the picture would be a heap overflow
// that only a sanitizer build reports. A real allocator rounds to a page — Virtual/Udmabuf.h's
// `UdmabufImageBytes`, which the heap provider shares so the stand-in and the kernel cannot disagree
// about a stride — so the memory past the last row is there, is legal to write, and must not be
// touched. A renderer that derived its extent from `MappedImage::Length` rather than from the
// target's size would pass every assertion in that file and fail this one.
GYRO_TEST(BlitPresent, NothingIsWrittenPastTheImageInsideTheMapping)
{
	// Chosen so that the image does not fill a whole number of pages on any page size a Linux
	// machine has: 60 * 4 * 30 is 7,200 bytes.
	constexpr PixelSize<DeviceSpace> Odd{ 60, 30 };

	DiscardingSink sink;
	Fixture fixture{ sink, Odd };

	const DmabufBuffer* buffer = fixture.Output().Buffer(0);
	GYRO_REQUIRE(buffer != nullptr);

	const std::size_t image = static_cast<std::size_t>(buffer->Stride()) * static_cast<std::size_t>(Odd.Height);
	const std::span<std::byte> pixels = buffer->Pixels();

	GYRO_REQUIRE(pixels.size() > image);

	fixture.Prefill();
	GYRO_REQUIRE(fixture.Bind().has_value());

	// A solid over the whole output, so every row of the picture is written and the composite has
	// every reason to keep going.
	const std::array<DrawItem, 1> items{ Solid({ {}, { 60.0F, 30.0F } }, Colour(1.0F, 0.0F, 0.0F)) };
	GYRO_REQUIRE(fixture.Frame(items, Whole(Odd)));

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	// The last row of the picture is painted, which is what makes the next assertion about a bound
	// rather than about a composite that stopped early.
	GYRO_CHECK(reader.Image().IsUniform(PixelRect<DeviceSpace>{ { 0, 29 }, { 60, 1 } }, Red));

	std::size_t written = 0;

	for (std::size_t offset = image; offset < pixels.size(); ++offset)
	{
		written += pixels[offset] == Untouched ? 0U : 1U;
	}

	GYRO_CHECK_EQ(written, std::size_t{ 0 });
}

// The images go away and come back, which is what `TargetsInvalidated` and `Reconfigured` do in that
// order — and on this pair the ordering is not a tidiness argument.
//
// The descriptions `Blit` holds are pointers into a mapping the presenter unmaps, so a renderer that
// did not release on the invalidation would be writing through a dangling pointer on the next frame.
// The fixture wires the signal the way a composition root does; what this checks is that the
// renderer came back — a new set, a different size, and a composite on the new grid.
GYRO_TEST(BlitPresent, TheTargetSetGoesAwayAndComesBackAcrossAReconfiguration)
{
	DiscardingSink sink;
	Fixture fixture{ sink };

	fixture.Prefill();
	GYRO_REQUIRE(fixture.Bind().has_value());

	const std::array<DrawItem, 1> before{ Solid({ { 4.0F, 4.0F }, { 8.0F, 8.0F } }, Colour(1.0F, 0.0F, 0.0F)) };
	GYRO_REQUIRE(fixture.Frame(before, Whole(Resolution)));
	GYRO_REQUIRE_EQ(sink.Frames(), std::uint64_t{ 1 });

	constexpr PixelSize<DeviceSpace> Smaller{ 32, 16 };

	fixture.Output().Reconfigure({ .Resolution = Smaller, .Period = Period, .Format = Linear });
	fixture.Output().Advance(fixture.Clock().Now());

	// The renderer was told exactly once, and it was told while the old descriptions were still the
	// only thing it had — a second release would mean the invalidation fired for a set that had
	// already gone, and none at all would mean it is still holding the unmapped one.
	GYRO_REQUIRE_EQ(fixture.Releases(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(fixture.Blitter().Record(Composite(0)).has_value(), false);

	const std::span<const RenderTarget> reconfigured = fixture.Output().Targets();
	GYRO_REQUIRE(fixture.Output().Status().has_value());
	GYRO_REQUIRE(!reconfigured.empty());
	GYRO_CHECK_EQ(reconfigured.front().Size, Smaller);

	fixture.Prefill();
	GYRO_REQUIRE(fixture.Bind().has_value());

	// A damage rectangle accumulated before the output shrank, which is what a caller that has not
	// noticed the reconfiguration hands over. Clipped rather than written past the new image.
	Region<DeviceSpace> stale;
	stale.Add(PixelRect<DeviceSpace>{ { 8, 4 }, { 40, 24 } });

	const std::array<DrawItem, 1> after{ Solid({ { 8.0F, 4.0F }, { 16.0F, 8.0F } }, Colour(0.0F, 0.0F, 1.0F)) };
	GYRO_REQUIRE(fixture.Frame(after, stale));
	GYRO_CHECK_EQ(sink.Frames(), std::uint64_t{ 2 });

	const DmabufBuffer* buffer = fixture.Output().Buffer(0);
	GYRO_REQUIRE(buffer != nullptr);
	GYRO_REQUIRE_EQ(buffer->Size(), Smaller);

	const BufferReader reader{ *buffer };
	GYRO_REQUIRE(reader.IsValid());

	DumpOnFailure(reader.Image(), 0);

	// Drawn on the new grid, cleared where the clipped damage reached, and untouched outside it —
	// which is also what says the mapping being written is the new one rather than the old.
	GYRO_CHECK(reader.Image().IsUniform(PixelRect<DeviceSpace>{ { 8, 4 }, { 16, 8 } }, Blue));
	GYRO_CHECK_EQ(reader.Image().At(24, 4), OpaqueBlack);
	GYRO_CHECK_EQ(reader.Image().At(31, 15), OpaqueBlack);
	GYRO_CHECK_EQ(reader.Image().At(7, 4), Rgb8(0xAB, 0xAB, 0xAB));
	GYRO_CHECK_EQ(reader.Image().At(8, 3), Rgb8(0xAB, 0xAB, 0xAB));
}

// The ring turning, with a renderer whose output can tell one frame from another.
//
// **This is what the pairing buys over either file alone.** VirtualComposite.Test.cpp publishes one
// scene and checks that four frames arrive; every image in its ring holds the same picture, so a
// renderer that drew into the wrong one, or bound the first target and kept using it, would pass.
// Here each frame moves the panel, so the fourth frame lands in the image the first one used and has
// to hold the fourth frame's picture — the target index the presenter handed out and the memory the
// renderer wrote have to be the same image, three frames apart.
GYRO_TEST(BlitPresent, EveryImageInTheRingCarriesItsOwnFrame)
{
	constexpr std::uint32_t Depth = 3;
	constexpr int Frames = 6;

	CapturingSink sink{ Resolution, Linear, Frames };
	Fixture fixture{ sink, Resolution, Depth };

	fixture.Prefill();
	GYRO_REQUIRE(fixture.Bind().has_value());
	GYRO_REQUIRE_EQ(fixture.Output().Targets().size(), Depth);

	for (int frame = 0; frame < Frames; ++frame)
	{
		const std::array<DrawItem, 1> items{
			Solid({ { 4.0F + 8.0F * static_cast<float>(frame), 8.0F }, { 8.0F, 8.0F } }, Colour(1.0F, 0.0F, 0.0F))
		};

		GYRO_REQUIRE(fixture.Frame(items, Whole(Resolution)));
	}

	GYRO_REQUIRE_EQ(sink.Copied(), std::uint64_t{ Frames });
	GYRO_REQUIRE_EQ(sink.Retained(), std::size_t{ Frames });

	std::uint64_t previous = 0;

	for (std::size_t index = 0; index < sink.Retained(); ++index)
	{
		const CapturingSink::Capture capture = sink.At(index);
		GYRO_REQUIRE(capture.Image.IsValid());

		DumpOnFailure(capture.Image, capture.Frame.Sequence);

		// The ring cycles, so this frame's image is the one three frames ago used. Stated rather than
		// inferred, because it is the premise the next assertion rests on: without it, six frames into
		// six distinct images would pass too.
		GYRO_CHECK_EQ(capture.Frame.Target, static_cast<std::uint32_t>(index % Depth));

		if (index > 0)
		{
			GYRO_CHECK(capture.Frame.Sequence > previous);
		}

		previous = capture.Frame.Sequence;

		const std::int32_t left = 4 + 8 * static_cast<std::int32_t>(index);

		// This frame's panel, where this frame put it — and nothing left of it, which is where every
		// earlier frame's panel was. An image that came back holding a stale composite fails on the
		// second of those rather than the first.
		GYRO_CHECK(capture.Image.IsUniform(PixelRect<DeviceSpace>{ { left, 8 }, { 8, 8 } }, Red));
		GYRO_CHECK_EQ(
			capture.Image.BoundsOfDiffering(OpaqueBlack),
			std::optional{ PixelRect<DeviceSpace>{ { left, 8 }, { 8, 8 } } }
		);
	}
}
