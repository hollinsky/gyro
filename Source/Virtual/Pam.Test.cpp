#include "Virtual/Pam.h"

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"
#include "Virtual/Pixels.h"

// The dump, checked by reading the file back — which is the one place in the tree that does, and it
// is checking the *writer* rather than comparing against a reference.
//
// Virtual/Pixels.h has the argument for why no test ever compares a frame against a stored image.
// This file is the exception that proves the rule: what it asserts is that a header says what the
// image is and that the samples come out in the order and depth the format promises, so that the
// picture a person opens when something else fails is the picture that was actually drawn.

namespace
{
constexpr PixelFormat Xrgb8{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelFormat Xrgb10{ FormatXrgb2101010, 0, ModifierLinear };

// A directory under whatever the environment says is temporary, unique to the process so that two
// runs in parallel do not read each other's frames.
class Scratch
{
public:
	Scratch() : m_Path{ std::format("/tmp/gyro-pam-{}", static_cast<long>(::getpid())) } {}

	~Scratch()
	{
		for (const std::string& file : m_Files)
		{
			::unlink(file.c_str());
		}

		::rmdir(m_Path.c_str());
	}

	Scratch(const Scratch&) = delete;
	Scratch& operator=(const Scratch&) = delete;

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	// By value, because the vector behind it reallocates: a reference handed out before a second
	// `Track` is a dangling one, and the symptom is a test that reports the file it just wrote as
	// missing.
	[[nodiscard]] std::string Track(std::string file)
	{
		m_Files.push_back(file);

		return file;
	}

private:
	std::string m_Path;
	std::vector<std::string> m_Files;
};

[[nodiscard]] std::vector<std::byte> Read(const std::string& path)
{
	std::vector<std::byte> bytes;

	std::FILE* const file = std::fopen(path.c_str(), "rb");

	if (file == nullptr)
	{
		return bytes;
	}

	std::byte chunk[4096];

	for (std::size_t read = std::fread(chunk, 1, sizeof chunk, file); read > 0;
	     read = std::fread(chunk, 1, sizeof chunk, file))
	{
		bytes.insert(bytes.end(), chunk, chunk + read);
	}

	std::fclose(file);

	return bytes;
}

[[nodiscard]] std::string_view Text(const std::vector<std::byte>& bytes, std::size_t length)
{
	return { reinterpret_cast<const char*>(bytes.data()), std::min(length, bytes.size()) };
}
} // namespace

GYRO_TEST(Pam, AnEightBitFrameIsWrittenAsEightBitSamples)
{
	Scratch scratch;

	constexpr PixelSize<DeviceSpace> Size{ 2, 1 };
	std::vector<std::byte> pixels(static_cast<std::size_t>(Size.Width) * 4 * Size.Height);

	const Result<MutableImageView> canvas = MutableImageView::Over(pixels, Size, Size.Width * 4, Xrgb8);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	canvas->Set(0, 0, Rgb8(255, 0, 0));
	canvas->Set(1, 0, Rgb8(0, 128, 0));

	GYRO_REQUIRE_EQ(DumpFrame(canvas->Read(), scratch.Path(), 7).has_value(), true);

	const std::string path = scratch.Track(std::format("{}/frame-00000007.pam", scratch.Path()));
	const std::vector<std::byte> file = Read(path);

	GYRO_REQUIRE(!file.empty());

	const std::string_view header = "P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
	GYRO_CHECK_EQ(Text(file, header.size()), header);

	// Eight samples: two pixels, four channels, one byte each. RGBA order, which is PAM's and is not
	// the source format's — the conversion out of `XR24`'s BGRX is the thing being checked.
	GYRO_REQUIRE_EQ(file.size(), header.size() + 8);

	const std::byte* samples = file.data() + header.size();
	GYRO_CHECK_EQ(samples[0], std::byte{ 255 });
	GYRO_CHECK_EQ(samples[1], std::byte{ 0 });
	GYRO_CHECK_EQ(samples[2], std::byte{ 0 });
	GYRO_CHECK_EQ(samples[3], std::byte{ 255 });
	GYRO_CHECK_EQ(samples[5], std::byte{ 128 });
}

// Ten-bit content is written wide, and big-endian, which is the netpbm rule a little-endian machine
// gets wrong by doing nothing.
GYRO_TEST(Pam, ATenBitFrameKeepsItsPrecisionAndIsBigEndian)
{
	Scratch scratch;

	constexpr PixelSize<DeviceSpace> Size{ 1, 1 };
	std::vector<std::byte> pixels(4);

	const Result<MutableImageView> canvas = MutableImageView::Over(pixels, Size, 4, Xrgb10);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	// A value with different high and low bytes, so a byte order mistake is visible rather than
	// symmetric.
	const Rgba16 fine{ FromTenBit(513), 0, 0, 65535 };
	canvas->Set(0, 0, fine);

	GYRO_REQUIRE_EQ(DumpFrame(canvas->Read(), scratch.Path(), 0).has_value(), true);

	const std::string path = scratch.Track(std::format("{}/frame-00000000.pam", scratch.Path()));
	const std::vector<std::byte> file = Read(path);
	GYRO_REQUIRE(!file.empty());

	const std::string_view header = "P7\nWIDTH 1\nHEIGHT 1\nDEPTH 4\nMAXVAL 65535\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
	GYRO_CHECK_EQ(Text(file, header.size()), header);
	GYRO_REQUIRE_EQ(file.size(), header.size() + 8);

	const std::byte* samples = file.data() + header.size();
	GYRO_CHECK_EQ(samples[0], static_cast<std::byte>(fine.Red >> 8));
	GYRO_CHECK_EQ(samples[1], static_cast<std::byte>(fine.Red & 0xFF));
}

// No half-written file is ever visible under the final name, because a dump directory is something
// people point a reloading viewer at.
GYRO_TEST(Pam, NoPartialFileIsLeftUnderTheFinalName)
{
	Scratch scratch;

	constexpr PixelSize<DeviceSpace> Size{ 4, 4 };
	std::vector<std::byte> pixels(static_cast<std::size_t>(Size.Width) * 4 * Size.Height);

	const Result<MutableImageView> canvas = MutableImageView::Over(pixels, Size, Size.Width * 4, Xrgb8);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);
	canvas->Fill(canvas->Read().Extent(), Rgb8(1, 2, 3));

	GYRO_REQUIRE_EQ(DumpFrame(canvas->Read(), scratch.Path(), 1).has_value(), true);

	const std::string path = scratch.Track(std::format("{}/frame-00000001.pam", scratch.Path()));
	const std::string partial = scratch.Track(path + ".part");

	GYRO_CHECK_EQ(::access(path.c_str(), F_OK), 0);
	GYRO_CHECK_EQ(::access(partial.c_str(), F_OK), -1);
}

// A directory that cannot be created is reported rather than swallowed, since a dump nobody can find
// is a diagnostic that failed to diagnose.
GYRO_TEST(Pam, AnUnwritableDestinationIsReported)
{
	constexpr PixelSize<DeviceSpace> Size{ 1, 1 };
	std::vector<std::byte> pixels(4);

	const Result<MutableImageView> canvas = MutableImageView::Over(pixels, Size, 4, Xrgb8);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	GYRO_CHECK_EQ(DumpFrame(canvas->Read(), "/proc/gyro-cannot-exist", 0).has_value(), false);
	GYRO_CHECK_EQ(DumpFrame(canvas->Read(), {}, 0).has_value(), false);
	GYRO_CHECK_EQ(WritePam(ImageView{}, "/tmp/gyro-never-written.pam").has_value(), false);
}
