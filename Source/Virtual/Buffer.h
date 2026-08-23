#pragma once

#include <cstddef>
#include <span>

#include "Core/Fd.h"
#include "Seam/Buffer.h"

// Reading an allocated image with the CPU, which is the half of Seam/Buffer.h that is Linux's.
//
// **The owner moved up and this stayed put, and the line between them is the portable tier.**
// Decision 120 put `DmabufBuffer` and `Mapping` at the waist because `Render` produces them and
// `Nested` consumes them; `Seam` is `PORTABLE`, so `munmap` came with them and `DMA_BUF_IOCTL_SYNC`
// could not. That is not a technicality worth routing around: the ioctl is the one thing on this path
// that a machine without Linux's dma-buf could not answer, and CMake/CheckPortability.cmake is what
// keeps that visible rather than discovered by whoever ports it.
//
// What still uses it is what always did — a test inspecting what was drawn, and the dump's sink
// copying a frame out — and both of those are `Virtual`'s consumers.

// Bracket a CPU read of a buffer's mapping, which `DMA_BUF_IOCTL_SYNC` is what the kernel asks for
// even where the exporter's implementation of it does nothing.
//
// **It is an object rather than two calls because the end is the half that gets forgotten**, and a
// missed `SYNC_END` on an exporter that does have cache maintenance is a stale read on one machine
// and a correct one everywhere it was tested. Failures are dropped, per `Mapping::Reset`: there is no
// recovery from a sync that will not start, and the destructor could not report one anyway. An
// unmapped buffer makes this a no-op rather than an error, so a consumer that wants to look asks for
// the bytes and gets none.
class DmabufRead
{
public:
	explicit DmabufRead(const DmabufBuffer& buffer) noexcept;

	~DmabufRead();

	DmabufRead(const DmabufRead&) = delete;
	DmabufRead& operator=(const DmabufRead&) = delete;
	DmabufRead(DmabufRead&&) = delete;
	DmabufRead& operator=(DmabufRead&&) = delete;

	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return m_Bytes; }

private:
	RawFd m_Descriptor;
	std::span<const std::byte> m_Bytes;
};
