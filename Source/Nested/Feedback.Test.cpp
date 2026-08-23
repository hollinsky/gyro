#include "Nested/Feedback.h"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"
#include "Wayland/LinuxDmabufV1.h"

// The listener driven directly, with no socket in front of it.
//
// **Direct rather than through the peer, because what is being checked is the *reading* of a set the
// host sent** — an index past the end of the table, a tranche that arrives half-way, a pair offered
// twice. Nested/Peer.h can only send sets it can construct, and a well-formed host is exactly the
// case that does not need testing. The end-to-end path is Output.Test.cpp's.

namespace
{
struct TableEntry
{
	std::uint32_t Format = 0;
	std::uint32_t Padding = 0;
	std::uint64_t Modifier = 0;
};

// A format table as a real file, because that is what `format_table` carries and because mapping it
// is the part with a length in it.
[[nodiscard]] Fd Table(std::span<const TableEntry> entries)
{
	const int backing = ::memfd_create("gyro-feedback-test", MFD_CLOEXEC);

	if (backing < 0)
	{
		return Fd{};
	}

	Fd file{ backing };
	const std::size_t bytes = entries.size() * sizeof(TableEntry);

	if (::ftruncate(file.Get(), static_cast<::off_t>(bytes)) != 0)
	{
		return Fd{};
	}

	if (::write(file.Get(), entries.data(), bytes) != static_cast<::ssize_t>(bytes))
	{
		return Fd{};
	}

	return file;
}

[[nodiscard]] std::vector<std::byte> Indices(std::span<const std::uint16_t> values)
{
	std::vector<std::byte> bytes(values.size() * sizeof(std::uint16_t));
	std::memcpy(bytes.data(), values.data(), bytes.size());

	return bytes;
}

[[nodiscard]] std::vector<std::byte> Device(std::uint64_t value)
{
	std::vector<std::byte> bytes(sizeof(::dev_t));
	std::memcpy(bytes.data(), &value, bytes.size());

	return bytes;
}

constexpr TableEntry Entries[] = {
	{ FormatXrgb8888, 0, 0x0300000000000001ULL },
	{ FormatXrgb8888, 0, ModifierLinear },
	{ FormatArgb8888, 0, ModifierLinear },
};
} // namespace

GYRO_TEST(Feedback, ATrancheIsReadInTheHostsOwnOrder)
{
	Nested::DmabufFeedback feedback;

	feedback.OnFormatTable(Table(Entries), sizeof Entries);
	feedback.OnMainDevice(Device(0xe280));
	feedback.OnTrancheTargetDevice(Device(0xe281));

	const std::vector<std::byte> indices = Indices(std::array<std::uint16_t, 3>{ 0, 1, 2 });
	feedback.OnTrancheFormats(indices);
	feedback.OnTrancheFlags(Wayland::ZwpLinuxDmabufFeedbackV1TrancheFlags::Scanout);
	feedback.OnTrancheDone();
	feedback.OnDone();

	GYRO_REQUIRE_EQ(feedback.Generations(), std::uint64_t{ 1 });
	GYRO_REQUIRE_EQ(feedback.Support().Tranches.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(feedback.Support().MainDevice, std::uint64_t{ 0xe280 });
	GYRO_CHECK(feedback.Support().Tranches[0].Scanout);

	// **The order is the host's and is not re-ranked**, which is the whole of decision 120's
	// negotiation: the host has already sorted these against its own hardware, and a client that
	// preferred linear because linear is simpler would be second-guessing a decision it was not part
	// of.
	const std::vector<PixelFormat> candidates = feedback.Support().Candidates(FormatXrgb8888);

	GYRO_REQUIRE_EQ(candidates.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(candidates[0].Modifier, std::uint64_t{ 0x0300000000000001ULL });
	GYRO_CHECK_EQ(candidates[1].Modifier, ModifierLinear);

	// A fourcc nobody offered is not a candidate rather than a fallback.
	GYRO_CHECK(feedback.Support().Candidates(FormatNv12).empty());

	// The device follows the pair, so a syncobj node and a mismatch warning both name the tranche the
	// buffers actually came from rather than the main device.
	GYRO_CHECK_EQ(feedback.Support().DeviceFor(candidates[1]), std::uint64_t{ 0xe281 });
}

GYRO_TEST(Feedback, AnIndexPastTheTableIsDroppedRatherThanRead)
{
	// The table is a shared mapping another process wrote, and the length is what the event said. An
	// index past it is a host that miscounted, and reading whatever follows would be a modifier
	// assembled out of unrelated memory — which a device would then be asked to allocate under.
	Nested::DmabufFeedback feedback;

	feedback.OnFormatTable(Table(Entries), sizeof Entries);
	feedback.OnMainDevice(Device(0xe280));

	const std::vector<std::byte> indices = Indices(std::array<std::uint16_t, 4>{ 1, 9, 400, 0 });
	feedback.OnTrancheFormats(indices);
	feedback.OnTrancheDone();
	feedback.OnDone();

	const std::vector<PixelFormat> candidates = feedback.Support().Candidates(FormatXrgb8888);

	GYRO_REQUIRE_EQ(candidates.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(candidates[0].Modifier, ModifierLinear);
	GYRO_CHECK_EQ(candidates[1].Modifier, std::uint64_t{ 0x0300000000000001ULL });
}

GYRO_TEST(Feedback, IndicesWithNoTableAreDroppedWholesale)
{
	// The protocol sends the table before the indices into it. A host that got that backwards is one
	// whose set cannot be reconstructed, so nothing is guessed at.
	Nested::DmabufFeedback feedback;

	const std::vector<std::byte> indices = Indices(std::array<std::uint16_t, 2>{ 0, 1 });
	feedback.OnTrancheFormats(indices);
	feedback.OnTrancheDone();
	feedback.OnDone();

	GYRO_CHECK(feedback.Support().IsEmpty());
}

GYRO_TEST(Feedback, ASetIsInvisibleUntilDone)
{
	// The protocol says the feedback is coherent only between `done` events, and this is what that
	// buys: a reader acting on a half-arrived set would pair a modifier from one tranche with a device
	// from the next, and allocate against a constraint that was never stated together.
	Nested::DmabufFeedback feedback;

	feedback.OnFormatTable(Table(Entries), sizeof Entries);
	feedback.OnMainDevice(Device(0xe280));

	const std::vector<std::byte> indices = Indices(std::array<std::uint16_t, 1>{ 1 });
	feedback.OnTrancheFormats(indices);
	feedback.OnTrancheDone();

	GYRO_CHECK(feedback.Support().IsEmpty());
	GYRO_CHECK_EQ(feedback.Generations(), std::uint64_t{ 0 });

	feedback.OnDone();

	GYRO_CHECK(!feedback.Support().IsEmpty());
	GYRO_CHECK_EQ(feedback.Generations(), std::uint64_t{ 1 });
}

GYRO_TEST(Feedback, ASecondSetReplacesTheFirstRatherThanAddingToIt)
{
	// A host re-sends its feedback when its own hardware changes — a window moved to the other GPU's
	// monitor is the ordinary cause. Accumulating would leave the old device's modifiers on offer, and
	// the first one gyro tried would be the one that just stopped working.
	Nested::DmabufFeedback feedback;

	feedback.OnFormatTable(Table(Entries), sizeof Entries);
	feedback.OnTrancheTargetDevice(Device(0xe280));
	feedback.OnTrancheFormats(Indices(std::array<std::uint16_t, 1>{ 0 }));
	feedback.OnTrancheDone();
	feedback.OnDone();

	feedback.OnTrancheTargetDevice(Device(0xe999));
	feedback.OnTrancheFormats(Indices(std::array<std::uint16_t, 1>{ 1 }));
	feedback.OnTrancheDone();
	feedback.OnDone();

	GYRO_REQUIRE_EQ(feedback.Support().Tranches.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(feedback.Support().Tranches[0].Device, std::uint64_t{ 0xe999 });

	const std::vector<PixelFormat> candidates = feedback.Support().Candidates(FormatXrgb8888);

	GYRO_REQUIRE_EQ(candidates.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(candidates[0].Modifier, ModifierLinear);
}

GYRO_TEST(Feedback, APairOfferedTwiceIsOfferedOnce)
{
	// Two tranches routinely repeat a pair — a scanout band and a sampling band over the same device.
	// A duplicate costs the caller a second failed allocation attempt against a modifier the device
	// has already refused, which is nothing much, and the deduplication keeps the *order* meaningful:
	// the earlier position is the better band.
	Nested::DmabufFeedback feedback;

	feedback.OnFormatTable(Table(Entries), sizeof Entries);

	feedback.OnTrancheTargetDevice(Device(0xe280));
	feedback.OnTrancheFormats(Indices(std::array<std::uint16_t, 1>{ 0 }));
	feedback.OnTrancheDone();

	feedback.OnTrancheTargetDevice(Device(0xe281));
	feedback.OnTrancheFormats(Indices(std::array<std::uint16_t, 2>{ 0, 1 }));
	feedback.OnTrancheDone();

	feedback.OnDone();

	const std::vector<PixelFormat> candidates = feedback.Support().Candidates(FormatXrgb8888);

	GYRO_REQUIRE_EQ(candidates.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(candidates[0].Modifier, std::uint64_t{ 0x0300000000000001ULL });

	// And the device is the *first* tranche that offered it, which is the one whose ranking put it
	// there.
	GYRO_CHECK_EQ(feedback.Support().DeviceFor(candidates[0]), std::uint64_t{ 0xe280 });
	GYRO_CHECK_EQ(feedback.Support().DeviceFor(candidates[1]), std::uint64_t{ 0xe281 });
}

GYRO_TEST(Feedback, AnEmptyTrancheIsNotABand)
{
	// A tranche whose formats never arrived would otherwise become a band with a device and nothing in
	// it, which `DeviceFor` would then be able to return for a format that band never offered.
	Nested::DmabufFeedback feedback;

	feedback.OnFormatTable(Table(Entries), sizeof Entries);
	feedback.OnTrancheTargetDevice(Device(0xdead));
	feedback.OnTrancheDone();

	feedback.OnTrancheTargetDevice(Device(0xe280));
	feedback.OnTrancheFormats(Indices(std::array<std::uint16_t, 1>{ 1 }));
	feedback.OnTrancheDone();
	feedback.OnDone();

	GYRO_REQUIRE_EQ(feedback.Support().Tranches.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(feedback.Support().Tranches[0].Device, std::uint64_t{ 0xe280 });
}
