#include "Core/Pam.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Testing/Test.h"

// What this file is for is a picture somebody else produced arriving intact, so what it asserts is the
// two ways that fails silently: a sample decoded against the wrong maximum, which is a wallpaper that
// is uniformly too dark, and a row read at the wrong width, which is a wallpaper that shears. Both
// look like a bug in the compositor and neither reaches a log.
//
// The refusals are asserted by code rather than by prose: a truncated file and an enormous header are
// the two a hostile or half-written file arrives as, and `ENODATA` against `E2BIG` is what a reader of
// the log has to tell apart.

namespace
{
// A file, built the way netpbm builds one: the header this module formats, then samples.
[[nodiscard]] std::vector<std::byte> File(const std::string& header, const std::vector<std::uint8_t>& payload)
{
	std::vector<std::byte> bytes;

	for (const char character : header)
	{
		bytes.push_back(static_cast<std::byte>(character));
	}

	for (const std::uint8_t sample : payload)
	{
		bytes.push_back(static_cast<std::byte>(sample));
	}

	return bytes;
}

[[nodiscard]] std::uint32_t Channel(std::uint32_t word, std::uint32_t shift) noexcept
{
	return (word >> shift) & 0xFFU;
}
} // namespace

// The round trip through the one grammar, which is the property the header lives in `Core` for: what
// the writer emits is what the reader reads, so a field renamed on one side fails here rather than in
// a wallpaper somebody cannot open.
GYRO_TEST(Pam, ReadsBackTheHeaderItFormats)
{
	const PamHeader header{ .Width = 1920, .Height = 1080, .Depth = 4, .MaxValue = 255 };

	const std::vector<std::byte> bytes = File(FormatPamHeader(header), {});

	const Result<PamHeaderView> read = ParsePamHeader(bytes);

	GYRO_REQUIRE(read.has_value());
	GYRO_CHECK(read->Header == header);
	GYRO_CHECK(read->Length == FormatPamHeader(header).size());
}

// Two texels, eight bits, no alpha. Blue is the low byte and the alpha byte is opaque, which is the
// layout `Scene/Textures.h` adopts — a reader that assembled the word the other way round is a
// background whose reds and blues are swapped, and that is the one decode failure a person notices
// instantly and a test written in greys never would.
GYRO_TEST(Pam, DecodesRgbIntoTheWordAnAuthorAdopts)
{
	const std::vector<std::byte> bytes =
		File(FormatPamHeader({ .Width = 2, .Height = 1, .Depth = 3, .MaxValue = 255 }), { 10, 20, 30, 40, 50, 60 });

	const Result<PamImage> image = PamImage::Decode(bytes);

	GYRO_REQUIRE(image.has_value());
	GYRO_CHECK(image->Width() == 2);
	GYRO_CHECK(image->Height() == 1);
	GYRO_CHECK(!image->HasAlpha());
	GYRO_CHECK(image->Stride() == 8);

	GYRO_CHECK(image->At(0, 0) == 0xFF0A141EU);
	GYRO_CHECK(image->At(1, 0) == 0xFF28323CU);
}

// A PAM carries straight alpha and a texel carries premultiplied, so the multiply is the decode's job.
// Half coverage over full red is half red: skipping it is a wallpaper that is too bright wherever it
// is transparent, which is exactly where nobody is looking for a bug.
GYRO_TEST(Pam, PremultipliesAlphaIntoTheColour)
{
	const std::vector<std::byte> bytes =
		File(FormatPamHeader({ .Width = 1, .Height = 1, .Depth = 4, .MaxValue = 255 }), { 255, 0, 0, 128 });

	const Result<PamImage> image = PamImage::Decode(bytes);

	GYRO_REQUIRE(image.has_value());
	GYRO_CHECK(image->HasAlpha());
	GYRO_CHECK(Channel(image->At(0, 0), 24) == 128);
	GYRO_CHECK(Channel(image->At(0, 0), 16) == 128);
	GYRO_CHECK(Channel(image->At(0, 0), 8) == 0);
	GYRO_CHECK(Channel(image->At(0, 0), 0) == 0);
}

// Sixteen-bit samples are big-endian by specification, which is the one thing a little-endian machine
// gets wrong by doing nothing: read the other way round, 0xFFFF and 0x00FF are the same byte pair and
// full white decodes as almost black.
GYRO_TEST(Pam, ReadsWideSamplesBigEndian)
{
	const std::vector<std::byte> bytes = File(
		FormatPamHeader({ .Width = 1, .Height = 1, .Depth = 3, .MaxValue = 65535 }),
		{ 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00 }
	);

	const Result<PamImage> image = PamImage::Decode(bytes);

	GYRO_REQUIRE(image.has_value());
	GYRO_CHECK(Channel(image->At(0, 0), 16) == 255);
	GYRO_CHECK(Channel(image->At(0, 0), 8) == 128);
	GYRO_CHECK(Channel(image->At(0, 0), 0) == 0);
}

// A maximum that is neither 255 nor 65535 is legal and is what several tools emit. Scaled rather than
// shifted, so the brightest sample the file can hold is white — a shift would make a `MAXVAL 1` image
// black and a `MAXVAL 100` image a third of the brightness its author saw.
GYRO_TEST(Pam, ScalesSamplesByTheFilesOwnMaximum)
{
	const std::vector<std::byte> bytes =
		File(FormatPamHeader({ .Width = 1, .Height = 1, .Depth = 3, .MaxValue = 100 }), { 100, 50, 0 });

	const Result<PamImage> image = PamImage::Decode(bytes);

	GYRO_REQUIRE(image.has_value());
	GYRO_CHECK(Channel(image->At(0, 0), 16) == 255);
	GYRO_CHECK(Channel(image->At(0, 0), 8) == 128);
	GYRO_CHECK(Channel(image->At(0, 0), 0) == 0);
}

// Rows follow the width and not the other way round. Asserted with an asymmetric image because a
// square one passes under a transposed read, which is how this class of bug survives a test suite.
GYRO_TEST(Pam, RowsFollowTheDeclaredWidth)
{
	const std::vector<std::byte> bytes = File(
		FormatPamHeader({ .Width = 3, .Height = 2, .Depth = 3, .MaxValue = 255 }),
		{ 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6 }
	);

	const Result<PamImage> image = PamImage::Decode(bytes);

	GYRO_REQUIRE(image.has_value());
	GYRO_CHECK(Channel(image->At(2, 0), 0) == 3);
	GYRO_CHECK(Channel(image->At(0, 1), 0) == 4);

	// Off the edge is transparent black rather than the next row's first texel, which is what makes an
	// assertion that overran fail on the value instead of on the process.
	GYRO_CHECK(image->At(3, 0) == 0);
}

// Comments and unknown fields are part of the format, and a header from a newer netpbm has to open
// rather than refuse — the picture is still a picture.
GYRO_TEST(Pam, SkipsCommentsAndFieldsItDoesNotKnow)
{
	const std::string header = "P7\n# made by something\nWIDTH 1\nHEIGHT 1\nDEPTH 3\nMAXVAL 255\n"
							   "TUPLTYPE RGB\nSOMETHINGNEW 4\nENDHDR\n";

	const Result<PamImage> image = PamImage::Decode(File(header, { 9, 9, 9 }));

	GYRO_REQUIRE(image.has_value());
	GYRO_CHECK(Channel(image->At(0, 0), 0) == 9);
}

// A truncated file is a half-finished download, and it is refused rather than padded: the picture it
// would otherwise produce is a background that is a wallpaper down to a line and black below it, which
// reads as a compositor bug.
GYRO_TEST(Pam, RefusesAPayloadShorterThanItsHeaderPromised)
{
	const std::vector<std::byte> bytes =
		File(FormatPamHeader({ .Width = 4, .Height = 4, .Depth = 3, .MaxValue = 255 }), { 1, 2, 3 });

	const Result<PamImage> image = PamImage::Decode(bytes);

	GYRO_REQUIRE(!image.has_value());
	GYRO_CHECK(image.error().Code() == ENODATA);
}

// The two numbers in the header are a claim about an allocation, and they are checked as one before
// anything is sized to them.
GYRO_TEST(Pam, RefusesAnExtentPastWhatItWillDecode)
{
	const std::vector<std::byte> bytes =
		File(FormatPamHeader({ .Width = 1'000'000, .Height = 1'000'000, .Depth = 3, .MaxValue = 255 }), {});

	const Result<PamHeaderView> header = ParsePamHeader(bytes);

	GYRO_REQUIRE(!header.has_value());
	GYRO_CHECK(header.error().Code() == E2BIG);
}

// Something that is not a PAM at all, which is a person passing the wrong path on a command line.
GYRO_TEST(Pam, RefusesWhatIsNotAPam)
{
	const std::vector<std::byte> bytes = File(
		"\x89"
		"PNG\r\n",
		{ 0 }
	);

	const Result<PamImage> image = PamImage::Decode(bytes);

	GYRO_REQUIRE(!image.has_value());
	GYRO_CHECK(image.error().Code() == EINVAL);
}

// A depth this file does not decode is refused by code rather than by guess: a grayscale PAM is a real
// file, and inventing a colour for it would be a background nobody authored.
GYRO_TEST(Pam, RefusesADepthItDoesNotDecode)
{
	const std::vector<std::byte> bytes =
		File("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 1\nMAXVAL 255\nTUPLTYPE GRAYSCALE\nENDHDR\n", { 128 });

	const Result<PamImage> image = PamImage::Decode(bytes);

	GYRO_REQUIRE(!image.has_value());
	GYRO_CHECK(image.error().Code() == ENOTSUP);
}
