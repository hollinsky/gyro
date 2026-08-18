// write is POSIX rather than ISO C, and glibc hides it under -std=c++NN. Requested in the translation
// unit for the reason Core/Clock.cpp requests it: the portable tier names what it wants from the
// platform rather than relying on the build asking for GNU extensions.
#define _POSIX_C_SOURCE 200809L

#include "Core/Signal.h"

#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace
{
// Partial writes handled and the result consumed, because glibc marks write warn_unused_result under
// _FORTIFY_SOURCE and a cast to void does not suppress that. Same shape as Core/DebugAllocator.cpp's,
// and deliberately not shared with it: that one runs from inside the allocator and must not gain a
// dependency on anything, which is a constraint worth leaving alone for four lines.
void WriteAll(const char* text, std::size_t length) noexcept
{
	while (length != 0)
	{
		const ssize_t written = ::write(STDERR_FILENO, text, length);
		if (written <= 0)
		{
			return;
		}

		text += written;
		length -= static_cast<std::size_t>(written);
	}
}
} // namespace

namespace Detail
{
// Written rather than printed, for the reason DebugAllocator writes: one of the threads that can
// reach this is inside a frame section, where a formatting call would allocate and turn the report
// into a second abort with nothing said.
[[noreturn]] void ReportSignalThreadViolation(const char* what) noexcept
{
	static const char prefix[] = "gyro: signal thread violation: ";
	static const char suffix[] = " (Docs/Structure.md: signals are intra-thread only)\n";

	WriteAll(prefix, sizeof(prefix) - 1);
	WriteAll(what, std::strlen(what));
	WriteAll(suffix, sizeof(suffix) - 1);

	std::abort();
}
} // namespace Detail
