// fork, waitpid, dup2 and _exit are POSIX rather than ISO C, and glibc hides them under -std=c++NN.
// Same reason Core/Clock.cpp asks: the portable tier names what it wants from the platform.
#define _POSIX_C_SOURCE 200809L

#include "Core/FrameSection.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <thread>
#include <vector>

#include "Testing/Test.h"

GYRO_TEST(FrameSection, DepthNests)
{
	GYRO_CHECK(!OnFramePath());

	{
		const FrameSection outer;
		GYRO_CHECK(OnFramePath());

		{
			const FrameSection inner;
			GYRO_CHECK(OnFramePath());
		}

		// A flag would have been cleared here, and the frame loop's outermost section will contain
		// narrower ones the moment anything else wants to assert against the frame path.
		GYRO_CHECK(OnFramePath());
	}

	GYRO_CHECK(!OnFramePath());
}

GYRO_TEST(FrameSection, DepthUnwinds)
{
	try
	{
		const FrameSection section;
		throw 0;
	}
	catch (int)
	{}

	// The interesting case is not the throw, it is the frame after it: a section left armed by an
	// escaping exception turns one failure into every subsequent allocation aborting.
	GYRO_CHECK(!OnFramePath());
}

GYRO_TEST(FrameSection, DepthIsThreadLocal)
{
	// The shape decision 45 produced, in miniature: the dispatch thread allocating freely while the
	// frame thread is inside its section. A process-wide ban would fail this test, which is the
	// point of writing it — it is the design constraint, not an implementation detail.
	std::atomic<bool> ready{ false };
	std::atomic<bool> done{ false };
	std::atomic<bool> sawFramePath{ true };

	std::thread worker{ [&] {
		while (!ready.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}

		sawFramePath.store(OnFramePath(), std::memory_order_relaxed);

		std::vector<int> grown;
		grown.reserve(4096);

		done.store(true, std::memory_order_release);
	} };

	// Entered after the thread exists, because constructing one allocates.
	{
		const FrameSection section;

		ready.store(true, std::memory_order_release);
		while (!done.load(std::memory_order_acquire))
		{
			// Neither yield nor a relaxed atomic load allocates, which is what lets this spin
			// inside the section rather than around it.
			std::this_thread::yield();
		}
	}

	worker.join();

	GYRO_CHECK(!sawFramePath.load(std::memory_order_relaxed));
}

#ifdef GYRO_FRAME_PATH_CHECK

// The check fires by aborting, so observing it means observing a process death. fork is the whole
// mechanism: the child inherits the replaced allocator and dies, the parent reads the signal.
//
// Every thread this binary spawns is joined before here, so the child is single-threaded and the
// usual fork-with-threads hazard — inheriting a malloc arena lock held by a thread that does not
// exist on this side — does not arise.
static bool AbortsInChild(void (*body)())
{
	const pid_t child = ::fork();
	if (child < 0)
	{
		return false;
	}

	if (child == 0)
	{
		// The violation report is the expected outcome here, so it goes nowhere rather than into
		// the middle of a passing test run.
		const int null = ::open("/dev/null", O_WRONLY);
		if (null >= 0)
		{
			::dup2(null, STDERR_FILENO);
		}

		body();

		// Reached only if the check did not fire, which the parent reads as a normal exit.
		::_exit(0);
	}

	int status = 0;
	if (::waitpid(child, &status, 0) != child)
	{
		return false;
	}

	return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

GYRO_TEST(FrameSection, AllocationInsideAborts)
{
	// Also the canary for the allocator being linked in at all. A replacement operator new that no
	// symbol references is exactly the kind of thing a build change drops silently, and this is the
	// test that notices — Core is an OBJECT library for that reason, see the CMakeLists comment.
	GYRO_CHECK(AbortsInChild([] {
		const FrameSection section;

		void* volatile memory = ::operator new(64);
		static_cast<void>(memory);
	}));
}

GYRO_TEST(FrameSection, DeallocationInsideAborts)
{
	GYRO_CHECK(AbortsInChild([] {
		void* memory = ::operator new(64);

		const FrameSection section;
		::operator delete(memory);
	}));
}

GYRO_TEST(FrameSection, NullDeleteInsideIsNotAViolation)
{
	// `delete p` where p is null reaches no allocator. Aborting on it would make correct, ordinary
	// code a violation, which is the false positive that gets a check like this turned off.
	const FrameSection section;

	void* memory = nullptr;
	::operator delete(memory);
}

GYRO_TEST(FrameSection, AllocationOutsideIsFine)
{
	std::vector<int> grown;
	grown.reserve(4096);

	GYRO_CHECK_EQ(grown.capacity() >= 4096, true);
}

#endif
