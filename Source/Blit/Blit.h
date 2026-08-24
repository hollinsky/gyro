#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Blit/Band.h"
#include "Blit/Transfer.h"
#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Seam/Importer.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"

// The CPU renderer: `Seam/Renderer.h`'s `Record` half, into a mapping rather than onto a device.
//
// **It is the floor beneath the floor tier**, which is
// [decision 79](../../Docs/Decisions.md)'s reading and what makes everything else about it follow.
// Decision 40 makes software rendering a *device* selection, so lavapipe is the floor; this runs
// before any GPU driver has loaded on every boot, and permanently on a machine where Vulkan will not
// initialize at all. There is nothing underneath it.
//
// **Which is why refusing is safe here in a way it is not one tier up, and only for one reason.**
// Seam/Renderer.h's *a renderer refuses what it cannot express* rests on something below to fall to,
// and below this there is nothing: a refusal is a dark machine that cannot say why. What makes it
// safe is that `Blit` only ever composites gyro's own scene — the console's and the splash's, never
// a client's — so a refused item is a bug in code gyro wrote, caught by a test. **That has to stay
// true by construction rather than by habit**, and it is the one sentence in this file worth
// re-reading before widening what reaches it.
//
// **What it draws today is a solid, a texture, and a group.** Decision 110 authors the boot scene
// to what this draws rather than growing this to meet a scene it never sees: no material, no
// elevation, no `Reference`, and axis-aligned quads with subpixel edges — a scaled and placed logo
// needs coverage on the boundary spans and nothing more. Everything else is refused rather than
// silently dropped, which is where Render/Renderer.h stands one tier up after decision 107.
//
// **The group is the exception to *author the scene down to the renderer*, and decision 110 says
// why.** Group opacity is not per-node alpha — decision 60 — and a window fading out together with
// its open submenu has to cross-fade as one image or the overlap shows through at every value except
// the two ends. That needs an offscreen, and an offscreen is the one thing a flat list cannot be
// widened into later: doing it afterwards would touch the frame loop, both renderers, and the damage
// path at once. So it is built here even though the splash and the console nest nothing.
//
// **The band is what makes it nearly free.** A group's members carry their own device-space corners
// and its quad is where they already are, so the offscreen is the same pixels of the same output —
// which means a band-sized level of scratch is a whole offscreen, and flattening a subtree is one
// `Over` per row rather than a render target and a resample. See `MaxDepth` for what nesting costs.
//
// **The texture is required rather than optional, and the reason is the first second of a boot.**
// The firmware's BGRT logo is an image, and Docs/Architecture.md#from-firmware-to-gyro makes
// reproducing it a scale-and-place in gyro's own mode — the offsets are in the *firmware's* mode, so
// the logo is resampled almost every time, and getting it wrong is a visible jump at the one moment
// the whole design exists to make seamless. The console is the second consumer and it is one image
// rather than one item per cell, for decision 110's reason: sixteen thousand cells is a megabyte of
// draw list a frame to say that one line changed.
//
// **A rounded corner is refused for now and is not foreclosed.** The coverage arithmetic a subpixel
// edge already needs is most of what an analytic corner wants, and the boot scene is gyro's own and
// can be authored square until there is a reason not to. Decision 110's refusal list is where that
// gets revisited.
//
// **Nothing here is platform-specific and that is enforced.** This module is `PORTABLE`: a composite
// into a mapped pointer names no header a `CheckPortability.cmake` run would object to, which is what
// makes the whole renderer testable on a machine with no GPU, no seat, and no panel — the machine it
// exists to draw on.

class Blit final : public IRenderer, public ITextureImporter
{
public:
	// Seam/RenderTarget.h's ring ceiling, because this renderer binds exactly what a presenter hands it
	// and has no reason to carry slack the way a device-side bind table does. A set larger than that is
	// `EINVAL` rather than a partial bind, per Seam/Renderer.h: half a set is a numbering with holes in
	// it.

	// The most images held at once. Decision 110 names both consumers — the firmware logo the splash
	// continues and the console's own text grid — and this is well clear of them, for
	// Core/SlotAllocator.h's reason: a bound exists to turn unbounded growth into a stated refusal
	// rather than to estimate a working set.
	static constexpr std::size_t MaxImages = 8;

	// How long a draw list this will take, which is `Frame/Evaluator.h`'s arena capacity arrived at
	// independently — this module may not name that one, and a bound that has to be guessed is better
	// guessed at the same number. A longer list is `EINVAL` rather than a truncated picture: nothing
	// produces one, and reporting it beats overrunning the classification below.
	static constexpr std::size_t MaxItems = 4096;

	// How deeply a group may nest. Every level is a whole band of scratch, reserved at `BindTargets`
	// because `Record` may not allocate and cannot know in advance how deep a frame goes — so this is
	// the one multiplier on the footprint Blit/Band.h's entire argument is about keeping small. Five
	// levels is a little over a megabyte the frame thread has locked down, against 66 MB for a single
	// full-size 4K scratch.
	//
	// **Four, and the number is the vocabulary rather than the boot scene.** What this renderer
	// actually composites nests nothing: the splash is a logo and the console is one image, so zero
	// would draw every frame gyro has today. Decision 110's other refusals are safe because they are
	// gyro's own scene and are caught by a test, but a *depth* is the one refusal that could arrive
	// from a scene which is otherwise entirely drawable, and below this renderer there is nothing to
	// fall to — a refusal here is a dark machine that cannot say why. So it is set where decision 60's
	// transitions stop composing instead: a greeter cross-fade holding an overview dismissal holding a
	// workspace holding a window that is itself fading is four, and stacking a fifth is not a
	// transition anybody has described.
	static constexpr std::size_t MaxDepth = 4;

	// The clock is for `Submission::RecordCost` and nothing else. Seam/Renderer.h puts the measurement
	// on the party that knows where the work started and stopped, and on this renderer that figure is
	// the whole of decision 29's `C` — there is no second device for the rest of it to hide in.
	explicit Blit(const IClock& clock) : m_Clock{ &clock } { m_Painted.resize(MaxItems); }

	// **Every target must be a `MappedImage`.** A CPU blitter handed a dmabuf is a composition-root
	// miswiring, so it is `EINVAL` here rather than a cast that happens to work on the machine it was
	// written on — Seam/Renderer.h names this as the first case the discriminated memory exists for.
	//
	// `EINVAL` also for a packed format this cannot code, a modifier that is not linear, and an output
	// whose transfer function is absolute — see Blit/Transfer.h for why `Pq` waits on
	// Docs/Open.md's wire-colorimetry entry rather than being approximated.
	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets, ColorState output) override;

	void ReleaseTargets() noexcept override;

	// Seam/Importer.h's dispatch half, and on this renderer there is nothing to import: every source it
	// can take is already CPU pixels, so adoption is a table entry rather than a device allocation.
	// Descriptors are `EINVAL` — a blitter handed a dmabuf is the composition-root miswiring
	// Seam/RenderTarget.h names at the other end of the composite, arriving at this end.
	//
	// **Borrowed rather than copied, and the caller keeps the pixels alive.** The console's grid is the
	// panel's size, which is thirty megabytes at 4K, and printing a line into it is a `memmove` inside
	// memory it already owns; a renderer that copied on adoption would copy the whole grid every time a
	// line appeared, and one that copied at each frame would be the full-size scratch Blit/Band.h exists
	// to refuse. So this records a pointer and `Forget` is what has to happen before the pixels go away.
	// Core/Texture.h's generational id protects against a *stale id*, which resolves to nothing; it
	// cannot protect against memory freed underneath a live one, which is why the seam puts the lifetime
	// on the caller and ties it to the watermark.
	//
	// Allocates nothing, whatever the seam permits — the table is fixed at `MaxImages`. `EINVAL` for a
	// null id, for pixels that do not describe the image they claim, for a format or a layout this
	// cannot sample, and when there is no slot left.
	[[nodiscard]] Result<void> Adopt(TextureId id, const TextureSource& source) override;

	// Deliberately not done by `ReleaseTargets`: an image outlives the target set it was drawn into, and
	// a mode change that rebinds targets does not change which logo is on them.
	void Forget(TextureId id) noexcept override;

	// **Two passes over the item list, and the first one draws nothing.** An item this cannot express
	// is `EINVAL`, and refusing it after painting half the list would leave a target that is neither
	// the old frame nor the new one and then present it. So the whole list is checked before the first
	// pixel is written, and a refused frame leaves the target exactly as it was.
	//
	// The `RenderMode` is read and makes no difference: the floor composite drops effects, and there
	// are none here to drop. Reporting the mode back through the cost is still right, because the
	// caller's budget is split along that axis before it is split by device.
	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override;

	// The pixels were finished on the CPU before the call returned, so there is nothing to wait for.
	[[nodiscard]] bool IsComplete(SyncPoint) const override { return true; }

	// No second device, so no second cost. Seam/Renderer.h says this is the correct report rather than
	// a stub: this renderer's work has nowhere else to have happened.
	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost>) override { return 0; }

private:
	// One bound target, flattened out of the variant at bind time so that `Record` never re-asks a
	// question that was already answered — which is also what keeps the frame path free of the branch
	// that would have to handle the answer being no.
	struct Bound
	{
		std::byte* Pixels = nullptr;
		std::size_t Length = 0;
		std::uint32_t Stride = 0;
		PixelSize<DeviceSpace> Size{};
		PixelFormat Format{};
	};

	// One adopted image. The id is kept whole rather than reduced to its index, which is the entire
	// mechanism behind Core/Texture.h's *a stale id draws nothing*: a generation that has moved on
	// compares unequal and the lookup misses, where an index alone would resolve to whatever took
	// the slot.
	struct Image
	{
		TextureId Id;
		const std::byte* Pixels = nullptr;
		std::uint32_t Stride = 0;
		std::uint32_t Code = 0;
		std::uint32_t Bits = 0;
		PixelSize<BufferSpace> Size{};
	};

	// What a textured item reads, reduced in `Record` to the arithmetic a pixel needs: a base
	// pointer, a decode table, an affine map from a device column to a texel coordinate, and the
	// texels the map is allowed to reach.
	struct Sampled
	{
		const std::byte* Pixels = nullptr;

		// The decode direction over the source's own depth, resolved per item rather than per texel —
		// see Blit/Transfer.h. Null for an item that samples nothing.
		const std::uint16_t* Decode = nullptr;

		std::uint32_t Stride = 0;
		std::uint32_t Code = 0;
		std::uint32_t Shift = 0;

		// `t = Origin + pixel * Scale`, in texel-centre coordinates, so that zero is the centre of
		// texel zero and a resample that landed on one lands on an integer — which is what makes the
		// filter return that texel unchanged rather than nearly so.
		float OriginX = 0.0F;
		float OriginY = 0.0F;
		float ScaleX = 1.0F;
		float ScaleY = 1.0F;

		// **Clamped to the sampled rectangle rather than to the image.** A filter tap at the edge of
		// an atlas slot would otherwise read the slot beside it, which is the one texture artefact
		// that looks like a rendering bug in a completely different part of the picture.
		std::int32_t MinX = 0;
		std::int32_t MinY = 0;
		std::int32_t MaxX = 0;
		std::int32_t MaxY = 0;

		// Whether the source's codes are already proportional to light, which decides whether a
		// premultiplied texel can be converted where it stands.
		bool Proportional = false;

		bool Straight = false;

		// `DrawItem::Sampling`'s answer, carried down to the loop that acts on it: the resample is a
		// no-op, so the filter is skipped and a texel is copied. See `SampleRow` in Blit.cpp for why
		// that is a cost decision rather than a correctness one.
		bool Exact = false;

		// The map, applied. A device column and a device row rather than a point, because a run walks
		// one of them and holds the other.
		[[nodiscard]] float Across(std::int32_t column) const noexcept
		{
			return OriginX + static_cast<float>(column) * ScaleX;
		}

		[[nodiscard]] float Down(std::int32_t row) const noexcept { return OriginY + static_cast<float>(row) * ScaleY; }
	};

	// What one item contributes, worked out once in `Record` and read once per band. The colour
	// conversion is an exact `std::pow` per channel, so doing it per band would be doing it a few
	// hundred times for a full-screen repaint; the pass that has to walk the list anyway to refuse
	// what it cannot express is the pass that should be keeping the answer. A texture's per-item
	// arithmetic — the map, the clamp, the table — is here for the same reason and pays it once.
	struct Painted
	{
		Rect<DeviceSpace> Shape{};
		Light Colour{};
		Sampled Source{};
		float Opacity = 1.0F;

		// The items behind this one that compose into it, for a group. It is the length of the run
		// rather than a child count — Seam/Renderer.h — so a nested group and everything under it are
		// inside this number, and the walk skips the whole of it whether or not the group draws.
		std::uint32_t Members = 0;

		bool Draws = false;
		bool Textured = false;
		bool Grouped = false;
	};

	[[nodiscard]] Result<Painted> Classify(const DrawItem& item) const noexcept;

	// The run structure, checked once for the whole list and before the first pixel, which is
	// `Record`'s rule and the reason it is a pass of its own rather than a test inside `Classify` —
	// one item cannot see where another one's run ends.
	//
	// `EINVAL` for a run that reaches past the list, for one that reaches past the run it is nested
	// inside — the same test against the enclosing end, and what makes a nested group *contained* in
	// its parent rather than merely following it — and for a group deeper than `MaxDepth`.
	[[nodiscard]] Result<void> Nesting(std::span<const DrawItem> items) const noexcept;

	// What an id names, or nothing. A linear scan over at most `MaxImages`, once per item rather than
	// once per pixel — a map would be a container in a module whose whole argument is that its
	// working set stays in cache.
	[[nodiscard]] const Image* Find(TextureId id) const noexcept;

	// Which decode table a source depth reads. Two of them, and the mapping is spelled once here
	// rather than as an index at each use.
	[[nodiscard]] static constexpr std::size_t Depth(std::uint32_t bits) noexcept { return bits == 10 ? 1 : 0; }

	// One texel, decoded into the composite's linear premultiplied light and clamped to the sampled
	// rectangle.
	[[nodiscard]] static Light Fetch(const Sampled& source, std::int32_t x, std::int32_t y) noexcept;

	// The filtered read: four texels and a bilinear weight, in linear light rather than in the
	// source's encoding. Bilinear rather than a box, and Blit.cpp is where that is argued.
	[[nodiscard]] static Light Filter(const Sampled& source, float u, float v) noexcept;

	// Either of the above, whichever the item's `Sampling` asked for. The two partially covered edge
	// columns go through here; a run hoists the question out of its loop instead.
	[[nodiscard]] static Light Sample(const Sampled& source, float u, float v) noexcept;

	// One row of a textured item over `[from, to)` of the device grid, attenuated by `scale`, into
	// the sample scratch — which is what `Band::BlendRun`'s per-pixel form then reads.
	[[nodiscard]] std::span<const Light>
	SampleRow(const Sampled& source, float v, std::int32_t from, std::int32_t to, std::uint16_t scale) noexcept;

	// The composite for one damage rectangle, band by band.
	void Paint(const Bound& target, PixelRect<DeviceSpace> rect, std::size_t items) noexcept;

	// `[from, to)` of the item list into one level of the scratch, over one band's rows and
	// `[left, right)` of its columns. A group is the same walk one level up, which is the whole of
	// what makes this recursive rather than a loop.
	void Compose(
		std::size_t level,
		std::size_t from,
		std::size_t to,
		std::int32_t rows,
		std::int32_t bandTop,
		std::int32_t left,
		std::int32_t right
	) noexcept;

	// One item into one level: the spans, the two partially covered edge columns, and the coverage
	// that distinguishes them.
	void Draw(
		Band& into,
		const Painted& painted,
		std::int32_t rows,
		std::int32_t bandTop,
		std::int32_t left,
		std::int32_t right
	) noexcept;

	// The level above, brought down into `level` at the group's own opacity — decision 60's
	// *composited once at g*. `[left, right)` is the group's own columns, already clipped, because
	// that is the rectangle the level above was erased over and reading outside it would read a
	// previous group's pixels.
	void Flatten(
		std::size_t level,
		const Painted& group,
		std::int32_t rows,
		std::int32_t bandTop,
		std::int32_t left,
		std::int32_t right
	) noexcept;

	// One band's rows, encoded into the target. The only place the target is written, and it is
	// written and never read.
	void Emit(
		const Bound& target,
		std::int32_t top,
		std::int32_t rows,
		std::int32_t left,
		std::int32_t right
	) const noexcept;

	const IClock* m_Clock = nullptr;

	std::array<Bound, MaxTargets> m_Targets{};
	std::uint32_t m_Count = 0;

	std::array<Image, MaxImages> m_Images{};
	std::size_t m_Held = 0;

	// Sized once, in the constructor, and never resized after: `Record` runs inside
	// Core/FrameSection.h's guard, where a `std::vector` growing past its reserve aborts the process.
	std::vector<Painted> m_Painted;

	// One row of sampled pixels, as wide as the widest bound target — sized at `BindTargets` for the
	// same reason and never touched between frames.
	std::vector<Light> m_Samples;

	ColorState m_Output = ColorState::Srgb();
	TransferTable m_Transfer{};

	// The decode direction, one table per source depth. Both are built at a binding because both are
	// cheap and because the alternative is deciding at adoption what the output's transfer function
	// is going to be, which is a fact this renderer does not have until it is bound.
	std::array<DecodeTable, 2> m_Decode{};

	// The scratch, one level per depth of nesting plus the output's own at zero. All of them are
	// reserved together at `BindTargets` and all of them are the same rows of the same output, which
	// is what lets a group be a blend rather than a resample — see `Band::BlendAbove`.
	std::array<Band, MaxDepth + 1> m_Levels{};
};
