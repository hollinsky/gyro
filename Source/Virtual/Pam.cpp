#include "Virtual/Pam.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "Core/Fd.h"

namespace
{
// `write` is permitted to write less than it was asked for, and the loop that handles it is the one
// everybody leaves out. On a regular file it is nearly unobservable — which is what makes the
// omission survive review and then fail on the day somebody points a dump at a pipe.
[[nodiscard]] Result<void> WriteAll(RawFd file, std::span<const std::byte> bytes)
{
	while (!bytes.empty())
	{
		const ssize_t written = ::write(file.Value, bytes.data(), bytes.size());

		if (written < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			return Failure(errno, "writing a PAM frame");
		}

		bytes = bytes.subspan(static_cast<std::size_t>(written));
	}

	return {};
}

[[nodiscard]] Result<void> WriteAll(RawFd file, std::string_view text)
{
	return WriteAll(file, std::as_bytes(std::span{ text.data(), text.size() }));
}

// One row, encoded into `into` at the file's sample depth. Returns how many bytes were filled.
//
// Sixteen-bit samples are big-endian: the netpbm specification says so, and it is the one detail of
// this format that a little-endian machine gets wrong by doing nothing.
[[nodiscard]] std::size_t EncodeRow(const ImageView& image, std::int32_t y, bool wide, std::span<std::byte> into)
{
	std::size_t at = 0;

	for (std::int32_t x = 0; x < image.Size().Width; ++x)
	{
		const Rgba16 pixel = image.At(x, y);
		const std::uint16_t channels[4]{ pixel.Red, pixel.Green, pixel.Blue, pixel.Alpha };

		for (const std::uint16_t channel : channels)
		{
			if (wide)
			{
				into[at++] = static_cast<std::byte>(channel >> 8);
				into[at++] = static_cast<std::byte>(channel & 0xFFU);
			}
			else
			{
				into[at++] = static_cast<std::byte>(ToEightBit(channel));
			}
		}
	}

	return at;
}
} // namespace

Result<void> WritePam(const ImageView& image, std::string_view path)
{
	if (!image.IsValid())
	{
		return Failure(EINVAL, "nothing to write: the image view is empty");
	}

	// Ten-bit sources get sixteen-bit samples; everything else gets eight. `BitsPerChannel` answers
	// zero for a format `ImageView` would not have accepted, so the fallback is unreachable rather
	// than a policy.
	const bool wide = BitsPerChannel(image.Format().Code) > 8;

	const std::string temporary = std::string{ path } + ".part";

	Fd file{ ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) };

	if (!file.IsValid())
	{
		return Failure(errno, "creating a PAM frame");
	}

	const std::string header = std::format(
		"P7\nWIDTH {}\nHEIGHT {}\nDEPTH 4\nMAXVAL {}\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
		image.Size().Width,
		image.Size().Height,
		wide ? 65535 : 255
	);

	if (const Result<void> written = WriteAll(file.Borrow(), header); !written)
	{
		::unlink(temporary.c_str());

		return written;
	}

	std::vector<std::byte> row(static_cast<std::size_t>(image.Size().Width) * 4U * (wide ? 2U : 1U));

	for (std::int32_t y = 0; y < image.Size().Height; ++y)
	{
		const std::size_t filled = EncodeRow(image, y, wide, row);

		if (const Result<void> written = WriteAll(file.Borrow(), std::span{ row }.first(filled)); !written)
		{
			::unlink(temporary.c_str());

			return written;
		}
	}

	// Closed before the rename, so that the file the reader opens is one the kernel has no more of
	// this process's writes queued against.
	file.Reset();

	if (::rename(temporary.c_str(), std::string{ path }.c_str()) != 0)
	{
		const int code = errno;
		::unlink(temporary.c_str());

		return Failure(code, "publishing a PAM frame");
	}

	return {};
}

Result<void> DumpFrame(const ImageView& image, std::string_view directory, std::uint64_t sequence)
{
	if (directory.empty())
	{
		return Failure(EINVAL, "no dump directory");
	}

	const std::string path{ directory };

	if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
	{
		return Failure(errno, "creating a frame dump directory");
	}

	return WritePam(image, std::format("{}/frame-{:08}.pam", path, sequence));
}

std::string_view FrameDumpDirectory() noexcept
{
	const char* const configured = std::getenv("GYRO_FRAME_DUMP");

	return configured != nullptr ? std::string_view{ configured } : std::string_view{};
}
