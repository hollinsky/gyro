#pragma once

#include <type_traits>

#include "Core/ColorState.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"

// What a node draws, as the element types of the per-kind content runs.
//
// **A node carries a kind and one index, and the kind says which run that index is a position in.**
// Docs/Decisions.md decision 95 puts content here rather than in a union inline in the node record,
// which is decision 90's rejected inline spring at a smaller scale: the common node is a container
// or a group with nothing in the union, and a walk that dragged twenty-four bytes of payload through
// cache in order not to use it pays for that on every node of every frame. So a container names
// nothing, and this file holds only what a leaf that draws has.
//
// **Nothing here names a container of these records**, for the reason World/Node.h gives about the
// node run: Publication carries a run as an offset, a count, and the writer's element size and
// alignment, so adding a field to one of these does not touch the waist.

// An image the renderer samples.
//
// **A live client surface and a compositor-owned snapshot are one record**, where decision 51's
// sketch made them two node kinds. Docs/Animation.md#exit-pixels turns a closing window's surface
// into a snapshot *while the exit spring is running*, so two kinds means the node changes kind
// mid-transition — and what that costs is on screen rather than in the encoding: a menu collapsing
// back toward the control that opened it restarts its collapse at the moment the client releases its
// buffer, dozens of times a minute inside one application. Seam/Renderer.h's `DrawTexture` makes the
// same collapse one layer down and for the same case; its comment is the other half of this one.
//
// Who owns the pixels and how long they live is a question for the half of the renderer that
// imported them, which is what Core/Texture.h's single id space already says.
struct ImageContent
{
	// Null draws nothing and says nothing, per Core/Texture.h — a stale id is a lifetime bug, and a
	// frame is not where a lifetime bug gets reported.
	TextureId Texture;

	// The texels sampled. Real-valued because `wp_viewport`'s `src` is `wl_fixed` and an atlas slot's
	// content may sit at a half-texel inset. **Empty means the whole image**, which is `DrawTexture`'s
	// convention rather than a second one — the two records are read against each other constantly and
	// a sentinel that meant "sample nothing" in one of them and "sample everything" in the other is a
	// black window nobody can attribute.
	Rect<BufferSpace> Source;

	// The window geometry rect: what the frame treatment applies to, per decision 96.
	//
	// **gyro rounds every window to at least a floor radius, and the rect is what makes that safe.** A
	// client that draws its own shadow sets `xdg_surface.set_window_geometry` to its visible bounds —
	// it must, or every compositor tiles it with gaps — so rounding the node's whole extent would cut
	// a corner out of the shadow margin, several pixels away from anything a person can see, leaving
	// the window itself square. **Empty means the whole extent**, which is the client that never sets
	// it — most of Xwayland, and also the population with no client-drawn shadow — and there the
	// rounding is simply right.
	//
	// **It is not a crop.** Decision 96 rejects cropping to this rect: the rounded-rect test is bounded
	// to it and fragments outside it pass through untouched, so nothing a client drew outside its
	// declared bounds is discarded. The failure mode is gyro rounding a rect that was not the window,
	// which is visible and local, rather than gyro silently deleting content on the strength of a hint
	// it cannot verify for a dmabuf.
	Rect<SurfaceSpace> Frame;

	// What this image's texels mean as light. See below for why it is here rather than on the node.
	ColorState Color;

	friend constexpr bool operator==(ImageContent, ImageContent) noexcept = default;
};

// A colour fill: the background before or without a wallpaper, the firmware background colour
// decision 37's continuous handoff continues into, the letterbox fill when a wallpaper's aspect does
// not match the mode. Mostly gyro's own rather than shell vocabulary — a shell wanting a coloured
// rectangle uses its own surface.
//
// It survives against a 1x1 texture stretched because of where it is needed: `Blit`, the console
// renderer of decision 79, paints a fill with no device, no import, and no texture lifetime, and
// `Blit` is what is running at the moment the picture handed over by firmware has to stay continuous.
struct SolidContent
{
	// Four components meaning whatever `Color` says they mean, which is the same rule a buffer's
	// texels are read under.
	float Red = 0.0F;
	float Green = 0.0F;
	float Blue = 0.0F;
	float Alpha = 1.0F;

	// **The colour state is here rather than on the node.** A container has no pixels and so has
	// nothing that means anything as light; eight bytes on every node in the tree to say so is the
	// padding decision 90 refused for springs, arriving on a different field. Nine workspaces of
	// containers and groups is what that would cost, on the walk that has to finish inside a frame.
	ColorState Color;

	friend constexpr bool operator==(SolidContent, SolidContent) noexcept = default;
};

// The contract the publication boundary assumes: these cross as bytes at an offset and are
// reconstituted by a reader that never calls a constructor, exactly as World/Node.h and Core/Handle.h
// are.
static_assert(std::is_trivially_copyable_v<ImageContent> && std::is_standard_layout_v<ImageContent>);
static_assert(std::is_trivially_copyable_v<SolidContent> && std::is_standard_layout_v<SolidContent>);
static_assert(std::is_aggregate_v<ImageContent> && std::is_aggregate_v<SolidContent>);

static_assert(sizeof(ImageContent) == 48, "A texture id, two rects, and a colour state");
static_assert(sizeof(SolidContent) == 24, "Four components and a colour state");
static_assert(alignof(ImageContent) == 4, "Nothing here is wider than a float or a slot index");
static_assert(alignof(SolidContent) == 4);

// A record nobody finished draws nothing, and every field says so the same way. The texture names no
// image; both rects are empty, which is the *whole* image and the *whole* extent rather than a
// degenerate sample and an unrounded corner; the colour state is what untagged content is by rule.
// Asserted rather than assumed, because these defaults are what a partially-written publisher
// produces, and the direction chosen everywhere here is the one whose failure is a missing image
// rather than a wrongly cropped one.
static_assert(ImageContent{}.Texture.IsNull(), "An unfinished image names no pixels");
static_assert(ImageContent{}.Source.IsEmpty(), "Empty is the whole image, per DrawTexture");
static_assert(ImageContent{}.Frame.IsEmpty(), "Empty is the whole extent, which is the client that sets no geometry");
static_assert(ImageContent{}.Color == ColorState::Srgb(), "Untagged content is sRGB by rule");

// Opaque black, which is `DrawSolid`'s default exactly — the two spellings of a fill nobody set have
// to agree, or a node round-tripping through the seam changes colour on the way. Opaque rather than
// transparent for World/Node.h's reason about `Opacity`: a fill that should not be drawn is a bug
// somebody can see, and an invisible one is a bug somebody bisects for.
static_assert(SolidContent{}.Red == 0.0F && SolidContent{}.Green == 0.0F && SolidContent{}.Blue == 0.0F);
static_assert(SolidContent{}.Alpha == 1.0F, "A fill nobody finished is visible");
static_assert(SolidContent{}.Color == ColorState::Srgb());
