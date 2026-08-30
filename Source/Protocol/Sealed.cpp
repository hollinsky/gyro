#include "Protocol/Sealed.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <utility>

// Sealing arrived after the rest of `fcntl.h` and a toolchain may still be describing an older
// kernel's headers. Protocol/Shm.cpp names the same constants for the same reason.
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif

#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif

#ifndef F_SEAL_GROW
#define F_SEAL_GROW 0x0004
#endif

#ifndef F_SEAL_WRITE
#define F_SEAL_WRITE 0x0008
#endif

Result<Fd> SealedMemfd(const char* name, std::span<const std::byte> bytes)
{
	Fd writable{ ::memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING) };

	if (!writable.IsValid())
	{
		return Failure(errno, "creating a sealed descriptor");
	}

	for (std::size_t written = 0; written < bytes.size();)
	{
		const ssize_t step = ::write(writable.Borrow().Value, bytes.data() + written, bytes.size() - written);

		if (step <= 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			return Failure(errno, "filling a sealed descriptor");
		}

		written += static_cast<std::size_t>(step);
	}

	const std::string path = "/proc/self/fd/" + std::to_string(writable.Borrow().Value);

	Fd sealed{ ::open(path.c_str(), O_RDONLY | O_CLOEXEC) };

	if (!sealed.IsValid())
	{
		return Failure(errno, "reopening a sealed descriptor read-only");
	}

	if (::fcntl(writable.Borrow().Value, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0)
	{
		return Failure(errno, "sealing a descriptor");
	}

	return sealed;
}
