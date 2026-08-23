// mmap is POSIX; it is named here for Core/Fd.cpp's reason rather than inherited from a build flag.
#define _POSIX_C_SOURCE 200809L

#include "Nested/Feedback.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

namespace Nested
{
namespace
{
// What one `dev_t` looks like on the wire: an array of exactly `sizeof(dev_t)` bytes, host byte
// order. Read into a `uint64_t` rather than a `dev_t` so that the type is the same width whatever the
// C library calls it, and so a host that sent the wrong number of bytes drops the value instead of
// reading past the array.
[[nodiscard]] std::uint64_t ReadDevice(std::span<const std::byte> bytes) noexcept
{
	std::uint64_t device = 0;

	if (bytes.size() > sizeof device)
	{
		return 0;
	}

	std::memcpy(&device, bytes.data(), bytes.size());

	return device;
}
} // namespace

std::vector<PixelFormat> DmabufSupport::Candidates(std::uint32_t code) const
{
	std::vector<PixelFormat> candidates;

	for (const DmabufTranche& tranche : Tranches)
	{
		for (const PixelFormat& format : tranche.Formats)
		{
			// A pair offered by two tranches is offered once. The first tranche is the better one, so
			// the earlier position is the one to keep.
			if (format.Code == code && std::ranges::find(candidates, format) == candidates.end())
			{
				candidates.push_back(format);
			}
		}
	}

	return candidates;
}

std::uint64_t DmabufSupport::DeviceFor(PixelFormat format) const noexcept
{
	for (const DmabufTranche& tranche : Tranches)
	{
		if (std::ranges::find(tranche.Formats, format) != tranche.Formats.end())
		{
			return tranche.Device;
		}
	}

	return MainDevice;
}

void DmabufFeedback::OnFormatTable(Fd fd, std::uint32_t size)
{
	m_Table.Reset();
	m_Entries = 0;

	if (!fd.IsValid() || size < sizeof(TableEntry))
	{
		return;
	}

	// `MAP_PRIVATE` because the protocol says so and because it is the difference that matters: the
	// table belongs to the host, and a shared mapping would let a defect here corrupt the set every
	// other client is reading.
	void* const mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd.Borrow().Value, 0);

	if (mapped == MAP_FAILED)
	{
		return;
	}

	m_Table = Mapping{ static_cast<std::byte*>(mapped), size };

	// Truncated toward zero, so a table whose length is not a whole number of entries loses the last
	// partial one rather than reading four bytes of it.
	m_Entries = size / sizeof(TableEntry);
}

void DmabufFeedback::OnMainDevice(std::span<const std::byte> device)
{
	m_Pending.MainDevice = ReadDevice(device);
}

void DmabufFeedback::OnTrancheTargetDevice(std::span<const std::byte> device)
{
	m_Tranche.Device = ReadDevice(device);
}

void DmabufFeedback::OnTrancheFlags(Wayland::ZwpLinuxDmabufFeedbackV1TrancheFlags flags)
{
	m_Tranche.Scanout = (static_cast<std::uint32_t>(flags) &
	                     static_cast<std::uint32_t>(Wayland::ZwpLinuxDmabufFeedbackV1TrancheFlags::Scanout)) != 0;
}

void DmabufFeedback::OnTrancheFormats(std::span<const std::byte> indices)
{
	const std::span<const std::byte> table = m_Table.Bytes();

	if (table.empty())
	{
		// Indices into a table that never arrived. Dropped rather than guessed at: the protocol sends
		// the table first, so this is a host out of order rather than a set gyro can reconstruct.
		return;
	}

	// Sixteen-bit indices, packed. A trailing odd byte is a host that miscounted and the entry it
	// would have named does not exist.
	for (std::size_t offset = 0; offset + sizeof(std::uint16_t) <= indices.size(); offset += sizeof(std::uint16_t))
	{
		std::uint16_t index = 0;
		std::memcpy(&index, indices.data() + offset, sizeof index);

		if (index >= m_Entries)
		{
			continue;
		}

		TableEntry entry{};
		std::memcpy(&entry, table.data() + (std::size_t{ index } * sizeof(TableEntry)), sizeof entry);

		m_Tranche.Formats.push_back(PixelFormat{ entry.Format, 0, entry.Modifier });
	}
}

void DmabufFeedback::OnTrancheDone()
{
	// A band with nothing in it says nothing and is dropped, so that `Candidates` never has to skip
	// one and `DeviceFor` never returns the device of a tranche that offered no formats.
	if (!m_Tranche.Formats.empty())
	{
		m_Pending.Tranches.push_back(std::move(m_Tranche));
	}

	m_Tranche = DmabufTranche{};
}

void DmabufFeedback::OnDone()
{
	// Swapped whole. Until this point the set is half-arrived, and a reader that acted on it would be
	// pairing a modifier from one tranche with a device from the next.
	m_Support = std::move(m_Pending);
	m_Pending = DmabufSupport{};
	m_Tranche = DmabufTranche{};
	++m_Generations;
}

std::string Describe(const DmabufSupport& support)
{
	std::string text = std::format("main {:#x}", support.MainDevice);

	for (const DmabufTranche& tranche : support.Tranches)
	{
		text += std::format(
			" | tranche {:#x}{} x{}", tranche.Device, tranche.Scanout ? " scanout" : "", tranche.Formats.size()
		);

		// The first few only. A host offers dozens of pairs and a log line that printed all of them
		// would be the reason nobody reads the log.
		for (std::size_t index = 0; index < std::min<std::size_t>(tranche.Formats.size(), 3); ++index)
		{
			text += std::format(" {}", tranche.Formats[index]);
		}

		if (tranche.Formats.size() > 3)
		{
			text += " ...";
		}
	}

	return text;
}
} // namespace Nested
