#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"

// The netpbm PAM header, both directions, and the decode that turns one into pixels an author can
// adopt.
//
// **Both directions rather than a reader alone, which is the whole reason `FormatPamHeader` is
// here.** The writer has been in [Virtual/Pam.h](../Virtual/Pam.h) since frame dumps existed and it
// formats the header inline; a reader that stated the same eight fields again would agree with it on
// the day it was written and disagree after somebody edited one of them. That is
// [Trace/Schema.h](../Trace/Schema.h)'s argument one format over — gyro writes this by hand and
// nothing else in the world checks it — and the resolution is the same: the field names live in one
// place both directions read. **The dump writer has not been moved onto it yet**, which is a loose
// end rather than a design: it is the emit half that belongs here and the file half — the temporary,
// the rename, the row encoding at the source's own depth — that belongs there.
//
// **It is in `Core` rather than in `Virtual` because the reader's caller is `Scene`.**
// [Scene/Background.h](../Scene/Background.h) decodes a background image on the dispatch thread and
// `Scene` is portable and may name neither `Virtual` nor `Seam`. Below both waists is where a type two
// modules on opposite sides need lives (decision 87), and this one is below because the pixels it
// produces are exactly what `Scene/Textures.h` adopts: eight bits a channel in a little-endian word.
//
// **PAM rather than PNG, and the trade is the one `Virtual/Pam.h` already made, read in the other
// direction.** Decoding a PNG means zlib inside the one process on the machine that holds DRM master
// and runs `SCHED_FIFO`, in order to save disk on a file that is read once. A 4K background is 33 MB
// as a PAM, which is a cost paid by a filesystem rather than by a person waiting for a frame — and
// every tool that produces one, ImageMagick included, writes PAM without being asked twice.
//
// **Nothing here is frame-thread code.** It allocates the image, and `Core/FrameSection.h` aborts on
// an allocation inside a frame. The decode happens where a texture is minted, which is dispatch-side
// by construction.

// SPEC: the most texels this file will decode, which is a ceiling against a malformed header rather
// than a size anything is expected to reach.
//
// A width and a height are two numbers in a file somebody else wrote, and their product is the
// allocation. Sixty-seven million is an 8K panel with room over it, and it bounds the words at 256 MB
// — refused loudly at the header instead of reaching the allocator as a length nobody checked.
inline constexpr std::size_t MaxPamTexels = std::size_t{ 1 } << 26;

// SPEC: the most bytes `PamImage::Read` will take from a descriptor.
//
// The same argument for the case where there is no header to check yet: a descriptor's length is not
// known before it is read, and a pipe has none at all. Half a gigabyte is every legitimate PAM this
// compositor will ever be handed and is a bound on what a hostile one costs.
inline constexpr std::size_t MaxPamBytes = std::size_t{ 512 } << 20;

// What a PAM header says, in the four fields that change how the payload is read.
//
// `TUPLTYPE` is deliberately not among them. It is optional in the format and it is prose: `DEPTH` is
// what says how many samples a tuple has and therefore where the next row starts, so a file whose
// depth and tuple type disagree is read by its depth. A reader that trusted the string would be one a
// four-channel image labelled `RGB` walks off the end of.
struct PamHeader
{
	std::int32_t Width = 0;
	std::int32_t Height = 0;

	// Three or four: RGB or RGB with alpha. Nothing else is decoded — a grayscale or a bilevel PAM is
	// a real file and is not a thing gyro is ever handed, and refusing it is better than inventing a
	// colour for it.
	std::int32_t Depth = 4;

	// The largest sample value, which is what a sample is a fraction *of*. One to 65535 per the
	// specification; at or below 255 a sample is one byte and above it two, big-endian.
	std::uint32_t MaxValue = 255;

	[[nodiscard]] constexpr bool HasAlpha() const noexcept { return Depth == 4; }

	[[nodiscard]] constexpr bool IsWide() const noexcept { return MaxValue > 255; }

	friend constexpr bool operator==(PamHeader, PamHeader) noexcept = default;
};

// The header as bytes, `ENDHDR` and its newline included, so that what follows is the payload's first
// byte.
//
// `note` is free text a writer wants to leave in the file — what produced the frame, and which frame
// it was. It is emitted as comments, one per line of it, because a `#` runs to the end of a line and a
// note written whole would put its own second line into the grammar as a field nobody defined. A
// reader skips them, which is what makes an annotated dump the same file as an unannotated one.
[[nodiscard]] std::string FormatPamHeader(const PamHeader& header, std::string_view note = {});

// A header read back, and where the payload starts.
struct PamHeaderView
{
	PamHeader Header{};

	// Bytes consumed, which is the offset of the first sample.
	std::size_t Length = 0;
};

// Read a header off the front of `bytes`.
//
// `EINVAL` for a magic that is not `P7`, a field that is not a number, a dimension that is not
// positive, or a header with no `ENDHDR` in it; `ENOTSUP` for a depth or a maximum value this file
// does not decode; `E2BIG` past `MaxPamTexels`.
[[nodiscard]] Result<PamHeaderView> ParsePamHeader(std::span<const std::byte> bytes);

// An image decoded into the layout an author adopts, which is `Scene/Textures.h`'s and
// `Scene/Cursor.h`'s: eight bits a channel in a 32-bit little-endian word, blue lowest, alpha in the
// top byte and already multiplied into the other three.
//
// **The accessors are `CursorImage`'s deliberately.** Both exist to be handed to
// `ITextures::Adopt(size, stride, bytes, alpha)`, and two producers of the same argument list that
// spell it differently is a call site that has to be read twice.
class PamImage
{
public:
	// Decode a whole file's bytes. Trailing bytes past the payload are ignored, which is what a file
	// with a comment or a second image after it looks like.
	//
	// `ENODATA` where the payload is shorter than the header promised — a truncated download, and the
	// one failure worth naming separately, because the picture it would otherwise produce is a
	// background that is half a wallpaper and half black.
	[[nodiscard]] static Result<PamImage> Decode(std::span<const std::byte> bytes);

	// The same, from a descriptor read to its end.
	//
	// **A descriptor rather than a path, because that is how one arrives.** A background comes from a
	// shell over the session handover, which passes descriptors for `Session/Handover.h`'s reason — the
	// party that opened the file is the party entitled to it — and the command-line flag opens the path
	// itself so that both reach one verb.
	//
	// `EFBIG` past `MaxPamBytes`, and whatever `read` answered.
	[[nodiscard]] static Result<PamImage> Read(RawFd file);

	[[nodiscard]] std::int32_t Width() const noexcept { return m_Width; }
	[[nodiscard]] std::int32_t Height() const noexcept { return m_Height; }

	// Whether the file carried an alpha channel. False is an opaque image and is `TextureAlpha::None`
	// on the far side; true is `Premultiplied`, because the multiply happened here.
	[[nodiscard]] bool HasAlpha() const noexcept { return m_HasAlpha; }

	[[nodiscard]] std::uint32_t Stride() const noexcept { return static_cast<std::uint32_t>(m_Width) * 4U; }

	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return std::as_bytes(std::span{ m_Words }); }

	// The word at a texel, for a test that asserts what the image holds. Out of range is transparent
	// black, which is `CursorImage::At`'s answer and for its reason: an assertion that ran off the edge
	// should fail on the value rather than on the process.
	[[nodiscard]] std::uint32_t At(std::int32_t x, std::int32_t y) const noexcept;

private:
	PamImage(std::int32_t width, std::int32_t height, bool alpha, std::vector<std::uint32_t> words) noexcept
		: m_Words{ std::move(words) }, m_Width{ width }, m_Height{ height }, m_HasAlpha{ alpha }
	{}

	std::vector<std::uint32_t> m_Words;
	std::int32_t m_Width = 0;
	std::int32_t m_Height = 0;
	bool m_HasAlpha = false;
};
