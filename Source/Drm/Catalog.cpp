#include "Drm/Catalog.h"

#include <drm/drm_mode.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace Drm
{
namespace
{
// A trivially copyable value read out of the blob at `offset`, or nothing where the blob is shorter
// than the read. Copied rather than pointed at: the blob is whatever the kernel returned and carries
// no alignment promise, so a struct overlaid on it is undefined behaviour that happens to work on
// x86 and traps on the architectures this compositor also wants to run on.
template<typename T>
[[nodiscard]] bool ReadAt(std::span<const std::byte> blob, std::size_t offset, T& into) noexcept
{
	if (offset > blob.size() || blob.size() - offset < sizeof(T))
	{
		return false;
	}

	std::memcpy(&into, blob.data() + offset, sizeof(T));

	return true;
}
} // namespace

Duration PeriodOf(const drmModeModeInfo& mode) noexcept
{
	const std::int64_t total = static_cast<std::int64_t>(mode.htotal) * static_cast<std::int64_t>(mode.vtotal);

	if (mode.clock == 0 || total == 0)
	{
		return Duration::zero();
	}

	// kHz against pixels: nanoseconds are `total / (clock * 1000) * 1e9`, which is this without the
	// intermediate rounding. Rounded to nearest rather than truncated, because the error is otherwise
	// systematically early and FrameClock accumulates it.
	const std::int64_t hertzTimesThousand = static_cast<std::int64_t>(mode.clock);
	const std::int64_t numerator = total * 1'000'000;

	return Duration{ (numerator + hertzTimesThousand / 2) / hertzTimesThousand };
}

const drmModeModeInfo*
ChooseMode(std::span<const drmModeModeInfo> modes, PixelSize<DeviceSpace> wanted, Duration period) noexcept
{
	const drmModeModeInfo* preferred = nullptr;
	const drmModeModeInfo* best = nullptr;
	Duration closest = Duration::max();

	for (const drmModeModeInfo& mode : modes)
	{
		if ((mode.type & DRM_MODE_TYPE_PREFERRED) != 0 && preferred == nullptr)
		{
			preferred = &mode;
		}

		if (wanted.IsEmpty() || static_cast<std::int64_t>(mode.hdisplay) != wanted.Width ||
		    static_cast<std::int64_t>(mode.vdisplay) != wanted.Height)
		{
			continue;
		}

		if (period <= Duration::zero())
		{
			return &mode;
		}

		const Duration error = std::chrono::abs(PeriodOf(mode) - period);

		if (error < closest)
		{
			closest = error;
			best = &mode;
		}
	}

	if (best != nullptr)
	{
		return best;
	}

	// The fallback is the preferred mode, and where a connector flags none it is the first — which is
	// where the kernel puts its own best guess, and a connector with no flagged mode still has to
	// produce a picture rather than stay dark over a missing bit.
	if (preferred != nullptr)
	{
		return preferred;
	}

	return modes.empty() ? nullptr : modes.data();
}

std::vector<PlaneFormat> DecodeFormats(std::span<const std::byte> blob)
{
	std::vector<PlaneFormat> catalog;

	drm_format_modifier_blob header{};

	if (!ReadAt(blob, 0, header) || header.version != FORMAT_BLOB_CURRENT)
	{
		return catalog;
	}

	catalog.reserve(header.count_formats);

	for (std::uint32_t index = 0; index < header.count_formats; ++index)
	{
		std::uint32_t code = 0;

		if (!ReadAt(blob, header.formats_offset + std::size_t{ index } * sizeof(std::uint32_t), code))
		{
			return {};
		}

		catalog.push_back(PlaneFormat{ .Code = code, .Modifiers = {} });
	}

	for (std::uint32_t index = 0; index < header.count_modifiers; ++index)
	{
		drm_format_modifier entry{};

		if (!ReadAt(blob, header.modifiers_offset + std::size_t{ index } * sizeof(entry), entry))
		{
			return {};
		}

		// Bit `bit` of the mask names `formats[entry.offset + bit]`. A mask bit past the end of the
		// format table is a blob describing something that is not there, and is dropped rather than
		// read.
		for (std::uint32_t bit = 0; bit < 64; ++bit)
		{
			if ((entry.formats & (std::uint64_t{ 1 } << bit)) == 0)
			{
				continue;
			}

			const std::uint64_t which = std::uint64_t{ entry.offset } + bit;

			if (which >= catalog.size())
			{
				continue;
			}

			catalog[static_cast<std::size_t>(which)].Modifiers.push_back(entry.modifier);
		}
	}

	return catalog;
}

std::span<const std::uint64_t> ModifiersFor(std::span<const PlaneFormat> catalog, std::uint32_t code) noexcept
{
	for (const PlaneFormat& format : catalog)
	{
		if (format.Code == code)
		{
			return format.Modifiers;
		}
	}

	return {};
}
} // namespace Drm
