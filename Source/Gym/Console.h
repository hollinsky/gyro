#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Core/Result.h"
#include "Geometry/NodeTransform.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Text/Font.h"

// The instrument the recovery console's typography is chosen with: every rung of the ladder, the face
// three row counts would pick on *this* panel, and what a glyph looks like when it is not drawn at its
// own size.
//
// **It exists because `Text` had no callers.** `Text/Label.h` bakes words and `Text/Raster.h` lays out
// cells, and until this gym every claim either of them makes was a claim about a buffer in a unit
// test. Nothing had put a glyph on a panel. The console is what will, and the console is also a PTY, a
// scrollback and a line discipline — so the first look at whether a letter is legible would have come
// wrapped in three other things that can be wrong, on the one machine where being unable to read the
// screen is the whole problem. This is that look, with none of them.
//
// **The specimens are sized in the panel's own pixels, which is `Gym/Pointer.h`'s rule rather than
// `Gym/Lanes.h`'s.** A lane is a fraction of the output so the instrument is the same shape on a 1080p
// panel and a 4K one; a glyph is the opposite, because a bitmap face does not scale and the only
// question worth asking is how many device pixels a letter is made of. So every label here is authored
// at `texels / density`, which is `Scene/Cursor.h`'s arithmetic and lands one texel on one device
// pixel — and a label that comes out soft, doubled or shifted is the resample rather than the font.
//
// **The scaled pair is the argument for the ladder, on screen.** Decision 38 has the console keep a
// glyph at every size instead of scaling one, and the reason is a sentence that is much cheaper to
// believe with the picture beside it: the same label is drawn once at its own size, once enlarged and
// once reduced, and what happens to it at the second and third is what a ladder costs six bakes to
// avoid.
//
// **Nothing here moves, and that is the second thing it measures.** This is `settle`'s shape with a
// picture in it: the scene is authored once and answers `Never()` forever, so a console gym that keeps
// waking the loop is a wake somebody authored by accident. Text is the one content in gyro that has no
// reason to animate, which makes it the honest subject for that claim.

// How many rows a console might want down the panel. The face each of these resolves to is what the
// pick row shows, drawn *in* that face — so the specimen that cannot be read is the answer rather than
// a caption about it.
//
// **Three counts rather than one, because this gym does not settle the policy.** What the console will
// actually ask for is an Open.md question, and the instrument for it is the three candidates side by
// side on the panel a person is looking at: 30 is a boot log you read from across a room, 60 is a shell
// somebody is working in, and 45 is what the argument is usually about.
inline constexpr std::int32_t ConsoleRowCounts[3]{ 30, 45, 60 };

inline constexpr std::size_t ConsoleRowChoices = 3;

// How far off its own size the scaled pair is drawn.
//
// **Neither factor is a whole number, and the first run of this gym is why.** At exactly double, a
// one-bit glyph is not degraded at all: every texel becomes a two-by-two block and the enlarged copy
// comes out as clean as the reference, which is a specimen arguing *against* the ladder decision 38
// rests on. What a bitmap actually loses it loses at the factors nothing lines up on — a stem one
// texel wide becomes one pixel here and two there along the same letter — so the pair is deliberately
// off every integer and off a half.
inline constexpr double ConsoleEnlargement = 1.7;
inline constexpr double ConsoleReduction = 0.6;

// The most rungs the ladder row has entities for. Spleen ships six and `Text/Font.h`'s ladder is
// whatever the bake produced, so this is a bound rather than a count — a seventh face is an entity that
// is not authored rather than an overrun, and `ConsoleScene::Rungs` is what a reader looks at.
inline constexpr std::size_t MaxLadderRungs = 12;

// The scene: three still blocks down the panel.
struct ConsoleScene
{
	// Everything hangs off this, parked at the output's origin.
	EntityId Stage{};

	// A dark field. Not black, so a label that failed to import is a hole in something rather than more
	// panel — `Gym/Cards.h`'s reason, and the same one that makes the console's own ground not black.
	EntityId Backdrop{};

	// One specimen per baked face, at its own cell size, top to bottom in the ladder's order. The row
	// that answers *which of these can I read from here*.
	std::array<EntityId, MaxLadderRungs> Ladder{};
	std::size_t Rungs = 0;

	// One per entry in `ConsoleRowCounts`, drawn in the face that count picks on this output.
	std::array<EntityId, ConsoleRowChoices> Picks{};

	// What `Nearest` answered for each count, so a test can assert the specimen was drawn in the face
	// the policy picked rather than in whichever one the layout happened to reach.
	std::array<const Face*, ConsoleRowChoices> Picked{};

	// A newline, a tab and a line longer than the ones around it, in the middle count's face. This is
	// the whole of `Text/Raster.h`'s layout as a picture: cells that do not line up in a column, or a
	// tab that lands somewhere other than the next multiple of eight, are visible here and nowhere else
	// in this scene.
	EntityId Layout{};

	// The same string with an opaque background, which is the two-word select `Text/Label.h` does. A
	// caption over a backdrop and a caption over its own block are the two things the console draws,
	// and the second is what a status line is.
	EntityId Reversed{};

	// The layout specimen again, off its own size in both directions. Nothing is animated: what is being
	// looked at is a still frame, because a staircase that is wrong is wrong before it moves.
	EntityId Enlarged{};
	EntityId Reduced{};
};

// Author the specimens into an empty store, against the first output the store carries.
//
// `textures` is what each label's pixels are adopted through, and this gym never retires them: the
// scene is authored once and lives as long as the store does. The images themselves do not outlive the
// call — `Scene/Textures.h` reads the bytes during `Adopt` and the registry holds them afterwards.
//
// Refused with a sentence where the store carries no outputs, the first has no bounds, or a label
// refused to bake — which is `AuthorCards`' refusal for its reason: a scene laid out against a
// degenerate rectangle is an output that is black for a cause nobody can see from the frame. The
// output's scale is not among the refusals, because `Geometry/Scale.h` has no zero to be refused.
[[nodiscard]] Result<ConsoleScene> AuthorConsole(SceneStore& scene, ITextures& textures);
