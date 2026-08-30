#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Fd.h"
#include "Core/Result.h"

// One datagram on or off a handover socket, with the descriptor that rides beside it.
//
// **Both ends of the handover are here, and that is the point of the file.** gyro accepts these
// connections and the agent makes one; a `sendmsg` written twice is two chances to disagree about
// `MSG_NOSIGNAL`, about what a short return means, and about who closes a descriptor that arrived on
// a message nobody wanted. Session/Control.h is the policy over this and Session/Handover.h is the
// vocabulary under it; this is the syscall between them and it decides nothing.
//
// **Nothing here reads the message.** A datagram comes back as bytes and a descriptor count, and
// whether those two agree is `DescriptorsFor`'s question — asked by the receiver, because the answer
// is different at each end.

namespace Session
{
// How many descriptors are lifted off one datagram before the rest are dropped.
//
// **Four, where the protocol's largest answer is one.** The number is not a capacity to grow into: a
// peer choosing to attach eight is a peer this end is going to refuse, and the only thing the extra
// room buys is that the refusal can *say* how many arrived instead of reporting the truncation the
// kernel would perform at exactly one. What it costs is three descriptors held for the microseconds
// before the refusal closes them.
inline constexpr std::size_t MaxAttached = 4;

// What a receive found. `Empty` is an ordinary answer — Seam/EventSource.h's drain reads to empty, so
// the last read of every wakeup is one of these.
enum class ReceiveState : std::uint8_t
{
	Message,
	Empty,
	Ended,
};

// A datagram, and everything the caller has to judge it by.
//
// **`Attached` owns**, which is what makes a refused message safe: the descriptors close when this
// goes out of scope, so a receiver that decides the message is nonsense does nothing further and
// leaks nothing. Fd is move-only, so this struct is too.
struct Received
{
	ReceiveState State = ReceiveState::Empty;

	// Bytes written into the caller's buffer. Never more than the buffer holds, even where the
	// datagram was longer — see `Truncated`.
	std::size_t Bytes = 0;

	std::array<Fd, MaxAttached> Attached{};
	std::size_t AttachedCount = 0;

	// The datagram did not fit, in bytes or in descriptors. **A refusal rather than a partial read**:
	// a sequenced-packet socket has already discarded the rest, so what is in hand is a prefix of a
	// message from a peer speaking something this build does not.
	bool Truncated = false;
};

// Send one datagram, with at most one descriptor attached. `attached` invalid sends none.
//
// The whole message goes in one call or the call fails: a sequenced-packet socket does not do partial
// writes, so there is no resume path to write and none to test.
[[nodiscard]] Result<void> Send(RawFd socket, std::span<const std::byte> message, RawFd attached) noexcept;

// Take one datagram, without blocking.
[[nodiscard]] Result<Received> Receive(RawFd socket, std::span<std::byte> into) noexcept;
} // namespace Session
