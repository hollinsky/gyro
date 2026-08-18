// close is POSIX rather than ISO C, and glibc hides it under -std=c++NN. Requested in the translation
// unit for the reason Core/Clock.cpp requests it: the portable tier names what it wants from the
// platform rather than relying on the build asking for GNU extensions.
#define _POSIX_C_SOURCE 200809L

#include "Core/Fd.h"

#include <unistd.h>

void Fd::Reset(int descriptor) noexcept
{
	if (m_Descriptor >= 0)
	{
		// The return value is deliberately ignored, and retrying on EINTR would be a bug rather than
		// a diligence. Linux has already released the descriptor by the time close() reports the
		// error, so a retry closes whatever number was handed out next — a use-after-close that lands
		// on an unrelated file and reproduces nowhere near here. The error close() can report is
		// about flushing, which is not something an owner going out of scope can act on.
		//
		// This is the one place gyro drops a failure on purpose, which is why it is written down.
		::close(m_Descriptor);
	}

	m_Descriptor = descriptor;
}
