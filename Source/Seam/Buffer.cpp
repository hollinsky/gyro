// munmap is POSIX rather than ISO C, and glibc hides it under -std=c++NN. Requested here for the
// reason Core/Fd.cpp requests it: what is wanted from the platform is named where it is wanted, and
// POSIX is what the portable tier is allowed to want (CMake/CheckPortability.cmake).
#define _POSIX_C_SOURCE 200809L

#include "Seam/Buffer.h"

#include <sys/mman.h>

void Mapping::Reset() noexcept
{
	if (m_Pixels != nullptr)
	{
		// Dropped for Core/Fd.cpp's reason, and the reasoning transfers exactly: the only way munmap
		// fails is a base or a length that never described a mapping, which is a defect here rather
		// than something the caller could act on, and the caller is usually a destructor that has no
		// way to report it.
		::munmap(m_Pixels, m_Length);
	}

	m_Pixels = nullptr;
	m_Length = 0;
}
