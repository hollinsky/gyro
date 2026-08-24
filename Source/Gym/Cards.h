#pragma once

#include <cstdint>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/NodeTransform.h"
#include "Scene/Store.h"

// The scene the card gym drives: one imported image, drawn four times, each copy doing one thing to it.
//
// **Gym/Lanes.h's argument, applied to sampling instead of to springs.** A lane exists so that a wrong
// picture becomes *which channel*; these four exist so that a wrong image becomes *which stage*. An
// image reaching the panel has been decoded, filtered, folded and clamped, and each of those fails as a
// picture that is merely a bit off — so the card is drawn once with nothing done to it and three more
// times with exactly one thing done, and the reference is always in the same frame as the suspect.
//
// **The still copy is authored at its own texel size and never moves**, which is what makes it a
// reference rather than a fourth suspect: at unit output scale it samples one texel per pixel, so
// anything soft, shifted or discoloured on that copy is the import and not the resample. The other
// three are laid out as fractions of the output, for the reason Gym/Lanes.h gives — the instrument is
// the same shape on a 1080p panel and a 4K one.
//
// **Every copy is premultiplied and that is a constraint rather than a choice.** `Blit/Blit.cpp`
// refuses an item whose colour state is not the output's — including its alpha mode — and one refused
// item loses the whole frame, so a straight-alpha node in this scene would empty the very backend the
// gym is most useful on. Gym/Card.h still draws both spellings, because the straight one is what the
// day a client commits one needs, and its unit test is where that is checked meanwhile.

// How large the card is drawn, in texels. Big enough that its one-texel checkerboard and stripes are
// several pixels of panel wide after the minifying copy has shrunk them, and small enough that the
// still copy is a quarter of a 1080p panel's height rather than all of it.
inline constexpr std::int32_t CardTexels = 256;

// The two ends of the scale channel. The small end is well into minification rather than a gentle
// shrink: the checkerboard patch has to *resolve* for the gamma diagnostic to say anything, and a card
// at nine tenths filters almost nothing.
//
// Prefixed, because Gym/Lanes.h names the lanes' own ends and one driver includes both headers.
inline constexpr Vector3<float> CardScaleSmall{ 0.28F, 0.28F, 1.0F };
inline constexpr Vector3<float> CardScaleFull{ 1.0F, 1.0F, 1.0F };

// The two ends of the opacity channel. Dim rather than invisible, for `CardScaleSmall`'s reason in
// Gym/Lanes.h: a copy animated to nothing and a copy that failed to draw are the same picture.
inline constexpr float CardFadeDim = 0.15F;
inline constexpr float CardFadeFull = 1.0F;

// The four copies, as the ids a gym drives them through.
struct CardScene
{
	// The one node everything hangs off, parked at the output's origin so every position below is read
	// against the output rather than against global space.
	EntityId Stage{};

	// A fill over the whole output. The card's own ground is a different dark, so a card that failed to
	// import is a hole in this rather than a black panel — and the half-alpha block on the card is read
	// against this, which is why it is a colour and not black.
	EntityId Backdrop{};

	// One texel per pixel, unmoved. The reference.
	EntityId Still{};

	// Driven on the scale channel, between a fraction and full size. The minifying end is what makes
	// the checkerboard patch resolve, so this is the copy the gamma diagnostic is read on.
	EntityId Scaled{};

	// Driven on opacity. The fold, against the backdrop.
	EntityId Faded{};

	// Driven on translation, along the row below the others. Subpixel phase: a filter that snaps to
	// whole texels shows here as a card that stutters between positions while the others are smooth.
	EntityId Sliding{};

	// Where the sliding copy sits at each end of its travel, in the stage's space. Derived from the
	// extents actually authored rather than from the fractions they came from, for `LaneScene`'s reason.
	Vector3<double> SlideNear{};
	Vector3<double> SlideFar{};
};

// Author the four copies into an empty store, against the first output the store carries.
//
// `texture` is already adopted — the gym mints it before it authors, because a node naming an id that
// no renderer holds draws nothing and says nothing, which is the one failure Core/Texture.h makes
// deliberately silent and therefore the one worth not authoring into.
//
// Refused with a sentence where the store carries no outputs or the first has no bounds, which is
// `AuthorLanes`' refusal for its reason: a scene laid out against a degenerate rectangle is an output
// that is black for a cause nobody can see from the frame.
[[nodiscard]] Result<CardScene> AuthorCards(SceneStore& scene, TextureId texture);
