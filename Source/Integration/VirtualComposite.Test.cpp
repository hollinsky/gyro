#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Frame/Evaluator.h"
#include "Frame/Loop.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Seam/EventSource.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"
#include "Virtual/Device.h"
#include "Virtual/Heap.h"
#include "Virtual/Output.h"
#include "Virtual/Pam.h"
#include "Virtual/Pixels.h"
#include "Virtual/Sink.h"
#include "World/Content.h"
#include "World/Node.h"

// The whole path, from a scene the dispatch side published to the bytes a consumer reads.
//
// **What this covers that nothing else does.** SceneRoundTrip.Test.cpp stops at the draw list;
// RenderImport.Test.cpp starts at the renderer and drives the presenter by hand; Schedulability.
// Test.cpp runs the loop against a renderer that draws nothing. Between them is the part that has
// never been run end to end: a snapshot published on one side becoming pixels a consumer can read on
// the other, through `FrameLoop` — acquisition, evaluation, recording, presentation, the flip
// completing, and a consumer releasing so that the ring turns again. Every one of those is a place a
// frame can be lost, and until this file none of them was checked by looking at what was drawn.
//
// **It names `Frame`, `Publication`, `World`, and `Virtual`, which no module may**, so it lives here
// for the reason the publication soak beside it does.
//
// **It runs everywhere, and that is a requirement rather than a bonus.** The allocator is
// `HeapAllocator` and the renderer is a CPU blitter, so nothing here needs `/dev/udmabuf`, a GPU, a
// seat, or a Vulkan ICD. Docs/Open.md's *how `Virtual`'s tests are gated* entry is about the tests
// that genuinely cannot avoid the kernel; this file is the argument that most of the interesting
// path is not among them.

namespace
{
constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelSize<DeviceSpace> Resolution{ 64, 32 };
constexpr Duration Period = PeriodFromHertz(60.0);

constexpr Rgba16 Background = Rgb8(0, 0, 0);
constexpr Rgba16 Red = Rgb8(255, 0, 0);
constexpr Rgba16 Blue = Rgb8(0, 0, 255);

// A composite done on the CPU, into the mapping the presenter handed over.
//
// **This is where `Blit` goes.** Decision 79 puts a CPU renderer in front of the boot console —
// Seam/Renderer.h is written against it as the first of the two implementations that shaped the
// interface — and what is below is the shape of its first hundred lines: bind a `MappedImage`,
// refuse a dmabuf, clear the damage, paint the list in order, and hand back an immediate point.
// When that module lands, this class is deleted and its type name is substituted; nothing else in
// this file changes, which is the property that makes it worth writing this way rather than
// reaching into the buffers directly.
//
// What it deliberately does not do is anything a real blitter would have to: no perspective, no
// rounded corners, no dressings, no group offscreens, and axis-aligned bounds instead of quads. It
// paints what the assertions below read, and the parts it skips are the parts `Blit` exists to
// argue about.
class SketchRenderer final : public IRenderer
{
public:
	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets) override
	{
		ReleaseTargets();

		if (targets.size() > m_Views.size())
		{
			return Failure(EINVAL, "more targets than this renderer holds");
		}

		for (const RenderTarget& target : targets)
		{
			const MappedImage* mapped = target.AsMapped();

			if (mapped == nullptr)
			{
				// Seam/Renderer.h's *a renderer refuses what it cannot bind rather than assuming*, and
				// the case it names first: a CPU blitter handed a dmabuf is a composition-root
				// miswiring, so it is `EINVAL` here rather than a cast that happens to work.
				ReleaseTargets();

				return Failure(EINVAL, "a CPU renderer needs a mapped target");
			}

			Result<MutableImageView> view =
				MutableImageView::Over(mapped->Bytes(), target.Size, mapped->Stride, target.Format);

			if (!view)
			{
				ReleaseTargets();

				return std::unexpected{ view.error() };
			}

			m_Views[m_Count++] = *view;
		}

		return {};
	}

	void ReleaseTargets() noexcept override
	{
		m_Views.fill(MutableImageView{});
		m_Count = 0;
	}

	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override
	{
		if (request.Target >= m_Count)
		{
			return Failure(EINVAL, "recording into a target that is not bound");
		}

		const MutableImageView& view = m_Views[request.Target];

		// `LOAD` semantics, exactly as the Vulkan renderer's render pass takes: what is outside the
		// damage is what was there. That is the whole of what a damage region means, and a renderer
		// that cleared the target would make every partial-damage assertion below vacuous.
		for (const PixelRect<DeviceSpace>& rect : request.Damage.Rects())
		{
			view.Fill(rect, Background);
		}

		for (const DrawItem& item : request.Items)
		{
			const DrawSolid* solid = std::get_if<DrawSolid>(&item.Content);

			if (solid == nullptr)
			{
				continue;
			}

			const PixelRect<DeviceSpace> bounds = item.Shape.PixelBounds();
			const Rgba16 colour{
				Component(solid->Red), Component(solid->Green), Component(solid->Blue), Component(solid->Alpha)
			};

			// Clipped to the damage rather than drawn over the whole target, which is the scissor a
			// real renderer sets once. Painted in list order, which is decision 55's z.
			for (const PixelRect<DeviceSpace>& rect : request.Damage.Rects())
			{
				view.Fill(Intersect(bounds, rect), colour);
			}
		}

		++Records;

		return Submission{ .Point = SyncPoint::Immediate(), .RecordCost = {} };
	}

	[[nodiscard]] bool IsComplete(SyncPoint) const override { return true; }

	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost>) override { return 0; }

	std::uint64_t Records = 0;

private:
	[[nodiscard]] static std::uint16_t Component(float value) noexcept
	{
		return static_cast<std::uint16_t>(std::clamp(value, 0.0F, 1.0F) * 65535.0F + 0.5F);
	}

	[[nodiscard]] static PixelRect<DeviceSpace>
	Intersect(PixelRect<DeviceSpace> left, PixelRect<DeviceSpace> right) noexcept
	{
		const std::int32_t x = std::max(left.Left(), right.Left());
		const std::int32_t y = std::max(left.Top(), right.Top());
		const std::int32_t r = std::min(left.Right(), right.Right());
		const std::int32_t b = std::min(left.Bottom(), right.Bottom());

		return r > x && b > y ? PixelRect<DeviceSpace>::FromEdges({ x, y }, { r, b }) : PixelRect<DeviceSpace>{};
	}

	std::array<MutableImageView, MaxVirtualTargets> m_Views{};
	std::uint32_t m_Count = 0;
};

// A panel of one colour at one place, which is the smallest thing that has a position worth
// asserting.
[[nodiscard]] Node Panel(std::uint32_t solid, PixelPoint<DeviceSpace> at, PixelSize<DeviceSpace> size)
{
	Node node{};
	node.Kind = NodeKind::Solid;
	node.Content = solid;
	node.Extent = { static_cast<float>(size.Width), static_cast<float>(size.Height) };
	node.Transform.Translation = Vector3<double>{ static_cast<double>(at.X), static_cast<double>(at.Y), 0.0 };

	return node;
}

[[nodiscard]] SolidContent Colour(float red, float green, float blue)
{
	return SolidContent{ .Red = red, .Green = green, .Blue = blue, .Alpha = 1.0F, .Color = ColorState::Srgb() };
}

// Everything wired the way a composition root would wire it, held together so a test body is about
// what was drawn.
class Machine
{
public:
	// `wake` is what the *scene* asks for, which Frame/Loop.h weighs separately from damage: a scene
	// that wants every frame draws whether or not anything is owed, and one that wants none draws
	// only what damage forces. The damage tests below say `Never` so that what they count is theirs.
	Machine(
		std::span<const Node> scene,
		std::span<const SolidContent> solids,
		VirtualOutputPolicy policy = {},
		FrameRetention retention = FrameRetention::Release,
		Wake wake = Wake::EveryFrame()
	)
		: m_Sink{ Resolution, Linear, 4, retention }, m_Wake{ wake }
	{
		// The one part of the policy this file chooses: a CPU renderer takes the mapped face of the
		// same images a Vulkan one would import as dmabufs. See Virtual/Output.h's `TargetFace`.
		policy.Face = TargetFace::Mapped;

		const OutputConfiguration configuration{ .Resolution = Resolution, .Period = Period, .Format = Linear };

		m_Output = m_Device.Add(configuration, m_Allocator, m_Sink, &m_Renderer, policy);
		GYRO_REQUIRE(m_Output != nullptr);
		GYRO_REQUIRE(m_Output->Status().has_value());

		GYRO_REQUIRE_EQ(m_Renderer.BindTargets(m_Output->Targets()).has_value(), true);

		m_Outputs[0].Bind(*m_Output, m_Renderer, 0, configuration);

		Publish(scene, solids);

		m_Loop.Bind({ m_Outputs.data(), 1 });
		m_Loop.Listen({ m_Sources.data(), 1 });
	}

	// One turn: the step the composition root takes, then the sleep it takes after it.
	void Turn()
	{
		(void)m_Loop.Step();
		m_Clock.Advance(Period);
	}

	[[nodiscard]] VirtualOutput& Output() noexcept { return *m_Output; }

	[[nodiscard]] FrameOutput& Bound() noexcept { return m_Outputs[0]; }

	[[nodiscard]] CapturingSink& Sink() noexcept { return m_Sink; }

	[[nodiscard]] SketchRenderer& Renderer() noexcept { return m_Renderer; }

private:
	void Publish(std::span<const Node> scene, std::span<const SolidContent> solids)
	{
		// One wake per bound output, because decision 84 reads a schedule of the wrong length as no
		// information — a short span would quietly test the unpublished case.
		const std::array<Wake, 1> wakes{ m_Wake };
		const OutputAdapter placement = OutputAdapter::Identity();

		SnapshotPublisher publisher;
		publisher.PutWakes(wakes);
		publisher.PutNodes<Node>(scene);
		publisher.PutSolids<SolidContent>(solids);
		publisher.PutViews<OutputAdapter>({ &placement, 1 });
		publisher.Build(m_Snapshot, 1);

		GYRO_CHECK(m_Ring.Publish(m_Snapshot.Bytes(), 0));
	}

	ManualClock m_Clock{};
	HeapAllocator m_Allocator{};
	CapturingSink m_Sink;
	Wake m_Wake{};
	SketchRenderer m_Renderer{};
	VirtualDevice m_Device{ m_Clock };
	VirtualOutput* m_Output = nullptr;

	SnapshotRing m_Ring{};
	ReturnChannel m_Returns{};
	SnapshotBuffer m_Snapshot{};
	SceneEvaluator m_Evaluator{ m_Clock };

	std::array<FrameOutput, 1> m_Outputs{};
	FrameLoop m_Loop{ m_Clock, m_Ring, m_Returns, m_Evaluator };
	std::array<IEventSource*, 1> m_Sources{ &m_Device };
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

// The whole crossing: a scene published on the dispatch side, drawn where the walk placed it, read
// by a consumer out of the image the presenter handed back.
GYRO_TEST(VirtualComposite, APublishedSceneReachesTheConsumerAsPixels)
{
	const std::array<SolidContent, 2> solids{ Colour(1.0F, 0.0F, 0.0F), Colour(0.0F, 0.0F, 1.0F) };

	std::vector<Node> scene(3);
	scene[0].SubtreeLength = 2;
	scene[1] = Panel(0, { 4, 4 }, { 16, 8 });
	scene[2] = Panel(1, { 40, 20 }, { 8, 8 });

	Machine machine{ scene, solids };

	// Whole-output damage on the first frame, which is what a freshly bound output owes.
	machine.Bound().DamageWholeOutput();

	machine.Turn();
	machine.Turn();

	GYRO_REQUIRE_EQ(machine.Sink().Copied(), std::uint64_t{ 1 });

	const CapturingSink::Capture capture = machine.Sink().Newest();
	GYRO_REQUIRE(capture.Image.IsValid());

	DumpOnFailure(capture.Image, capture.Frame.Sequence);

	// The two panels, where the walk put them — which is the whole chain: the node's translation, the
	// output adapter, the projection, and the renderer's own placement, agreeing.
	GYRO_CHECK(capture.Image.IsUniform(PixelRect<DeviceSpace>{ { 4, 4 }, { 16, 8 } }, Red));
	GYRO_CHECK(capture.Image.IsUniform(PixelRect<DeviceSpace>{ { 40, 20 }, { 8, 8 } }, Blue));

	// And nothing outside them, checked one pixel past each edge of the first panel rather than at a
	// distance — an off-by-one in the projection is invisible from the middle of the rectangle.
	GYRO_CHECK_EQ(capture.Image.At(3, 4), Background);
	GYRO_CHECK_EQ(capture.Image.At(20, 4), Background);
	GYRO_CHECK_EQ(capture.Image.At(4, 3), Background);
	GYRO_CHECK_EQ(capture.Image.At(4, 12), Background);
}

// One frame per boundary, each seen once, and the ring turning because the consumer gave the images
// back. A consumer handed the same image twice would pass every assertion above.
GYRO_TEST(VirtualComposite, EachFrameReachesTheConsumerExactlyOnce)
{
	const std::array<SolidContent, 1> solids{ Colour(1.0F, 0.0F, 0.0F) };

	std::vector<Node> scene(2);
	scene[0].SubtreeLength = 1;
	scene[1] = Panel(0, { 4, 4 }, { 16, 8 });

	Machine machine{ scene, solids };

	std::uint64_t previous = 0;

	for (int frame = 0; frame < 4; ++frame)
	{
		machine.Bound().DamageWholeOutput();
		machine.Turn();
	}

	// The last turn's frame has not been drained yet, so three of the four have landed.
	GYRO_REQUIRE(machine.Sink().Copied() >= 3);

	for (std::size_t index = 0; index < machine.Sink().Retained(); ++index)
	{
		const CapturingSink::Capture capture = machine.Sink().At(index);

		GYRO_REQUIRE(capture.Image.IsValid());
		GYRO_CHECK(capture.Image.IsUniform(PixelRect<DeviceSpace>{ { 4, 4 }, { 16, 8 } }, Red));

		if (index > 0)
		{
			GYRO_CHECK(capture.Frame.Sequence > previous);
		}

		previous = capture.Frame.Sequence;
	}

	// The ring turned, so every image came back: an output holding one at the end is the one that was
	// just presented and has not been drained.
	GYRO_CHECK(machine.Output().FreeTargets() > 0);
}

// The consumer that will not let go, seen from the frame loop's side.
//
// Virtual/Output.h says a stalled output is correct behaviour rather than a dropped frame, and
// Frame/Loop.h says a refused acquisition is *not damage lost*. Those are two halves of one claim
// and this is where they meet: the loop stops presenting, keeps owing the damage, and pays it the
// moment an image comes back.
GYRO_TEST(VirtualComposite, AStalledConsumerCostsFramesAndNeverDamage)
{
	const std::array<SolidContent, 1> solids{ Colour(1.0F, 0.0F, 0.0F) };

	std::vector<Node> scene(2);
	scene[0].SubtreeLength = 1;
	scene[1] = Panel(0, { 4, 4 }, { 16, 8 });

	Machine machine{ scene, solids, { .Targets = 2 }, FrameRetention::Hold, Wake::Never() };

	for (int frame = 0; frame < 6; ++frame)
	{
		machine.Bound().DamageWholeOutput();
		machine.Turn();
	}

	// Two images and a consumer that keeps them: two composites, and no more however long the loop
	// runs. A renderer that kept recording would be drawing into an image somebody is reading.
	GYRO_CHECK_EQ(machine.Renderer().Records, std::uint64_t{ 2 });
	GYRO_CHECK_EQ(machine.Output().FreeTargets(), 0U);

	// The damage the loop could not pay is still owed, which is what makes the skip safe.
	GYRO_CHECK(!machine.Bound().Damage().IsEmpty());

	machine.Sink().ReleaseHeld(machine.Output());
	machine.Turn();

	GYRO_CHECK_EQ(machine.Renderer().Records, std::uint64_t{ 3 });
	GYRO_CHECK(machine.Bound().Damage().IsEmpty());
}

// A characterisation rather than a promise, and the gap it pins is a real one.
//
// Seam/Renderer.h says `RecordRequest::Damage` is *per target and not per output* — a target reached
// by `AcquireTarget` may be two or three frames old, so what is stale in it is the union of every
// damage since **it** was last drawn, and accumulating that "belongs to the caller". The caller is
// Frame/Loop.h, and what it accumulates is per *output*, cleared on every successful present. With a
// ring three deep, an image is handed back having missed two frames' worth of damage that nobody
// will repaint.
//
// It is invisible today because decision 101 reports the whole output while anything is moving, so
// every damage region in practice covers everything. It stops being invisible the moment there is a
// partial-composite path to feed, which is what that decision defers. This test states what happens
// now so that closing the gap is a deliberate change with a failing test in front of it rather than
// something that quietly starts working.
GYRO_TEST(VirtualComposite, PartialDamageIsNotYetAccumulatedAcrossTheRing)
{
	const std::array<SolidContent, 1> solids{ Colour(1.0F, 0.0F, 0.0F) };

	// One panel covering the whole output, so that "this image was fully painted" and "this image was
	// painted only where the damage was" are two different pictures rather than two shades of black.
	std::vector<Node> scene(2);
	scene[0].SubtreeLength = 1;
	scene[1] = Panel(0, {}, Resolution);

	Machine machine{ scene, solids, { .Targets = 2 }, FrameRetention::Release, Wake::Never() };

	// Frame one repaints everything, into the first image of a ring of two.
	machine.Bound().DamageWholeOutput();
	machine.Turn();
	machine.Turn();

	GYRO_REQUIRE_EQ(machine.Sink().Copied(), std::uint64_t{ 1 });
	GYRO_CHECK(machine.Sink().Newest().Image.IsUniform(PixelRect<DeviceSpace>{ {}, Resolution }, Red));

	// Frame two damages the left half only, and lands in the *other* image — which nothing has ever
	// drawn into.
	Region<DeviceSpace> half;
	half.Add(PixelRect<DeviceSpace>{ {}, { 32, 32 } });
	machine.Bound().AddDamage(half);

	machine.Turn();
	machine.Turn();

	GYRO_REQUIRE_EQ(machine.Sink().Copied(), std::uint64_t{ 2 });
	GYRO_REQUIRE_EQ(machine.Renderer().Records, std::uint64_t{ 2 });

	const CapturingSink::Capture second = machine.Sink().Newest();
	GYRO_REQUIRE(second.Image.IsValid());

	DumpOnFailure(second.Image, second.Frame.Sequence);

	// The damaged half is painted.
	GYRO_CHECK(second.Image.IsUniform(PixelRect<DeviceSpace>{ {}, { 32, 32 } }, Red));

	// The rest of it is not, and that is the whole finding: the consumer was handed a frame that is
	// half a panel, because the loop asked for the damage *this* frame produced rather than the
	// damage this *image* had missed. A buffer-age accumulation would have made the two halves agree.
	GYRO_CHECK(!second.Image.IsUniform(PixelRect<DeviceSpace>{ { 32, 0 }, { 32, 32 } }, Red));

	const std::optional<PixelRect<DeviceSpace>> painted = second.Image.BoundsOfDiffering(Background);
	GYRO_REQUIRE_EQ(painted.has_value(), true);
	GYRO_CHECK_EQ(*painted, (PixelRect<DeviceSpace>{ {}, { 32, 32 } }));
}
