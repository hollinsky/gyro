#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Seam/Dressing.h"
#include "Seam/RenderTarget.h"
#include "Seam/SyncPoint.h"
#include "World/Elevation.h"
#include "World/Material.h"

// The render half of the seam: what produces the pixels a presenter puts on the glass.
//
// Seam/Presenter.h is the other half and this one is written against it — `Record` fills a target the
// presenter owns and yields the SyncPoint that presenter's `Present` waits on. The two are separate
// interfaces because the thing that varies is different on each side:
// Docs/Decisions.md decision 40 makes software rendering a *device* rather than a backend, and
// decision 79 makes the pre-Vulkan console a *renderer* rather than a second presentation path. So a
// DRM presenter is paired with `Blit` at boot and with a Vulkan device a moment later, across the same
// frame clock, the same damage accumulation, and the same presentation feedback.
//
// **The two implementations are the design constraint, and they are far apart.** `Blit` is a CPU
// blitter into a mapped dumb buffer with no GPU driver loaded at all; the ordinary renderer is a
// Vulkan device with timeline semaphores, imported dmabufs, and a queue that finishes work some
// milliseconds after the thread stopped talking about it. Everything below that looks like
// over-provisioning for the first is what the second needs, and everything that looks like a
// restriction on the second is what the first can honour.
//
// **The verbs have opposite contracts and the file says which is which.** That is decision 73's
// lesson from `IPresenter`, applied on purpose rather than discovered late: `Record` is frame-side,
// bounded, and is decision 29's `C` on the CPU device; `BindTargets` and `ReleaseTargets` allocate,
// import, and are permitted to be slow, because they run at a target invalidation and never inside
// the frame section.
//
// **There are no signals here, and the asymmetry with `IPresenter` is the point.** A presenter
// reports things that happen without being asked — a flip landed, a transition completed — so it
// needs an event and a source to drain it. Nothing a renderer does arrives on its own: completion is
// polled through the timeline the caller already holds, and cost samples are collected when the
// caller asks. A renderer therefore needs no `IEventSource`, which is also what makes the null
// renderer the schedulability sweep runs a few lines of arithmetic rather than a fake device.
//
// **What is deliberately not here: how a texture comes to exist.** `DrawTexture` names one and
// nothing in this file mints one. Import is dispatch-side — a `wl_buffer` arrives on the dispatch
// thread and turns into a device image there, never inside the frame section — which makes it the
// renderer's *other* half, in the sense Publication and Animation already use the word: `Reader` and
// `Publisher`, `Solve` and `Author`, split by direction rather than by purity. That half is written
// when there is a protocol layer to call it. Naming the limit rather than guessing at it is
// Seam/Presenter.h's treatment of the promoted client buffer, one seam over.

// Which composite ran. The axis a cost is split along before it is split by device.
//
// Docs/Architecture.md#the-floor-tier calls the effect-free composite a first-class render mode rather
// than an emergency fallback, and it is a parameter rather than a second verb because the caller
// already holds it: decision 35's record-time check chooses the mode, and the thing that chose it is
// the thing that reports what it cost. It lived in Frame/Budget.h until this file existed, which is
// where that header said it belonged.
//
// The floor composite is a different shader rather than a prefix of the planned one — decision 62's
// fusion folds pointwise effects into the composite pipeline, so there is no separable span of a
// planned frame to stop early at. A renderer is told which one to draw and draws that one.
enum class RenderMode : std::uint8_t
{
	Planned,
	Floor,
};

// A node's quad, projected into the output's device grid.
//
// **A projected quad rather than a transform, and every part of that is forced.** Decision 55 gives
// nodes a 3D transform with node-local perspective and composites in strict tree order with no depth
// buffer, so what reaches a renderer is a plane in space and never a scene to be viewed. The chain
// that produced it is a product of decomposed TRS transforms which is not itself one — so it cannot
// cross as a `NodeTransform`, and Geometry/NodeTransform.h says why it should not cross as a matrix
// either: the layout and the precision at which translation folds against the output origin belong to
// the render path. Four corners is what survives both. It is also exactly what
// `NodeTransform::Apply` already produces, composed down the tree a point at a time, and the
// precision that matters happens there — global space is double and device space is float, and the
// fold is where the double is spent.
//
// **The weights are what keep a texture from swimming.** `Apply` divides each corner by the
// perspective weight it computed and discards it; interpolating anything across the resulting quad
// without it is affine-per-triangle, which is the warp every early 3D console is remembered for.
// The weight here is the accumulated divisor for that corner — the product along the chain, exactly
// one for an orthographic node — and it is the `w` a perspective-correct interpolation divides by.
//
// **Culling has already happened.** Decision 55 culls back faces unconditionally, and what answers
// it is the sign of *this quad's* area — the shoelace sum over the four corners below, which is the
// composed chain's orientation and the projection's together. *(Revised 2026-08-22.)* It was
// `NodeTransform::FacesViewer` until decision 93, which does not compose: that predicate reads one
// node's rotation and scale signs, and two nodes each turned eighty degrees both face front while
// their composition does not. A quad that reaches a renderer is one that faces the viewer.
struct Quad
{
	// Top-left, top-right, bottom-right, bottom-left of the node's own extent — the winding of a
	// Y-down space, and the order `[0, Width] x [0, Height]` visits under Docs/Architecture.md#the-
	// spaces' surface-local convention. Spelled out because "the four corners" names two different
	// orders and a renderer that assumed the other one produces a diagonal tear rather than an error.
	Point<DeviceSpace> Corners[4]{};

	// Per corner, positionally. One is orthographic, and dividing by this is unconditional because
	// the producer has already culled every node with a corner at or below
	// `Geometry/NodeTransform.h`'s `Projected::MinimumWeight`. *(Revised 2026-08-22.)* That used to
	// rest on `Perspective` being bounded, which decision 92 found was the artefact rather than the
	// guarantee: the bound held over a node's own quad and not over a descendant of one, and what
	// enforced it was the clamp whose saturation made a quad stop being a projective image of a
	// rectangle — the exact warp this array exists to remove.
	float Weights[4]{ 1.0F, 1.0F, 1.0F, 1.0F };

	// The quad an axis-aligned rectangle makes. The group item below is the caller that matters: a
	// flattened subtree lands on an axis-aligned bound and has no projection of its own.
	[[nodiscard]] static constexpr Quad FromRect(Rect<DeviceSpace> rect) noexcept
	{
		return { { { rect.Left(), rect.Top() },
			       { rect.Right(), rect.Top() },
			       { rect.Right(), rect.Bottom() },
			       { rect.Left(), rect.Bottom() } },
			     { 1.0F, 1.0F, 1.0F, 1.0F } };
	}

	// The exact real bound. Not rounded, because who rounds it decides which way, and the two callers
	// want opposite things: a mip footprint wants the real area and a scissor wants whole pixels.
	[[nodiscard]] constexpr Rect<DeviceSpace> Bounds() const noexcept
	{
		float left = Corners[0].X;
		float top = Corners[0].Y;
		float right = left;
		float bottom = top;

		for (const Point<DeviceSpace>& corner : Corners)
		{
			left = corner.X < left ? corner.X : left;
			top = corner.Y < top ? corner.Y : top;
			right = corner.X > right ? corner.X : right;
			bottom = corner.Y > bottom ? corner.Y : bottom;
		}

		return Rect<DeviceSpace>::FromEdges({ left, top }, { right, bottom });
	}

	// The whole pixels the quad can touch, rounded *outward*. One home for that rounding, for
	// Geometry/Region.h's reason: rounding damage inward is how a moving edge leaves a one-pixel
	// trail, and three backends rounding it themselves is three chances to pick the wrong direction.
	[[nodiscard]] PixelRect<DeviceSpace> PixelBounds() const noexcept
	{
		const Rect<DeviceSpace> bounds = Bounds();

		return PixelRect<DeviceSpace>::FromEdges(
			{ static_cast<std::int32_t>(std::floor(bounds.Left())),
		      static_cast<std::int32_t>(std::floor(bounds.Top())) },
			{ static_cast<std::int32_t>(std::ceil(bounds.Right())),
		      static_cast<std::int32_t>(std::ceil(bounds.Bottom())) }
		);
	}

	friend constexpr bool operator==(const Quad&, const Quad&) noexcept = default;
};

// A colour fill. Four components meaning whatever the item's `Color` says they mean, which is the
// same rule a buffer's texels are read under and the reason a solid needs no state of its own.
struct DrawSolid
{
	float Red = 0.0F;
	float Green = 0.0F;
	float Blue = 0.0F;
	float Alpha = 1.0F;

	friend constexpr bool operator==(DrawSolid, DrawSolid) noexcept = default;
};

// A read from an image the renderer holds.
//
// **A surface reference and a snapshot reference are one thing here**, where decision 51's node kinds
// keep them apart. The difference between them is who owns the pixels and how long they live, which
// is a question for the half of the renderer that imported them; what a composite does with either is
// sample a rectangle of texels. Collapsing the two at this seam is what makes an exit animation the
// same draw as the window it replaced.
struct DrawTexture
{
	TextureId Texture;

	// The texels sampled, real-valued because `wp_viewport`'s `src` is `wl_fixed` and an atlas slot's
	// content may sit at a half-texel inset. Empty means the whole image.
	Rect<BufferSpace> Source{};

	friend constexpr bool operator==(DrawTexture, DrawTexture) noexcept = default;
};

// The `Count` items immediately following this one, composited into an offscreen and then drawn as
// one — which is decision 60's flattening, and the only reason this list has structure at all.
//
// **Without it the list forecloses a settled decision, which is the whole test decision 78 sets.**
// Group opacity is not per-node alpha: a window and its open submenu fading together must cross-fade
// as one image or the overlap shows through, and that requires an offscreen. A flat list cannot say
// where the group starts and stops, and widening one into this later would touch the frame loop, both
// renderers, and the damage path at once.
//
// **`Count` is the length of the run and not a child count**, which is the reading that costs a
// renderer nothing: the members are `[index + 1, index + 1 + Count)` and finding them is arithmetic
// rather than a walk. A nested group and everything under it are inside its parent's run, so the two
// readings differ the moment anything nests — a menu, its panel, its open submenu and that submenu's
// panel is a run of three, never a child count of two.
//
// The item's `Quad` is the offscreen's placement and is axis-aligned, because the members' own
// projections are already baked into their device-space corners.
//
// **It is an extent rather than a shape, so a renderer rounds it outward to whole pixels and does not
// antialias it.** The offscreen is a grid of pixels and the bound of a set of quads is generally
// fractional, so the surface has to round out to hold the antialiased edges the members already
// carry. Covering the group's own boundary fractionally on top of that would attenuate those edges a
// second time, and what that looks like is a faint dark seam around every subtree that fades. The
// rounding is free: the pixels it adds are pixels no member wrote, and compositing nothing over
// something leaves the something exactly as it was. Written here rather than in either renderer
// because both have to reach the same picture.
struct DrawGroup
{
	// Items following this one that compose into it. Zero is a group with nothing in it, which is a
	// caller's bug and draws nothing rather than being refused — a frame is not where that is reported.
	std::uint32_t Count = 0;

	friend constexpr bool operator==(DrawGroup, DrawGroup) noexcept = default;
};

// An item with no content of its own: what it draws is its dressing, and there is nothing underneath
// it belonging to the node.
//
// **A container dressed `Glass` is the case, and it is the reason a material is a field rather than a
// kind.** Decision 95 puts a `Material` on every node including the ones that name no content run, so
// the walk emits for a node that has content *or* is dressed — and the second of those has a quad, an
// extent, an opacity, and a material, and nothing to sample or fill. A blurred backdrop and nothing
// else is exactly what that item is.
//
// **Spelled rather than encoded as a fully transparent `DrawSolid`**, which is the same picture and a
// worse contract: a renderer would be inferring "this item is only its dressing" from an alpha of
// zero, which is also what a solid animating to invisible looks like on the frame before it is
// dropped. One of those wants the effect pass and the other wants to be skipped.
struct DrawDressing
{
	friend constexpr bool operator==(DrawDressing, DrawDressing) noexcept = default;
};

// The alternatives, with `DrawDressing` first so that a default-constructed item draws nothing rather
// than opaque black — the same direction World/Node.h defaults every field in: a record nobody
// finished is a still node, not a black rectangle over somebody's screen.
using DrawContent = std::variant<DrawDressing, DrawSolid, DrawTexture, DrawGroup>;

// One thing drawn, in a list that is bottom-first preorder.
//
// **Z is the list order**, for Seam/Presenter.h's reason: a depth field beside an ordered list is a
// second encoding of one fact. Decision 55 composites in strict tree order, so preorder *is* the
// painter's order and the two never have to be reconciled.
//
// What has already happened before an item is built, none of it the renderer's to redo: animations
// are evaluated, the transform chain is composed and projected, back faces are culled, and anything
// entirely outside the target is gone. What has not happened is clipping to the damage region — that
// is a scissor the renderer sets from `RecordRequest::Damage`, once, rather than a set intersection
// per item.
struct DrawItem
{
	DrawContent Content;

	Quad Shape;

	// The node's own extent, which the quad's corners are the image of: local `(0, 0)` is
	// `Shape.Corners[0]` and local `(Width, Height)` is `Shape.Corners[2]`. It is what turns the four
	// corners back into a parameterization — texture coordinates, and the space `Radius` is measured
	// in — and it is surface-local because that is the convention a node's quad already uses
	// (Geometry/NodeTransform.h's `BoundingRadius`).
	Size<SurfaceSpace> Extent{};

	// Per-node alpha, which decision 60 is careful to say is not a group fade. A group's own opacity
	// is this field on the `DrawGroup` item, applied to the flattened result, and that difference is
	// the entire content of that decision.
	float Opacity = 1.0F;

	// Corner rounding, in `Extent`'s space so that it deforms with the quad rather than being
	// re-derived per frame from a device-space number that a scale animation invalidates.
	float Radius = 0.0F;

	Material Dress = Material::None;

	// How far the item sits off what is behind it, which the renderer draws as a shadow around this
	// item's quad. It is a field beside `Dress` rather than more enumerators inside it for
	// World/Elevation.h's reason: a glass panel casts a shadow too.
	//
	// **It is the emitting node's own, and it emits on its own.** Decision 99 places a dressing on the
	// node's own extent whatever the node's kind, and decision 95 puts both dressings in one slot
	// because they are orthogonal — so a node lifted but dressed in nothing is a `DrawDressing` whose
	// whole content is this field. That is the overview thumbnail: the tile's shadow, then the window
	// on top of it from the expansion, in that order because preorder is the painter's order. The
	// shadow is *not* placed around the referenced subtree's screen-space bound, which decision 99
	// rejects as either a render target on every tile or one shadow per subsurface.
	//
	// **A group takes it along with the opacity**, since both belong to the flattened result, and the
	// member it was taken from does not draw it again.
	//
	// Decision 104 is what the renderer derives from it: a height under one light, drawn as an
	// analytic rounded rect rather than as a blurred silhouette, which is why this costs no pass of
	// its own and stays off decision 34's quality ladder.
	Elevation Lift = Elevation::None;

	// What the item's texels or components mean as light. Per item rather than per request because a
	// composite mixes content that arrived in different states — decision 47 is that untagged content
	// is sRGB by rule, and a frame is where sRGB, PQ, and linear content sit side by side.
	ColorState Color = ColorState::Srgb();

	// How this item's *source* texels map onto device pixels, all the way from the buffer: the surface
	// adapter and the node chain composed, classified by Geometry/AxisTransform.h.
	//
	// **Derived on the side that can derive it exactly, because the renderer cannot.** Decision 56's
	// sharpness path asks whether the resample is a no-op and decision 67 makes settled geometry land
	// on the device grid unconditionally, so the common case — a window sitting still — is
	// resample-free and must take the sharp path. Recovering that from four floats means comparing
	// floats for equality, which is a guess; the producer knows because it held the transform that
	// reduced. All-false is the honest answer for a node whose chain did not reduce to an
	// `AxisTransform` at all, which is what `TransformClass` already means by default.
	TransformClass Sampling;

	// No blend flag, deliberately, where Seam/Presenter.h has one. A presenter needs it because KMS
	// has a property to program; a renderer has every input the flag would carry — the alpha mode in
	// `Color`, the opacity above, whether the source format has alpha at all — so a flag here would be
	// a fourth answer that can disagree with the three it was derived from.

	friend bool operator==(const DrawItem&, const DrawItem&) = default;
};

// One composite: everything a renderer needs to fill one target once.
struct RecordRequest
{
	// Index into the set last handed to `BindTargets`, which is `IPresenter::AcquireTarget`'s answer
	// and therefore the presenter's numbering. One numbering rather than two, so that a renderer and a
	// presenter cannot disagree about which image is being talked about.
	std::uint32_t Target = 0;

	RenderMode Mode = RenderMode::Planned;

	// Decision 34's quality tier: which structure every gathering material's pass chain has this
	// frame. Seam/Dressing.h holds what it means.
	//
	// **Per frame, beside the mode, rather than per binding beside the output's colour state.**
	// Decision 116 moved that state to `BindTargets` because the target end of a conversion decides
	// which pipelines have to exist and a frame may not compile; a tier decides a loop count and an
	// image extent and opens no pipeline, so the argument does not reach it. What decides it instead
	// is decision 63: the party that expands damage for a material must be using the same tier as the
	// party that draws it, and the only arrangement that guarantees that is for the tier to travel on
	// the request the damage was expanded for.
	//
	// Decision 34's stickiness — down quickly, up slowly, never during an animation — is a rule on
	// whatever *chooses* a tier, which is a startup probe nobody has written. Until then this is
	// `High` from one end of a session to the other.
	Tier Quality = Tier::High;

	// `Budget::Generation()`, stamped onto whatever timestamp query this frame writes so that a sample
	// resolving after a mode set can be dropped rather than filed against an output it does not
	// describe. It travels in because the query is written here and read frames later; see
	// Frame/Budget.h's `ObserveGpu`.
	std::uint32_t CostGeneration = 0;

	// No colour state here. *(Moved to `BindTargets` 2026-08-22.)* What the composite is encoded to is
	// the output's, a renderer is per output, and it is what decides which pipelines have to exist —
	// so it is stated once where a renderer is allowed to act on it rather than once per frame where
	// it cannot.

	// What must be redrawn in *this target*, in its device grid.
	//
	// **Per target and not per output, and a ring is why.** A target reached by `AcquireTarget` may be
	// two or three frames old, so what is stale in it is the union of every damage since it was last
	// drawn — the buffer-age question. Accumulating it belongs to the caller, which is also the party
	// decision 35 obliges to accumulate across a *skipped* frame rather than losing it.
	//
	// Empty means nothing changed and the renderer may return without drawing. That is not the same as
	// the caller skipping the frame: a target that is stale by age still needs the composite even when
	// no content moved, and only the caller can tell those apart.
	Region<DeviceSpace> Damage;

	// Bottom first, preorder. The storage is the caller's and lives exactly as long as the call —
	// Frame's own arena, sized by the admitted plan, because decision 36 forbids allocating here.
	std::span<const DrawItem> Items;
};

// What a recording produced.
struct Submission
{
	// What the presenter waits for before these pixels are read. Immediate where the content was
	// finished on the CPU before the call returned, which is every frame `Blit` produces.
	SyncPoint Point;

	// The CPU half of decision 29's `C`, measured by the only party that knows where recording started
	// and stopped — and, under chunking, that it was several spans rather than one. It is filed the
	// moment the call returns, per Frame/Budget.h's `ObserveCpu`, which is why it comes back by value
	// rather than being collected later.
	Duration RecordCost{};
};

// The GPU half of `C`, resolved late.
//
// It carries its own mode and generation because neither is knowable at collection time: timestamps
// are read back some frames after the submission that wrote them, by which point the caller has
// rendered other frames in other modes and may have reconfigured the output underneath both.
struct GpuCost
{
	Duration Cost{};
	std::uint32_t Generation = 0;
	RenderMode Mode = RenderMode::Planned;

	friend constexpr bool operator==(GpuCost, GpuCost) noexcept = default;
};

class IRenderer
{
public:
	IRenderer() = default;

	virtual ~IRenderer() = default;

	// Neither copied nor moved, and here the reason is stronger than `IPresenter`'s. Docs/Architecture
	// .md#device-migration puts every device handle behind a unit that tears down and rebuilds whole,
	// with no global device and no static pipeline handles — this is that unit, so a renderer is
	// replaced by being destroyed and constructed, which decision 41 already has the composition root
	// doing on every boot.
	IRenderer(const IRenderer&) = delete;
	IRenderer& operator=(const IRenderer&) = delete;
	IRenderer(IRenderer&&) = delete;
	IRenderer& operator=(IRenderer&&) = delete;

	// Take up the presenter's target set, importing whatever each one needs to be drawn into.
	//
	// **Unbounded and allocating, by contract.** It imports dmabufs, creates images and views, and may
	// build pipelines against a format it has not seen. It runs where `TargetsInvalidated` and
	// `Reconfigured` run — an output mid-transition is not presenting — and never inside the frame
	// section.
	//
	// **A renderer refuses what it cannot bind rather than assuming.** Seam/RenderTarget.h carries
	// discriminated memory precisely so that this is a branch somebody wrote: `Blit` handed a dmabuf
	// and a Vulkan device handed a CPU mapping are both composition-root miswirings, and both are
	// `EINVAL` here rather than a cast that happens to work on the machine it was written on. A format
	// or modifier the device cannot import is the same answer for the same reason. `ENOMEM` is the
	// other real one, and a caller that sees it has an output it cannot draw rather than a frame it
	// cannot draw.
	//
	// Binding a new set implies releasing the old one. Partial success is not expressible on purpose:
	// a set is what `AcquireTarget` indexes into, so half of one is not a smaller set, it is a
	// numbering with holes in it.
	//
	// **`output` is what the composite is encoded to, and it is here rather than on a request because
	// a renderer is per output.** *(Moved from `RecordRequest` 2026-08-22.)* The composition root says
	// so in as many words — a writer is bound to one presenter's target set for as long as that set
	// exists — so the output's colour state is constant for the life of a binding, and stating it per
	// frame was a per-frame restatement of a per-binding fact. What made it worth moving is that the
	// target end of every colour conversion decides which pipelines exist: a variant an item wants and
	// does not find is a refused frame, since decision 62 forbids compiling inside one, so the set has
	// to be built while the state is known and this is the only call that is allowed to be slow. It is
	// not on `RenderTarget` for that file's own reason: a copy per target is a second place to be
	// wrong when a reconfiguration changes one and not the others.
	[[nodiscard]] virtual Result<void> BindTargets(std::span<const RenderTarget> targets, ColorState output) = 0;

	// Drop everything imported from the last target set.
	//
	// Called on `TargetsInvalidated`, and it has to be a verb rather than a consequence of the next
	// bind: the descriptors in those targets are the presenter's and are about to be closed, so the
	// imports must go before the new set exists rather than when it arrives. After it, `Record`
	// refuses until something is bound.
	virtual void ReleaseTargets() noexcept = 0;

	// Draw one composite into one target and submit it.
	//
	// **This is `C` on the CPU device, so its cost is the contract.** It records and submits and does
	// not wait for the GPU *where the GPU is a device that can be waited on separately* — the whole
	// reason Seam/SyncPoint.h is a timeline point rather than a fence is that the present can be
	// issued against work that has not run. Nothing in here may allocate, take a device-wide lock, or
	// block on another output's work.
	//
	// **The qualification is decision 108 and it is narrower than it sounds.** *(Revised 2026-08-22.)*
	// A device that cannot export a timeline — lavapipe, measured, which is decision 40's permanent
	// floor tier — has no descriptor to name in a `SyncPoint`, and an invalid one there means *nothing
	// to wait for*. So such a renderer waits for its own submission here and reports an immediate
	// point, which is accurate rather than a concession: its queue runs on the CPU this call is
	// already on, so there is no second processor for a point to overlap with and the wait is work
	// that had to happen inside `C` either way. `Submission::RecordCost` on that device therefore
	// includes rasterization, which is the honest figure for it.
	//
	// **Chunking is expressible and needs nothing added.** Decision 29's contingency splits an
	// output's work at submission boundaries; that is several calls against the same held target with
	// the same `Damage`, the last one's `Point` being what the present waits on. What makes that
	// legal is that the caller holds the target across all of them, which it does — a target is
	// released by being presented, not by being recorded into.
	//
	// The failure vocabulary is `Present`'s, because the same conditions reach it: `EBUSY` for a
	// transient refusal, `ENODEV` or `EACCES` where the device is gone — device loss is real here in a
	// way it is not for a presenter, and it is the composition root's problem rather than the loop's —
	// and `EINVAL` where the request names an unbound target or an item the renderer cannot express,
	// which is a bug in the caller rather than a condition to retry.
	[[nodiscard]] virtual Result<Submission> Record(const RecordRequest& request) = 0;

	// Has the work behind this point finished?
	//
	// Decision 35's record-time check reads it before starting a frame, so it is a poll and never a
	// wait: a blocking answer here would put the GPU's schedule on the `SCHED_FIFO` thread, which is
	// the priority inversion Docs/Architecture.md#software-rendering-is-the-floor-tier is built to
	// avoid. An immediate point is complete by definition, which is what makes `Blit` answer this
	// without owning a timeline at all.
	[[nodiscard]] virtual bool IsComplete(SyncPoint point) const = 0;

	// Take whatever GPU costs have resolved since the last call, newest last, and return how many were
	// written. Never more than `into.size()`; the rest wait for the next call.
	//
	// A pull rather than a signal, and the caller's buffer rather than the renderer's: the frame loop
	// drains this where it drains everything else, once per iteration, and Core/Signal.h would make it
	// an emission from whichever thread the driver finished on. A renderer with no GPU device — `Blit`,
	// and the null renderer the schedulability sweep runs — writes nothing and returns zero forever,
	// which is the correct report rather than a stub: its work has no second device to cost.
	[[nodiscard]] virtual std::size_t CollectCosts(std::span<GpuCost> into) = 0;
};

[[nodiscard]] constexpr std::string_view Name(RenderMode mode) noexcept
{
	switch (mode)
	{
		case RenderMode::Planned:
			return "planned";
		case RenderMode::Floor:
			return "floor";
	}

	return "unknown";
}

// Prints as device[(0, 0) (64, 0) (64, 32) (0, 32)], and a projected one carries its weights. The
// bound is not printed: a reader chasing a misplaced window wants the corners, and the bound is what
// they would compute from them anyway.
template<>
struct std::formatter<Quad>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const Quad& quad, Context& context) const
	{
		auto out = std::format_to(context.out(), "{}[", DeviceSpace::Name);

		for (std::size_t index = 0; index < 4; ++index)
		{
			out =
				std::format_to(out, "{}({}, {})", index == 0 ? "" : " ", quad.Corners[index].X, quad.Corners[index].Y);
		}

		out = std::format_to(out, "]");

		for (std::size_t index = 0; index < 4; ++index)
		{
			if (quad.Weights[index] != 1.0F)
			{
				return std::format_to(
					out, " w({}, {}, {}, {})", quad.Weights[0], quad.Weights[1], quad.Weights[2], quad.Weights[3]
				);
			}
		}

		return out;
	}
};

// The contract everything downstream assumes. A draw list is built into an arena on the frame thread
// and read by a renderer that did not build it, so the items have to be copyable bytes with no
// lifetime of their own.
static_assert(std::is_trivially_copyable_v<Quad> && std::is_standard_layout_v<Quad>);
static_assert(std::is_trivially_copyable_v<DrawItem>, "An item is copied into an arena, never owned behind one");
static_assert(std::is_trivially_copyable_v<DrawContent>, "The variant is only as trivial as its alternatives");
static_assert(std::is_trivially_copyable_v<GpuCost> && std::is_trivially_copyable_v<Submission>);
static_assert(std::formattable<Quad, char>);

// The default item draws nothing at all — an empty quad, no extent, no content, and the identity
// colour state — which is what a half-built one should be. *(Revised 2026-08-22.)* It was an opaque
// black solid until `DrawDressing` took the variant's first position, and the direction is the one
// World/Node.h defaults every field in: a record nobody finished is invisible rather than a black
// rectangle over somebody's screen.
static_assert(std::holds_alternative<DrawDressing>(DrawContent{}));
static_assert(DrawItem{}.Shape.Bounds().IsEmpty());
static_assert(DrawItem{}.Dress == Material::None && DrawItem{}.Lift == Elevation::None);
static_assert(!DrawItem{}.Sampling.MapsRectangles(), "A transform that did not reduce classifies as nothing");

// A rectangle round-trips through the quad, which is the group item's whole path.
static_assert(
	Quad::FromRect({ { 10.0F, 20.0F }, { 64.0F, 32.0F } }).Bounds() ==
	Rect<DeviceSpace>{ { 10.0F, 20.0F }, { 64.0F, 32.0F } }
);
static_assert(Quad::FromRect({ { 10.0F, 20.0F }, { 64.0F, 32.0F } }).Corners[2] == Point<DeviceSpace>{ 74.0F, 52.0F });

// Two composites are two names, which is the assertion that fails first if the mode is ever widened
// into a flag set.
static_assert(Name(RenderMode::Planned) != Name(RenderMode::Floor));
