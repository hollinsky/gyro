#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Core/Fd.h"
#include "Seam/Buffer.h"
#include "Seam/RenderTarget.h"
#include "Wayland/LinuxDmabufV1.h"

// What the host will accept, read off `zwp_linux_dmabuf_v1`'s feedback.
//
// **This is the constraint decision 102 said a nested output could not infer, arriving stated.** That
// entry's argument against allocating from the rendering device was about scanout: the chip that
// draws is not always the chip that scans out, and the modifier set is an intersection with a display
// plane's. A nested output never scans out — the consumer is a compositor importing a buffer, which
// is what every Wayland client already does — and the host *says* what it will take. So the modifier
// is negotiated rather than assumed, which is decision 120's whole premise.
//
// **A tranche is a preference band and the order is the host's.** The host emits them best first,
// each with a target device and a set of format-and-modifier pairs indexed into a shared table it
// passed as a descriptor. gyro walks that order offering candidates to Seam/Allocator.h one at a time
// — the host ranks, the device vetoes — and the first pair both agree on is what the targets are
// allocated as. Re-ranking here would be gyro second-guessing a negotiation it was not part of.
//
// **The format table is a file the host wrote and gyro must not trust.** It is mapped read-only and
// privately, its length is what the event said rather than what the file claims, and an index past
// the end of it drops that entry rather than reading whatever follows. A host is not an adversary,
// but this is a shared mapping from another process and the difference costs one comparison.
//
// **Everything accumulates into a pending set and is swapped in at `done`.** The protocol says the
// feedback is only coherent between `done` events, so a reader that acted on a half-arrived set would
// be choosing a modifier out of one tranche and a device out of the next.

namespace Nested
{
// One preference band the host offered.
struct DmabufTranche
{
	// `dev_t`, as the host sent it. Zero where it said nothing, which is what Nested/Sync.h reads as
	// *any node will do*.
	std::uint64_t Device = 0;

	// The band is for direct scanout. Not acted on today — a nested output is composited by the host
	// like any other surface, and gyro has no second buffer to promote — and carried because it is
	// what says a band is narrower than it looks.
	bool Scanout = false;

	// Format and modifier pairs, in the host's order.
	std::vector<PixelFormat> Formats;
};

// The whole of one `done`.
struct DmabufSupport
{
	std::uint64_t MainDevice = 0;
	std::vector<DmabufTranche> Tranches;

	[[nodiscard]] bool IsEmpty() const noexcept { return Tranches.empty(); }

	// Every candidate the host offered for this fourcc, best first and across every tranche.
	//
	// Flattened rather than walked per tranche because the caller's question is *which pair does the
	// device also accept*, and a tranche boundary does not change that answer — it only orders it. The
	// target device follows the winner, which is `DeviceFor` below.
	[[nodiscard]] std::vector<PixelFormat> Candidates(std::uint32_t code) const;

	// The device of the tranche a chosen format came from, for the syncobj node and for the line that
	// says whether it is the one gyro is drawing on.
	[[nodiscard]] std::uint64_t DeviceFor(PixelFormat format) const noexcept;
};

// The listener, accumulating until `done`.
//
// It is `Ignoring`-derived for nothing: every event here is read, because every one of them either
// contributes to the set or ends a tranche. A protocol that grew a seventh would be a compiler error
// rather than an event that goes nowhere, which is what the generated listener is for.
class DmabufFeedback final : public Wayland::ZwpLinuxDmabufFeedbackV1Listener
{
public:
	// What the host last said, whole. Empty until the first `done`.
	[[nodiscard]] const DmabufSupport& Support() const noexcept { return m_Support; }

	// How many complete sets have arrived. A host may re-send feedback when its own hardware changes;
	// this is what a caller waiting for the first one polls, rather than a flag it has to clear.
	[[nodiscard]] std::uint64_t Generations() const noexcept { return m_Generations; }

	void OnDone() override;

	void OnFormatTable(Fd fd, std::uint32_t size) override;

	void OnMainDevice(std::span<const std::byte> device) override;

	void OnTrancheDone() override;

	void OnTrancheTargetDevice(std::span<const std::byte> device) override;

	void OnTrancheFormats(std::span<const std::byte> indices) override;

	void OnTrancheFlags(Wayland::ZwpLinuxDmabufFeedbackV1TrancheFlags flags) override;

private:
	// One entry of the shared table: a fourcc, four bytes of padding, and a modifier.
	struct TableEntry
	{
		std::uint32_t Format = 0;
		std::uint32_t Padding = 0;
		std::uint64_t Modifier = 0;
	};

	// The host's table, mapped. Held for the life of the feedback object rather than per `done`,
	// because the protocol sends it once and re-sends only when it changes.
	Mapping m_Table;
	std::size_t m_Entries = 0;

	DmabufSupport m_Pending;
	DmabufTranche m_Tranche;
	DmabufSupport m_Support;
	std::uint64_t m_Generations = 0;
};

// Prints as main 0xe280 tranche 0xe280 x14 [XR24 mod 0x0, ...], which is the line worth having when a
// host and a device cannot agree on anything: it says what was on offer.
[[nodiscard]] std::string Describe(const DmabufSupport& support);
} // namespace Nested
