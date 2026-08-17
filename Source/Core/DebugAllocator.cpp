// posix_memalign and write are POSIX rather than ISO C, and glibc hides both under -std=c++NN.
// Requested in the translation unit for the reason Core/Clock.cpp requests it: the portable tier
// names what it wants from the platform rather than relying on the build asking for GNU extensions.
#define _POSIX_C_SOURCE 200809L

#include "Core/FrameSection.h"

// The enforcing half of decision 36, and the whole file is conditional because it replaces the
// global allocator. On wherever _GLIBCXX_ASSERTIONS is on, which is Debug and RelWithDebInfo —
// see CMake/BuildFlags.cmake. RelWithDebInfo matters more than Debug here: the value is
// concentrated in the headless tests, decision 36's socket-flood assertion and the overrun tests
// are where this actually fires, and those are what CI runs.
#ifdef GYRO_FRAME_PATH_CHECK

#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

// glibc's, and not POSIX, so the report degrades to its first line rather than the file failing to
// compile somewhere without it. That is the right trade for a diagnostic: the abort is the check,
// the backtrace is the convenience.
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define GYRO_HAVE_BACKTRACE 1
#endif

// Partial writes handled and the result consumed, because glibc marks write warn_unused_result
// under _FORTIFY_SOURCE and a cast to void does not suppress that. printf is not an option here:
// the report runs from inside the allocator, and a formatting call that allocates would recurse.
static void WriteAll(const char* text, std::size_t length) noexcept
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

[[noreturn]] static void AbortFramePathViolation(const char* what) noexcept
{
	// Disarmed before anything else. glibc's backtrace() can dlopen libgcc on its first call and
	// allocate doing it, and a check that recursed through its own report would abort with nothing
	// printed — the one outcome worse than not checking. Thread-local, so this disarms the thread
	// that is already failing and no other.
	Detail::FrameSectionDepth = 0;

	static const char prefix[] = "gyro: frame path violation: ";
	static const char suffix[] = " inside a FrameSection (Docs/Decisions.md decision 36)\n";

	WriteAll(prefix, sizeof(prefix) - 1);
	WriteAll(what, std::strlen(what));
	WriteAll(suffix, sizeof(suffix) - 1);

#ifdef GYRO_HAVE_BACKTRACE
	// backtrace_symbols_fd rather than backtrace_symbols: the latter returns a malloc'd array, and
	// this is the one code path in the process that must not ask the allocator for anything.
	void* frames[64];
	::backtrace_symbols_fd(frames, ::backtrace(frames, 64), STDERR_FILENO);
#endif

	std::abort();
}

static void* Allocate(std::size_t size, std::size_t alignment)
{
	if (Detail::FrameSectionDepth != 0)
	{
		AbortFramePathViolation("allocation");
	}

	// operator new(0) owes the caller a distinct non-null pointer, and malloc(0) is permitted to
	// return null. Without this, a zero-sized allocation would be reported as exhaustion.
	if (size == 0)
	{
		size = 1;
	}

	// The new-handler protocol is part of what is being replaced, not an optional extra: code that
	// installs a handler expecting a retry loop gets one, or it silently stops working the moment
	// this file is linked in.
	for (;;)
	{
		void* memory = nullptr;

		if (alignment <= alignof(std::max_align_t))
		{
			memory = std::malloc(size);
		}
		else if (::posix_memalign(&memory, alignment, size) != 0)
		{
			// posix_memalign leaves the pointer unspecified on failure. aligned_alloc is the more
			// obvious call and is wrong here: it requires the size to be a multiple of the
			// alignment, which over-aligned new does not promise.
			memory = nullptr;
		}

		if (memory != nullptr)
		{
			return memory;
		}

		const std::new_handler handler = std::get_new_handler();
		if (handler == nullptr)
		{
			return nullptr;
		}

		handler();
	}
}

static void Deallocate(void* memory) noexcept
{
	// Null first, deliberately. `delete p` on a null pointer reaches no allocator and is ordinary,
	// correct code — a value that happens to be null on the frame path is not a violation, and
	// checking depth first would make it one.
	if (memory == nullptr)
	{
		return;
	}

	if (Detail::FrameSectionDepth != 0)
	{
		AbortFramePathViolation("deallocation");
	}

	std::free(memory);
}

// The complete replaceable set: plain, aligned, nothrow and array forms, and the matching sized and
// aligned deletes. All twenty, and the count is the point — miss one overload and the compiler
// quietly picks the un-replaced version from libstdc++, so the check is absent at exactly the
// unusual allocation that was worth catching.

void* operator new(std::size_t size)
{
	void* memory = Allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	if (memory == nullptr)
	{
		throw std::bad_alloc{};
	}

	return memory;
}

void* operator new[](std::size_t size)
{
	return ::operator new(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
	void* memory = Allocate(size, static_cast<std::size_t>(alignment));
	if (memory == nullptr)
	{
		throw std::bad_alloc{};
	}

	return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
	return ::operator new(size, alignment);
}

// The nothrow forms swallow whatever the new-handler threw rather than forwarding it, which is what
// the standard asks of them. A frame-path violation does not come back through here: it aborted.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	try
	{
		return Allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	}
	catch (...)
	{
		return nullptr;
	}
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
	return ::operator new(size, tag);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	try
	{
		return Allocate(size, static_cast<std::size_t>(alignment));
	}
	catch (...)
	{
		return nullptr;
	}
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept
{
	return ::operator new(size, alignment, tag);
}

// free() takes back what posix_memalign handed out, so the aligned and sized deletes have nothing
// to do differently. They are here so that no call site resolves to a libstdc++ definition, which
// is the only thing that would put a deallocation on the frame path past the check.
void operator delete(void* memory) noexcept
{
	Deallocate(memory);
}

void operator delete[](void* memory) noexcept
{
	Deallocate(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
	Deallocate(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
	Deallocate(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
	Deallocate(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
	Deallocate(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
	Deallocate(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
	Deallocate(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
	Deallocate(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
	Deallocate(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
	Deallocate(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
	Deallocate(memory);
}

#endif
