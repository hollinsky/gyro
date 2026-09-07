#pragma once

#include <string>
#include <string_view>

#include "Core/Result.h"

// What gyro says to the service manager, and the whole of what it says.
//
// **One datagram to an `AF_UNIX` socket, and no libsystemd.** The protocol is newline-separated
// `KEY=VALUE` lines sent to the address in `NOTIFY_SOCKET`, which is forty lines of socket code —
// and [decision
// 49](../../Docs/Decisions.md#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)
// already priced it that way, because the alternative brings a library whose other half is the bus
// [decision 7](../../Docs/Decisions.md#7-session-claiming-is-deferred-basu-rejected) declined to
// take. The kernel attaches the sender's credentials itself, so the socket needs no bind, no
// connect and no name of its own.
//
// **`READY=1` is the whole of it today, and what it buys is that a failure to start is reported as
// one.** Under `Type=simple` the service manager calls gyro started the instant it has forked, so a
// compositor that dies initializing Vulkan looks like one that started and then stopped, and
// anything ordered after it — the login agent, once it exists — starts against a machine with no
// control socket. Under `Type=notify` the unit is *activating* until this datagram arrives, so
// ordering means what it says and `TimeoutStartSec=` catches a startup that wedges.
//
// **What is deliberately not here, because it is not one line each:**
//
// - **`FDSTORE=1`, which is the mechanism decision 49 actually rests on.** Sending it needs
//   `SCM_RIGHTS` and, more to the point, needs the other half: a restarted gyro that does not take
//   the descriptor back out of the store through `LISTEN_FDS` opens the card node fresh, and
//   `Drm/Device.cpp` refuses that with *another process is DRM master of* — the store is still
//   holding the old file description, which is exactly why the picture survived. Storing without
//   adopting turns a restart from a black screen into a compositor that will not start, so the two
//   are one commit and it is not this one.
// - **`WATCHDOG=1`.** It is the only thing that catches the failure `RLIMIT_RTTIME` cannot — a frame
//   thread blocked on a driver lock accrues no real-time budget and trips nothing — and it is worth
//   nothing until the store above exists, since the restart it forces would take the last frame off
//   the glass. It also cannot be pinged from the frame thread without putting a syscall on the frame
//   path, so what it watches wants deciding rather than assuming.
// - **`STOPPING=1`.** gyro's shutdown does block, on the modeset the kernel performs inside
//   `close()` of the DRM descriptor, but it blocks for a frame rather than for a timeout somebody
//   would otherwise be waiting out.

// The address `NOTIFY_SOCKET` names, or empty where there is none — which is every development run,
// and is why nothing here treats its absence as a failure.
[[nodiscard]] std::string ServiceManagerAddress();

// `READY=1`, with a sentence beside it for `systemctl status` to show.
//
// **The status line is not decoration on a machine with no VT.** It is the one place a person who
// cannot see the screen can read what gyro thinks it is doing, and `journalctl` is the alternative
// only if they already know the run started.
//
// The address is a parameter for the reason `Session/Control.h`'s runtime root is one: a test cannot
// have a service manager listening, and reading the environment inside the call would put the one
// thing that must be exercised behind the one thing a test cannot arrange.
[[nodiscard]] Result<void> NotifyReady(std::string_view address, std::string_view status);
