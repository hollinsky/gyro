// open, close, fcntl and dup are POSIX rather than ISO C, and glibc hides them under -std=c++NN.
// Same reason Core/Clock.cpp asks: the portable tier names what it wants from the platform.
#define _POSIX_C_SOURCE 200809L

#include "Core/Fd.h"

#include <fcntl.h>
#include <unistd.h>

#include <format>
#include <utility>

#include "Testing/Test.h"

namespace
{
// A descriptor to own, from somewhere every platform this builds on has.
int OpenSomething() noexcept
{
	return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

// The question every ownership test actually asks. F_GETFD touches nothing and reports EBADF for a
// descriptor the process no longer holds, which is the only way to observe a close from outside.
bool StillOpen(int descriptor) noexcept
{
	return descriptor >= 0 && ::fcntl(descriptor, F_GETFD) != -1;
}
} // namespace

GYRO_TEST(Fd, DefaultOwnsNothing)
{
	const Fd descriptor;

	GYRO_CHECK(!descriptor.IsValid());
	GYRO_CHECK_EQ(descriptor.Get(), InvalidFd);
	GYRO_CHECK(!descriptor.Borrow().IsValid());
}

GYRO_TEST(Fd, ClosesOnDestruction)
{
	const int raw = OpenSomething();
	GYRO_REQUIRE(raw >= 0);

	{
		const Fd descriptor{ raw };
		GYRO_CHECK(descriptor.IsValid());
		GYRO_CHECK(StillOpen(raw));
	}

	GYRO_CHECK(!StillOpen(raw));
}

GYRO_TEST(Fd, MoveTransfersTheClose)
{
	const int raw = OpenSomething();
	GYRO_REQUIRE(raw >= 0);

	Fd source{ raw };
	Fd sink{ std::move(source) };

	// The moved-from owner holds nothing, which is what stops the second close.
	GYRO_CHECK(!source.IsValid());
	GYRO_CHECK_EQ(sink.Get(), raw);
	GYRO_CHECK(StillOpen(raw));

	{
		const Fd consumed{ std::move(sink) };
	}

	GYRO_CHECK(!StillOpen(raw));
}

GYRO_TEST(Fd, MoveAssignmentClosesWhatItReplaces)
{
	const int replaced = OpenSomething();
	const int kept = OpenSomething();
	GYRO_REQUIRE(replaced >= 0 && kept >= 0);

	Fd sink{ replaced };
	Fd source{ kept };

	sink = std::move(source);

	GYRO_CHECK(!StillOpen(replaced));
	GYRO_CHECK(StillOpen(kept));
	GYRO_CHECK_EQ(sink.Get(), kept);
	GYRO_CHECK(!source.IsValid());
}

GYRO_TEST(Fd, SelfMoveAssignmentKeepsTheDescriptor)
{
	const int raw = OpenSomething();
	GYRO_REQUIRE(raw >= 0);

	Fd descriptor{ raw };

	// Written through a reference so that the self-assignment is not one the compiler can see and
	// warn about. Without the guard in operator=, this closes the descriptor and then keeps owning
	// the number — the object looks healthy and the kernel has already taken it back.
	Fd& alias = descriptor;
	descriptor = std::move(alias);

	GYRO_CHECK(descriptor.IsValid());
	GYRO_CHECK(StillOpen(raw));
}

GYRO_TEST(Fd, ReleaseHandsOwnershipOut)
{
	const int raw = OpenSomething();
	GYRO_REQUIRE(raw >= 0);

	int escaped = InvalidFd;

	{
		Fd descriptor{ raw };
		escaped = descriptor.Release();
		GYRO_CHECK(!descriptor.IsValid());
	}

	GYRO_CHECK_EQ(escaped, raw);
	GYRO_CHECK(StillOpen(raw));

	::close(escaped);
	GYRO_CHECK(!StillOpen(raw));
}

GYRO_TEST(Fd, ResetClosesAndAdopts)
{
	const int first = OpenSomething();
	const int second = OpenSomething();
	GYRO_REQUIRE(first >= 0 && second >= 0);

	Fd descriptor{ first };
	descriptor.Reset(second);

	GYRO_CHECK(!StillOpen(first));
	GYRO_CHECK(StillOpen(second));

	descriptor.Reset();

	GYRO_CHECK(!StillOpen(second));
	GYRO_CHECK(!descriptor.IsValid());
}

GYRO_TEST(Fd, BorrowNamesWithoutOwning)
{
	const int raw = OpenSomething();
	GYRO_REQUIRE(raw >= 0);

	{
		const Fd descriptor{ raw };
		const RawFd borrowed = descriptor.Borrow();

		GYRO_CHECK_EQ(borrowed.Value, raw);
		GYRO_CHECK(borrowed.IsValid());

		// The comparison a poller makes on an IEventSource's Descriptor(): the fact it was handed
		// against the descriptor the source holds. No bookkeeping on either side.
		GYRO_CHECK_EQ(borrowed, descriptor.Borrow());
	}

	// The borrow outliving the owner is not prevented and is not meant to be — what the type prevents
	// is the borrow closing anything.
	GYRO_CHECK(!StillOpen(raw));
}

GYRO_TEST(Fd, Formats)
{
	GYRO_CHECK_EQ(std::format("{}", RawFd{ 7 }), "fd 7");
	GYRO_CHECK_EQ(std::format("{}", RawFd{}), "fd none");
	GYRO_CHECK_EQ(std::format("{}", Fd{}), "fd none");

	const int raw = OpenSomething();
	GYRO_REQUIRE(raw >= 0);

	// A real descriptor rather than a literal, because the only numbers that are safe to write down
	// here are the standard streams, and constructing an owning Fd over one of those would close it.
	const Fd descriptor{ raw };
	GYRO_CHECK_EQ(std::format("{}", descriptor), std::format("fd {}", raw));
}
