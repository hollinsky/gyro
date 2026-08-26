#pragma once

#include <span>
#include <string>
#include <vector>

#include "Bdf.h"
#include "Core/Result.h"

// The back end: parsed fonts in, one C++ translation unit out.
//
// **The coverage is declared here rather than taken from the font.** Spleen carries 969 glyphs at its
// larger sizes, and 256 of them are Braille patterns; baking everything would put a quarter of a
// megabyte of dot patterns into a compositor that will never ask for one. What is baked is what the
// four callers need — a recovery console, a boot message, a debug label and a passphrase prompt —
// which is printable Latin, the Latin-1 supplement, and the box drawing and block elements a console
// draws its own frames with.
//
// A face keeps whatever part of that it actually has: 5x8 carries thirteen box-drawing characters out
// of the block, and the bake cuts runs around the holes rather than padding them, so a size that has
// fewer glyphs costs fewer bytes rather than the same.

struct CodeRange
{
	char32_t First;
	char32_t Last;
};

// Printable ASCII, the Latin-1 supplement, box drawing, block elements. Not Latin Extended-A, not
// Braille, not the arrows: each is a block somebody can add here with a caller to point at.
inline constexpr CodeRange RequestedCoverage[] = {
	{ 0x0020, 0x007E },
	{ 0x00A0, 0x00FF },
	{ 0x2500, 0x257F },
	{ 0x2580, 0x259F },
};

struct FaceSource
{
	// `spleen-8x16`, which is what `Face::Name()` reports and what a log line names.
	std::string Name;
	BdfFont Font;
};

// One file's text, ready to be written. Sorted by cell height inside, because `Text/Font.h`'s
// `Nearest` breaks its tie towards the smaller face and that is only the smaller face if the ladder
// ascends — a property worth holding by construction rather than by the order of a CMake list.
[[nodiscard]] Result<std::string> Bake(std::vector<FaceSource> faces, std::string* diagnostic);
