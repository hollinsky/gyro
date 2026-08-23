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
#include "Geometry/Space.h"
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
// **What it draws today is a solid, and the staging is `Render`'s.** Decision 110 authors the boot
// scene to what this draws rather than growing this to meet a scene it never sees: no material, no
// elevation, no `Reference`, and axis-aligned quads with subpixel edges — a scaled and placed logo
// needs coverage on the boundary spans and nothing more. A `DrawTexture` and a `DrawGroup` are named
// there as required and are `EINVAL` here, which is exactly where Render/Renderer.h stands one tier
// up after decision 107: a solid draws, and everything else is refused rather than silently dropped.
// The texture is the firmware logo and is the next thing owed.
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

class Blit final : public IRenderer
{
public:
	// A ring three deep plus the one being scanned, which is what a DRM presenter binds at its
	// widest. A set larger than this is `EINVAL` rather than a partial bind, per Seam/Renderer.h: half
	// a set is a numbering with holes in it.
	static constexpr std::size_t MaxTargets = 4;

	// How long a draw list this will take, which is `Frame/Evaluator.h`'s arena capacity arrived at
	// independently — this module may not name that one, and a bound that has to be guessed is better
	// guessed at the same number. A longer list is `EINVAL` rather than a truncated picture: nothing
	// produces one, and reporting it beats overrunning the classification below.
	static constexpr std::size_t MaxItems = 4096;

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

	// What one item contributes, worked out once in `Record` and read once per band. The colour
	// conversion is an exact `std::pow` per channel, so doing it per band would be doing it a few
	// hundred times for a full-screen repaint; the pass that has to walk the list anyway to refuse
	// what it cannot express is the pass that should be keeping the answer.
	struct Painted
	{
		Rect<DeviceSpace> Shape{};
		Light Colour{};
		float Opacity = 1.0F;
		bool Draws = false;
	};

	[[nodiscard]] static Result<Painted> Classify(const DrawItem& item, ColorState output) noexcept;

	// The composite for one damage rectangle, band by band.
	void Paint(const Bound& target, PixelRect<DeviceSpace> rect, std::size_t items) noexcept;

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

	// Sized once, in the constructor, and never resized after: `Record` runs inside
	// Core/FrameSection.h's guard, where a `std::vector` growing past its reserve aborts the process.
	std::vector<Painted> m_Painted;

	ColorState m_Output = ColorState::Srgb();
	TransferTable m_Transfer{};
	Band m_Band{};
};
