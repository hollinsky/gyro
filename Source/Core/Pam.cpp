#include "Core/Pam.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <format>
#include <string_view>

namespace
{
// The header is ASCII, and reading it as text is what keeps this file short. A view over the caller's
// bytes rather than a copy: a header is a few dozen bytes at the front of something that may be
// thirty megabytes.
[[nodiscard]] std::string_view TextOver(std::span<const std::byte> bytes) noexcept
{
	return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

// One whitespace-delimited token, and what is left after it.
[[nodiscard]] std::string_view Token(std::string_view& line) noexcept
{
	const std::size_t start = line.find_first_not_of(" \t\r");

	if (start == std::string_view::npos)
	{
		line = {};

		return {};
	}

	const std::size_t end = line.find_first_of(" \t\r", start);
	const std::string_view token = line.substr(start, end == std::string_view::npos ? end : end - start);

	line = end == std::string_view::npos ? std::string_view{} : line.substr(end);

	return token;
}

// A field's value as a number. Refuses trailing rubbish rather than taking the prefix, because
// `WIDTH 1920x1080` is a file somebody generated wrongly and `1920` is the reading most likely to
// produce a picture that is subtly not what they made.
[[nodiscard]] bool Number(std::string_view text, std::uint32_t& into) noexcept
{
	if (text.empty())
	{
		return false;
	}

	const char* const end = text.data() + text.size();
	const std::from_chars_result read = std::from_chars(text.data(), end, into);

	return read.ec == std::errc{} && read.ptr == end;
}

// One sample, widened or narrowed to the eight bits a texel word carries.
//
// Rounded rather than truncated, and scaled by the file's own maximum rather than by a shift: a PAM
// is permitted any maximum from 1 to 65535, and a bilevel-ish `MAXVAL 1` read with a shift would be a
// black image.
[[nodiscard]] constexpr std::uint32_t ToEight(std::uint32_t sample, std::uint32_t maximum) noexcept
{
	if (sample >= maximum)
	{
		return 255;
	}

	return (sample * 255U + maximum / 2U) / maximum;
}

// Straight alpha into premultiplied, at eight bits, rounded.
[[nodiscard]] constexpr std::uint32_t Premultiply(std::uint32_t channel, std::uint32_t alpha) noexcept
{
	return (channel * alpha + 127U) / 255U;
}
} // namespace

std::string FormatPamHeader(const PamHeader& header, std::string_view note)
{
	// The fields go first, because `P7` has to be the first token of the file and everything between it
	// and `ENDHDR` is the header the note is part of.
	std::string text = std::format(
		"P7\nWIDTH {}\nHEIGHT {}\nDEPTH {}\nMAXVAL {}\nTUPLTYPE {}\n",
		header.Width,
		header.Height,
		header.Depth,
		header.MaxValue,
		header.HasAlpha() ? "RGB_ALPHA" : "RGB"
	);

	while (!note.empty())
	{
		const std::size_t end = note.find('\n');

		text += "# ";
		text += note.substr(0, end);
		text += '\n';

		note = end == std::string_view::npos ? std::string_view{} : note.substr(end + 1);
	}

	text += "ENDHDR\n";

	return text;
}

Result<PamHeaderView> ParsePamHeader(std::span<const std::byte> bytes)
{
	std::string_view text = TextOver(bytes);

	if (!text.starts_with("P7"))
	{
		return Failure(EINVAL, "not a PAM: the magic is not P7");
	}

	// Everything after the magic is lines until `ENDHDR`. The magic's own line may carry nothing else,
	// so it is consumed as a line like the rest rather than as two characters.
	std::size_t at = 0;
	bool ended = false;

	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t depth = 0;
	std::uint32_t maximum = 0;

	while (at < text.size())
	{
		const std::size_t newline = text.find('\n', at);

		if (newline == std::string_view::npos)
		{
			break;
		}

		std::string_view line = text.substr(at, newline - at);

		at = newline + 1;

		const std::string_view key = Token(line);

		// A comment runs to the end of its line, and a blank line is nothing. Both are legal and both
		// are what a hand-edited header collects.
		if (key.empty() || key.starts_with('#') || key == "P7")
		{
			continue;
		}

		if (key == "ENDHDR")
		{
			ended = true;

			break;
		}

		// `TUPLTYPE` is read and dropped: `DEPTH` is what the payload's layout follows, and this file
		// declines to be the reader that walks off a row because a string disagreed with a number.
		if (key == "TUPLTYPE")
		{
			continue;
		}

		std::uint32_t* field = nullptr;

		if (key == "WIDTH")
		{
			field = &width;
		}
		else if (key == "HEIGHT")
		{
			field = &height;
		}
		else if (key == "DEPTH")
		{
			field = &depth;
		}
		else if (key == "MAXVAL")
		{
			field = &maximum;
		}

		// An unknown key is a header from a newer netpbm than this reader, and skipping it is what the
		// format asks for. A known key with a value that is not a number is not.
		if (field == nullptr)
		{
			continue;
		}

		if (!Number(Token(line), *field))
		{
			return Failure(EINVAL, "a PAM header field is not a number", key);
		}
	}

	if (!ended)
	{
		return Failure(EINVAL, "PAM header has no ENDHDR");
	}

	if (width == 0 || height == 0)
	{
		return Failure(EINVAL, "PAM header has no positive extent");
	}

	if (depth != 3 && depth != 4)
	{
		return Failure(
			ENOTSUP, "a PAM depth that is neither RGB nor RGB_ALPHA", Subject{ std::format("DEPTH {}", depth) }
		);
	}

	if (maximum == 0 || maximum > 65535)
	{
		return Failure(ENOTSUP, "a PAM maximum value outside 1 to 65535", Subject{ std::format("MAXVAL {}", maximum) });
	}

	// Checked here rather than at the allocation, because here is where both numbers are still a
	// *claim* somebody else's file made rather than a length something has already been sized to.
	if (static_cast<std::size_t>(width) * height > MaxPamTexels)
	{
		return Failure(
			E2BIG, "a PAM past what this reader will decode", Subject{ std::format("{}x{}", width, height) }
		);
	}

	return PamHeaderView{ .Header = { .Width = static_cast<std::int32_t>(width),
		                              .Height = static_cast<std::int32_t>(height),
		                              .Depth = static_cast<std::int32_t>(depth),
		                              .MaxValue = maximum },
		                  .Length = at };
}

Result<PamImage> PamImage::Decode(std::span<const std::byte> bytes)
{
	const Result<PamHeaderView> header = ParsePamHeader(bytes);

	if (!header)
	{
		return std::unexpected{ header.error() };
	}

	const PamHeader& shape = header->Header;

	const std::size_t texels = static_cast<std::size_t>(shape.Width) * static_cast<std::size_t>(shape.Height);
	const std::size_t samples = texels * static_cast<std::size_t>(shape.Depth);
	const std::size_t wanted = samples * (shape.IsWide() ? 2U : 1U);

	const std::span<const std::byte> payload = bytes.subspan(header->Length);

	if (payload.size() < wanted)
	{
		return Failure(
			ENODATA,
			"a PAM payload shorter than its header promised",
			Subject{ std::format("{} of {} bytes", payload.size(), wanted) }
		);
	}

	std::vector<std::uint32_t> words(texels);

	const std::uint32_t maximum = shape.MaxValue;
	const bool wide = shape.IsWide();
	const bool alpha = shape.HasAlpha();

	std::size_t at = 0;

	for (std::size_t texel = 0; texel < texels; ++texel)
	{
		std::uint32_t channels[4]{ 0, 0, 0, maximum };

		for (std::int32_t index = 0; index < shape.Depth; ++index)
		{
			if (wide)
			{
				// Big-endian per the specification, which is the one detail of this format a
				// little-endian machine gets wrong by doing nothing — `Virtual/Pam.cpp` says the same
				// thing on the way out.
				channels[index] =
					(static_cast<std::uint32_t>(payload[at]) << 8) | static_cast<std::uint32_t>(payload[at + 1]);
				at += 2;
			}
			else
			{
				channels[index] = static_cast<std::uint32_t>(payload[at]);
				at += 1;
			}
		}

		const std::uint32_t coverage = alpha ? ToEight(channels[3], maximum) : 255U;
		const std::uint32_t red = ToEight(channels[0], maximum);
		const std::uint32_t green = ToEight(channels[1], maximum);
		const std::uint32_t blue = ToEight(channels[2], maximum);

		words[texel] = (coverage << 24) | (Premultiply(red, coverage) << 16) | (Premultiply(green, coverage) << 8) |
		               Premultiply(blue, coverage);
	}

	return PamImage{ shape.Width, shape.Height, alpha, std::move(words) };
}

Result<PamImage> PamImage::Read(RawFd file)
{
	if (!file.IsValid())
	{
		return Failure(EBADF, "reading a PAM from no descriptor");
	}

	std::vector<std::byte> bytes;

	// Grown rather than sized from `fstat`, because a descriptor handed over by a shell may be a pipe
	// or a memfd and a length that was right at the stat is not a length that is right at the read.
	// `MaxPamBytes` is what bounds it instead.
	for (;;)
	{
		const std::size_t filled = bytes.size();

		if (filled >= MaxPamBytes)
		{
			return Failure(EFBIG, "a PAM larger than this reader will take");
		}

		bytes.resize(std::min(filled + (std::size_t{ 1 } << 20), MaxPamBytes + 1));

		const ssize_t read = ::read(file.Value, bytes.data() + filled, bytes.size() - filled);

		if (read < 0)
		{
			if (errno == EINTR)
			{
				bytes.resize(filled);

				continue;
			}

			return Failure(errno, "reading a PAM");
		}

		bytes.resize(filled + static_cast<std::size_t>(read));

		if (read == 0)
		{
			break;
		}
	}

	return Decode(bytes);
}

std::uint32_t PamImage::At(std::int32_t x, std::int32_t y) const noexcept
{
	if (x < 0 || y < 0 || x >= m_Width || y >= m_Height)
	{
		return 0;
	}

	return m_Words[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Width) + static_cast<std::size_t>(x)];
}
