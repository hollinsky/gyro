#include "Drm/Commit.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>

#include "Testing/Test.h"

namespace
{
using namespace std::chrono_literals;

// What the commit thread is pointed at instead of an ioctl. A counter so a test can tell one commit
// from the next, a latch so a test can hold one inside the "ioctl", and an errno to hand back.
struct Issuer
{
	std::atomic<int> Issued{ 0 };
	std::atomic<std::uint32_t> LastFlags{ 0 };
	std::atomic<bool> Release{ true };
	std::atomic<int> Error{ 0 };

	static int Issue(void* context, const Drm::CommitRequest& request) noexcept
	{
		Issuer& self = *static_cast<Issuer*>(context);

		self.LastFlags.store(request.Flags, std::memory_order_relaxed);
		self.Issued.fetch_add(1, std::memory_order_release);

		while (!self.Release.load(std::memory_order_acquire))
		{
			std::this_thread::sleep_for(100us);
		}

		return self.Error.load(std::memory_order_acquire);
	}
};

// Spin until a predicate holds or the patience runs out, so a failing test fails rather than hangs.
template<typename Predicate>
[[nodiscard]] bool Await(Predicate predicate)
{
	for (int attempt = 0; attempt < 20'000; ++attempt)
	{
		if (predicate())
		{
			return true;
		}

		std::this_thread::sleep_for(100us);
	}

	return false;
}

[[nodiscard]] Drm::CommitRequest RequestWith(std::uint32_t flags)
{
	Drm::CommitRequest request;
	request.Flags = flags;
	request.ObjectCount = 1;

	return request;
}

GYRO_TEST(Commit, IssuesWhatItWasArmedWithAndReportsTheOutcome)
{
	Issuer issuer;
	Drm::CommitThread thread;

	thread.Start(&Issuer::Issue, &issuer);

	GYRO_REQUIRE(thread.IsRunning());
	GYRO_CHECK(thread.IsIdle());

	// Nothing is issued until something is armed, which is what makes the idle output cost nothing.
	std::this_thread::sleep_for(2ms);
	GYRO_CHECK(issuer.Issued.load() == 0);

	GYRO_REQUIRE(thread.Arm(RequestWith(0x11)));

	GYRO_REQUIRE(Await([&] { return thread.IsComplete(); }));

	GYRO_CHECK(issuer.Issued.load() == 1);
	GYRO_CHECK(issuer.LastFlags.load() == 0x11);

	const std::optional<Drm::CommitOutcome> outcome = thread.Reap();

	GYRO_REQUIRE(outcome.has_value());
	GYRO_CHECK(outcome->Error == 0);

	// The slot is free again, and only after it has been read.
	GYRO_CHECK(thread.IsIdle());
	GYRO_CHECK(!thread.Reap().has_value());
}

GYRO_TEST(Commit, ASlotHoldingACommitRefusesASecond)
{
	Issuer issuer;
	issuer.Release.store(false);

	Drm::CommitThread thread;
	thread.Start(&Issuer::Issue, &issuer);

	GYRO_REQUIRE(thread.Arm(RequestWith(1)));
	GYRO_REQUIRE(Await([&] { return issuer.Issued.load() == 1; }));

	// In the ioctl. A second arm is refused rather than queued: KMS would refuse the commit anyway, and
	// a queue here would be a queue of commits the kernel throws away.
	GYRO_CHECK(!thread.IsIdle());
	GYRO_CHECK(!thread.Arm(RequestWith(2)));

	// Still not reapable, because the commit has not returned.
	GYRO_CHECK(!thread.Reap().has_value());

	issuer.Release.store(true);

	GYRO_REQUIRE(Await([&] { return thread.IsComplete(); }));
	GYRO_REQUIRE(thread.Reap().has_value());
	GYRO_CHECK(thread.IsIdle());

	// And the slot takes the next one once it has been read.
	GYRO_REQUIRE(thread.Arm(RequestWith(2)));
	GYRO_REQUIRE(Await([&] { return thread.IsComplete(); }));
	GYRO_CHECK(issuer.LastFlags.load() == 2);
}

GYRO_TEST(Commit, AFailureArrivesAsAnOutcomeRatherThanFromArming)
{
	Issuer issuer;
	issuer.Error.store(EBUSY);

	Drm::CommitThread thread;
	thread.Start(&Issuer::Issue, &issuer);

	// Arming succeeds: the frame thread is told the commit was handed over, and the failure is a fact
	// that has not happened yet. This is the whole reason `Missed` is the signal a failure comes back as.
	GYRO_REQUIRE(thread.Arm(RequestWith(0)));
	GYRO_REQUIRE(Await([&] { return thread.IsComplete(); }));

	const std::optional<Drm::CommitOutcome> outcome = thread.Reap();

	GYRO_REQUIRE(outcome.has_value());
	GYRO_CHECK(outcome->Error == EBUSY);
}

GYRO_TEST(Commit, StoppingAThreadInsideACommitJoins)
{
	Issuer issuer;
	issuer.Release.store(false);

	Drm::CommitThread thread;
	thread.Start(&Issuer::Issue, &issuer);

	GYRO_REQUIRE(thread.Arm(RequestWith(0)));
	GYRO_REQUIRE(Await([&] { return issuer.Issued.load() == 1; }));

	// The shutdown lands while the commit is still in the kernel, which is the ordinary case: a panel is
	// nearly always mid-flip. Released on another thread so the join below is the thing being tested.
	std::thread releaser{ [&] {
		std::this_thread::sleep_for(2ms);
		issuer.Release.store(true);
	} };

	thread.Stop();
	releaser.join();

	GYRO_CHECK(!thread.IsRunning());

	// Idempotent, because the destructor calls it again.
	thread.Stop();
}

GYRO_TEST(Commit, AThreadStoppedWhileIdleIssuesNothing)
{
	Issuer issuer;
	Drm::CommitThread thread;

	thread.Start(&Issuer::Issue, &issuer);
	GYRO_REQUIRE(thread.IsRunning());

	thread.Stop();

	GYRO_CHECK(!thread.IsRunning());
	GYRO_CHECK(issuer.Issued.load() == 0);
}
} // namespace
