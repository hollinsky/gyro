#include "Drm/Catalog.h"

#include <drm/drm_mode.h>

#include <cstring>
#include <vector>

#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The half of the DRM backend a machine with no panel can run, and the two mistakes it exists to
// catch: a period taken from the rounded `vrefresh` field, and a modifier table read one entry out.

namespace
{
[[nodiscard]] drmModeModeInfo Mode(
	std::uint16_t width,
	std::uint16_t height,
	std::uint32_t clock,
	std::uint16_t htotal,
	std::uint16_t vtotal,
	std::uint32_t type = 0
)
{
	drmModeModeInfo mode{};
	mode.clock = clock;
	mode.hdisplay = width;
	mode.htotal = htotal;
	mode.vdisplay = height;
	mode.vtotal = vtotal;
	mode.type = type;

	return mode;
}

// A blob laid out the way the kernel lays one out: header, then formats, then modifiers.
[[nodiscard]] std::vector<std::byte>
BuildBlob(std::span<const std::uint32_t> formats, std::span<const drm_format_modifier> modifiers)
{
	drm_format_modifier_blob header{};
	header.version = FORMAT_BLOB_CURRENT;
	header.count_formats = static_cast<std::uint32_t>(formats.size());
	header.formats_offset = sizeof(header);
	header.count_modifiers = static_cast<std::uint32_t>(modifiers.size());
	header.modifiers_offset = header.formats_offset + static_cast<std::uint32_t>(formats.size_bytes());

	std::vector<std::byte> blob(header.modifiers_offset + modifiers.size_bytes());

	std::memcpy(blob.data(), &header, sizeof(header));
	std::memcpy(blob.data() + header.formats_offset, formats.data(), formats.size_bytes());
	std::memcpy(blob.data() + header.modifiers_offset, modifiers.data(), modifiers.size_bytes());

	return blob;
}
} // namespace

// 148.5 MHz over 2200x1125 is 60 Hz exactly; the same timings at 148.352 MHz are the 59.94 the panel
// actually runs at. `vrefresh` reports 60 for both, which is sixty-seven microseconds of error per
// frame — a whole vblank's worth of drift every fifteen seconds.
GYRO_TEST(DrmCatalog, PeriodComesFromTheTimings)
{
	GYRO_CHECK(Drm::PeriodOf(Mode(1920, 1080, 148'500, 2200, 1125)) == std::chrono::nanoseconds{ 16'666'667 });

	const Duration slow = Drm::PeriodOf(Mode(1920, 1080, 148'352, 2200, 1125));

	GYRO_CHECK(slow > std::chrono::nanoseconds{ 16'683'000 } && slow < std::chrono::nanoseconds{ 16'684'000 });
	GYRO_CHECK(Drm::PeriodOf(Mode(1920, 1080, 0, 2200, 1125)) == Duration::zero());
}

// The two panels the figure was measured on, so the arithmetic that carries it is checked against the
// modes it was checked against. The pair on one connector is the point: same totals, different pixel
// clock, so the blanking interval moves while every line count stays put — which is what separated a
// duration from a count of lines and killed the back-porch reading.
GYRO_TEST(DrmCatalog, TheBlankingIntervalIsTheDistanceToTheLatchDeadline)
{
	// eDP-1, 1920x1080 at 60.05 and at 40.03: 36 blanked lines either way.
	GYRO_CHECK(Drm::BlankingOf(Mode(1920, 1080, 141'000, 2104, 1116)) == std::chrono::nanoseconds{ 537'191 });
	GYRO_CHECK(Drm::BlankingOf(Mode(1920, 1080, 94'000, 2104, 1116)) == std::chrono::nanoseconds{ 805'787 });

	// DP-7, 2560x1080 at 60 against 1600x1200 at 60: twenty blanked lines against fifty, and the
	// interval is a duration rather than either count.
	GYRO_CHECK(Drm::BlankingOf(Mode(2560, 1080, 198'000, 3000, 1100)) == std::chrono::nanoseconds{ 303'030 });
	GYRO_CHECK(Drm::BlankingOf(Mode(1600, 1200, 162'000, 2160, 1250)) == std::chrono::nanoseconds{ 666'667 });

	// A table gyro did not author. Zero rather than the several hours an unsigned subtraction produces,
	// because this number is subtracted from every deadline the output ever predicts.
	GYRO_CHECK(Drm::BlankingOf(Mode(1920, 1080, 148'500, 2200, 1080)) == Duration::zero());
	GYRO_CHECK(Drm::BlankingOf(Mode(1920, 1080, 148'500, 2200, 1000)) == Duration::zero());
	GYRO_CHECK(Drm::BlankingOf(Mode(1920, 1080, 0, 2200, 1125)) == Duration::zero());
}

GYRO_TEST(DrmCatalog, ChoosesTheModeAtTheWantedRate)
{
	const std::vector<drmModeModeInfo> modes{
		Mode(1920, 1080, 148'500, 2200, 1125, DRM_MODE_TYPE_PREFERRED),
		Mode(1920, 1080, 297'000, 2200, 1125),
		Mode(1280, 720, 74'250, 1650, 750),
	};

	const drmModeModeInfo* const fast = Drm::ChooseMode(modes, { 1920, 1080 }, PeriodFromHertz(120.0));

	GYRO_REQUIRE(fast != nullptr);
	GYRO_CHECK(fast->clock == 297'000);

	// A resolution the connector does not offer falls back to the preferred mode rather than to
	// something the wrong size: a picture at a size nobody asked for is worse than one at a rate two
	// hundredths off.
	const drmModeModeInfo* const fallback = Drm::ChooseMode(modes, { 3840, 2160 }, PeriodFromHertz(60.0));

	GYRO_REQUIRE(fallback != nullptr);
	GYRO_CHECK(fallback->hdisplay == 1920 && fallback->clock == 148'500);

	GYRO_CHECK(Drm::ChooseMode({}, { 1920, 1080 }, PeriodFromHertz(60.0)) == nullptr);
}

GYRO_TEST(DrmCatalog, DecodesFormatsAgainstTheModifierMask)
{
	const std::vector<std::uint32_t> formats{ FormatXrgb8888, FormatArgb8888, FormatNv12 };

	// Bit i of `formats` names `formats[offset + i]`, and the offset is what makes this worth a test:
	// the second entry names the third format alone.
	const std::vector<drm_format_modifier> modifiers{
		drm_format_modifier{ .formats = 0b011, .offset = 0, .pad = 0, .modifier = ModifierLinear },
		drm_format_modifier{ .formats = 0b001, .offset = 2, .pad = 0, .modifier = 0x100000000000001ULL },
		drm_format_modifier{ .formats = 0b001, .offset = 0, .pad = 0, .modifier = 0x100000000000002ULL },
	};

	const std::vector<std::byte> blob = BuildBlob(formats, modifiers);
	const std::vector<Drm::PlaneFormat> catalog = Drm::DecodeFormats(blob);

	GYRO_REQUIRE(catalog.size() == 3);

	const std::span<const std::uint64_t> xrgb = Drm::ModifiersFor(catalog, FormatXrgb8888);

	GYRO_REQUIRE(xrgb.size() == 2);
	GYRO_CHECK(xrgb[0] == ModifierLinear);
	GYRO_CHECK(xrgb[1] == 0x100000000000002ULL);

	const std::span<const std::uint64_t> argb = Drm::ModifiersFor(catalog, FormatArgb8888);

	GYRO_REQUIRE(argb.size() == 1);
	GYRO_CHECK(argb[0] == ModifierLinear);

	const std::span<const std::uint64_t> nv12 = Drm::ModifiersFor(catalog, FormatNv12);

	GYRO_REQUIRE(nv12.size() == 1);
	GYRO_CHECK(nv12[0] == 0x100000000000001ULL);

	GYRO_CHECK(Drm::ModifiersFor(catalog, FormatXrgb2101010).empty());
}

// A blob that does not describe itself yields nothing rather than a read past the end. It arrives
// from the kernel, but the *length* arrives from a separate ioctl and gyro is the party that has to
// keep the two consistent.
GYRO_TEST(DrmCatalog, RefusesABlobThatDoesNotFit)
{
	const std::vector<std::uint32_t> formats{ FormatXrgb8888 };
	const std::vector<drm_format_modifier> modifiers{
		drm_format_modifier{ .formats = 0b001, .offset = 0, .pad = 0, .modifier = ModifierLinear },
	};

	std::vector<std::byte> blob = BuildBlob(formats, modifiers);
	blob.resize(blob.size() - 1);

	GYRO_CHECK(Drm::DecodeFormats(blob).empty());
	GYRO_CHECK(Drm::DecodeFormats({}).empty());

	// A mask bit naming a format past the end of the table is dropped rather than read.
	const std::vector<drm_format_modifier> wild{
		drm_format_modifier{ .formats = 0b110, .offset = 0, .pad = 0, .modifier = ModifierLinear },
	};

	const std::vector<Drm::PlaneFormat> catalog = Drm::DecodeFormats(BuildBlob(formats, wild));

	GYRO_REQUIRE(catalog.size() == 1);
	GYRO_CHECK(catalog[0].Modifiers.empty());
}

// **The pre-filter in front of the atomic test, and the two silences it must not read as refusals.**
// A plane that reports no `IN_FORMATS` at all is a driver that did not answer, and a framebuffer with
// no modifier is one the driver laid out itself — refusing either would disable promotion on hardware
// that works, which is a whole-screen composite every frame in exchange for a question this file was
// never able to answer.
GYRO_TEST(DrmCatalog, AdvertisedFiltersALayoutAPlaneNeverNamed)
{
	const std::vector<std::uint32_t> formats{ FormatXrgb8888, FormatArgb8888 };
	const std::vector<drm_format_modifier> modifiers{
		drm_format_modifier{ .formats = 0b011, .offset = 0, .pad = 0, .modifier = ModifierLinear },
		drm_format_modifier{ .formats = 0b001, .offset = 0, .pad = 0, .modifier = 0x100000000000002ULL },
	};

	const std::vector<Drm::PlaneFormat> catalog = Drm::DecodeFormats(BuildBlob(formats, modifiers));

	GYRO_CHECK(Drm::Advertised(catalog, PixelFormat{ .Code = FormatXrgb8888, .Modifier = ModifierLinear }));
	GYRO_CHECK(Drm::Advertised(catalog, PixelFormat{ .Code = FormatXrgb8888, .Modifier = 0x100000000000002ULL }));

	// The format is in the table and that modifier is not, which is the tiled buffer a cursor plane
	// will not read — refused here rather than by an ioctl on the frame thread.
	GYRO_CHECK(!Drm::Advertised(catalog, PixelFormat{ .Code = FormatArgb8888, .Modifier = 0x100000000000002ULL }));

	// The format is not in the table at all, which is the whole of what a cursor plane's one-entry
	// catalog says about every window buffer on the machine.
	GYRO_CHECK(!Drm::Advertised(catalog, PixelFormat{ .Code = FormatNv12, .Modifier = ModifierLinear }));

	// Neither silence is a refusal: an empty catalog, and a layout the framebuffer never stated.
	GYRO_CHECK(Drm::Advertised({}, PixelFormat{ .Code = FormatNv12, .Modifier = ModifierLinear }));
	GYRO_CHECK(Drm::Advertised(catalog, PixelFormat{ .Code = FormatArgb8888, .Modifier = ModifierInvalid }));

	// And a layer with no format at all is `Framebuffer`'s refusal to make, not this one's.
	GYRO_CHECK(Drm::Advertised(catalog, PixelFormat{}));
}
