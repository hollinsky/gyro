#pragma once

#include <cstddef>
#include <span>

#include "Core/Fd.h"
#include "Core/Result.h"

// A descriptor handed to a client that nothing can change afterwards.
//
// **Two interfaces in this module owe a client a file rather than a message**, and they owe it for the
// same reason: `wl_keyboard.keymap` is tens of kilobytes of xkb text and
// `zwp_linux_dmabuf_feedback_v1.format_table` is sixteen bytes per layout, and neither is something to
// send as wire arguments every time somebody binds. What a client does with one is `mmap` it, and that
// is what makes the sealing load-bearing rather than tidy: a mapping is live memory in the client's
// address space for as long as it wants, and a compositor that could still write into it — or that
// handed over a descriptor the *client* could write into and then hand on — has published something
// mutable to a party that reads it without looking again.
//
// **The descriptor that leaves is a second one, reopened read-only, and the seal goes on the first.**
// Sealing the writable descriptor alone refuses while a mapping is live and would still hand out write
// access; reopening through `/proc/self/fd` and sealing behind it is the arrangement that gives away
// exactly the read.
//
// Linux's, which is what keeps this in a platform module: `memfd_create` and `F_ADD_SEALS` have no
// POSIX spelling, and Core/Fd.h — where a reader would look first — may not name either.
[[nodiscard]] Result<Fd> SealedMemfd(const char* name, std::span<const std::byte> bytes);
