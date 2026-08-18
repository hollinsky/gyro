// fork, waitpid, dup2 and _exit are POSIX rather than ISO C, and glibc hides them under -std=c++NN.
// Same reason Core/Clock.cpp asks: the portable tier names what it wants from the platform.
#define _POSIX_C_SOURCE 200809L

#include "Core/Signal.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <functional>
#include <thread>
#include <vector>

#include "Core/FrameSection.h"
#include "Testing/Test.h"

namespace
{
// The shape every observer in the design has: a long-lived object with a method and a Connection
// member. `m_Action` is what lets one test make a handler do something interesting without a
// separate class per case; it is empty in every test that does not use it, and calling an empty
// std::function does not allocate.
class Observer
{
public:
	Observer(std::vector<int>* log, int id) noexcept : m_Log{ log }, m_Id{ id } {}

	void OnFired()
	{
		m_Log->push_back(m_Id);

		if (m_Action)
		{
			m_Action();
		}
	}

	std::function<void()> m_Action;
	Connection<> m_Link;

private:
	std::vector<int>* m_Log;
	int m_Id;
};

// Allocates nothing in its handler, which is what lets it be used inside a FrameSection.
class Ticker
{
public:
	void OnFired() noexcept { ++m_Count; }

	unsigned m_Count = 0;
	Connection<> m_Link;
};

// The check fires by aborting, so observing it means observing a process death. Same mechanism as
// Core/FrameSection.Test.cpp's, and not shared with it: a test helper reaching across test files is
// a dependency between two binaries that are meant to be independently runnable.
//
// The child is forked before any thread this test spawns exists, so the usual fork-with-threads
// hazard does not arise on the parent's side; the child creates its own thread afterwards.
bool AbortsInChild(void (*body)())
{
	const pid_t child = ::fork();
	if (child < 0)
	{
		return false;
	}

	if (child == 0)
	{
		// The violation report is the expected outcome here, so it goes nowhere rather than into the
		// middle of a passing test run.
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
} // namespace

GYRO_TEST(Signal, FiresInConnectOrder)
{
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };
	Observer third{ &log, 3 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);
	third.m_Link.ConnectTo<&Observer::OnFired>(signal, third);

	GYRO_CHECK_EQ(signal.Count(), std::size_t{ 3 });

	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(log[0], 1);
	GYRO_CHECK_EQ(log[1], 2);
	GYRO_CHECK_EQ(log[2], 3);
}

GYRO_TEST(Signal, ArgumentsReachEveryObserver)
{
	// A reference argument, because that is what Presented carries and passing it to N observers is
	// the case a by-value signature would silently copy.
	struct Info
	{
		int Sequence;
	};

	struct Watcher
	{
		void OnPresented(const Info& info) noexcept { m_Seen = info.Sequence; }

		int m_Seen = 0;
		Connection<const Info&> m_Link;
	};

	Signal<const Info&> signal;
	Watcher left;
	Watcher right;

	left.m_Link.ConnectTo<&Watcher::OnPresented>(signal, left);
	right.m_Link.ConnectTo<&Watcher::OnPresented>(signal, right);

	const Info info{ 42 };
	signal.Emit(info);

	GYRO_CHECK_EQ(left.m_Seen, 42);
	GYRO_CHECK_EQ(right.m_Seen, 42);
}

GYRO_TEST(Signal, DisconnectStopsDelivery)
{
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);

	first.m_Link.Disconnect();

	GYRO_CHECK(!first.m_Link.IsConnected());
	GYRO_CHECK_EQ(signal.Count(), std::size_t{ 1 });

	// Idempotent: a second disconnect is a no-op rather than a corruption of the list.
	first.m_Link.Disconnect();

	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(log[0], 2);
}

GYRO_TEST(Signal, ConnectingAgainRebinds)
{
	std::vector<int> log;
	Signal<> first;
	Signal<> second;

	Observer observer{ &log, 1 };

	observer.m_Link.ConnectTo<&Observer::OnFired>(first, observer);
	observer.m_Link.ConnectTo<&Observer::OnFired>(second, observer);

	GYRO_CHECK(first.IsEmpty());
	GYRO_CHECK_EQ(second.Count(), std::size_t{ 1 });

	first.Emit();
	GYRO_CHECK(log.empty());

	second.Emit();
	GYRO_CHECK_EQ(log.size(), std::size_t{ 1 });
}

// The first of the two lifetime directions device migration produces: an output is unplugged and the
// frame's per-output state is destroyed while the session is still alive and still emitting.
GYRO_TEST(Signal, ObserverDyingFirstIsForgotten)
{
	std::vector<int> log;
	Signal<> signal;

	Observer survivor{ &log, 1 };
	survivor.m_Link.ConnectTo<&Observer::OnFired>(signal, survivor);

	{
		Observer transient{ &log, 2 };
		transient.m_Link.ConnectTo<&Observer::OnFired>(signal, transient);
		GYRO_CHECK_EQ(signal.Count(), std::size_t{ 2 });
	}

	GYRO_CHECK_EQ(signal.Count(), std::size_t{ 1 });

	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(log[0], 1);
}

// The other direction, and the one a design storing only the observer's pointer would get wrong: the
// simpledrm presenter is destroyed and its signals go with it, while the FrameClock observing it
// survives to be re-seeded.
GYRO_TEST(Signal, SignalDyingFirstReleasesObservers)
{
	std::vector<int> log;
	Observer observer{ &log, 1 };

	{
		Signal<> signal;
		observer.m_Link.ConnectTo<&Observer::OnFired>(signal, observer);
		GYRO_CHECK(observer.m_Link.IsConnected());
	}

	// The connection knows, rather than holding a pointer into freed memory that its own destructor
	// would then walk.
	GYRO_CHECK(!observer.m_Link.IsConnected());

	// And disconnecting after the fact is a no-op rather than a use-after-free. This is the line that
	// would fail under a design where only the observer knew about the signal.
	observer.m_Link.Disconnect();
}

GYRO_TEST(Signal, HandlerDisconnectsItself)
{
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);

	first.m_Action = [&] { first.m_Link.Disconnect(); };

	signal.Emit();

	// It fired this time, because it was reached before it left.
	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(log[0], 1);
	GYRO_CHECK_EQ(log[1], 2);

	log.clear();
	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(log[0], 2);
}

// The case the cursor fix-up exists for, and the one a defensive copy of the list would have paid an
// allocation to avoid: the node the emission was about to visit is removed before it gets there.
GYRO_TEST(Signal, HandlerDisconnectsItsSuccessor)
{
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };
	Observer third{ &log, 3 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);
	third.m_Link.ConnectTo<&Observer::OnFired>(signal, third);

	first.m_Action = [&] { second.m_Link.Disconnect(); };

	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(log[0], 1);
	GYRO_CHECK_EQ(log[1], 3);
}

GYRO_TEST(Signal, HandlerDisconnectsAnAlreadyVisitedObserver)
{
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);

	second.m_Action = [&] { first.m_Link.Disconnect(); };

	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(log[0], 1);
	GYRO_CHECK_EQ(log[1], 2);
	GYRO_CHECK_EQ(signal.Count(), std::size_t{ 1 });
}

GYRO_TEST(Signal, HandlerDisconnectsTheWholeList)
{
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };
	Observer third{ &log, 3 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);
	third.m_Link.ConnectTo<&Observer::OnFired>(signal, third);

	first.m_Action = [&] {
		first.m_Link.Disconnect();
		second.m_Link.Disconnect();
		third.m_Link.Disconnect();
	};

	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(log[0], 1);
	GYRO_CHECK(signal.IsEmpty());
}

GYRO_TEST(Signal, ConnectDuringEmitDefersToTheNextEmission)
{
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer late{ &log, 2 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	first.m_Action = [&] { late.m_Link.ConnectTo<&Observer::OnFired>(signal, late); };

	signal.Emit();

	// Connected, and deliberately silent for the emission that was already in flight.
	GYRO_CHECK_EQ(signal.Count(), std::size_t{ 2 });
	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(log[0], 1);

	first.m_Action = nullptr;
	log.clear();
	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(log[1], 2);
}

GYRO_TEST(Signal, ReconnectDuringEmitAlsoDefers)
{
	// The same rule rather than a special case: a reconnect takes a new serial, so an observer that
	// disconnects and immediately reconnects does not fire twice for one emission.
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);

	first.m_Action = [&] {
		second.m_Link.Disconnect();
		second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);
	};

	signal.Emit();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(log[0], 1);
	GYRO_CHECK_EQ(signal.Count(), std::size_t{ 2 });
}

GYRO_TEST(Signal, EmissionsNest)
{
	std::vector<int> log;
	Signal<> signal;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };

	first.m_Link.ConnectTo<&Observer::OnFired>(signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(signal, second);

	bool reentered = false;
	first.m_Action = [&] {
		if (!reentered)
		{
			reentered = true;
			signal.Emit();
		}
	};

	signal.Emit();

	// Outer visits 1, whose handler runs a full inner emission (1 then 2), then the outer continues
	// to 2. Each frame carries its own cursor, which is what keeps the outer walk intact.
	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 4 });
	GYRO_CHECK_EQ(log[0], 1);
	GYRO_CHECK_EQ(log[1], 1);
	GYRO_CHECK_EQ(log[2], 2);
	GYRO_CHECK_EQ(log[3], 2);
}

GYRO_TEST(Signal, SignalDestroyedByItsOwnHandler)
{
	std::vector<int> log;

	Observer first{ &log, 1 };
	Observer second{ &log, 2 };

	auto* signal = new Signal<>{};

	first.m_Link.ConnectTo<&Observer::OnFired>(*signal, first);
	second.m_Link.ConnectTo<&Observer::OnFired>(*signal, second);

	first.m_Action = [&] { delete signal; };

	signal->Emit();

	// The emission stopped rather than walking a destroyed list, and both observers were released on
	// the way out.
	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(log[0], 1);
	GYRO_CHECK(!first.m_Link.IsConnected());
	GYRO_CHECK(!second.m_Link.IsConnected());
}

// The whole point of the design, stated as a test. Connect, emit, and disconnect all inside a frame
// section — under GYRO_FRAME_PATH_CHECK any allocation on any of those three paths aborts here.
GYRO_TEST(Signal, TheWholeLifecycleRunsInsideAFrameSection)
{
	Signal<> signal;
	Ticker first;
	Ticker second;

	{
		const FrameSection section;

		first.m_Link.ConnectTo<&Ticker::OnFired>(signal, first);
		second.m_Link.ConnectTo<&Ticker::OnFired>(signal, second);

		signal.Emit();
		signal.Emit();

		first.m_Link.Disconnect();

		signal.Emit();
	}

	GYRO_CHECK_EQ(first.m_Count, 2u);
	GYRO_CHECK_EQ(second.m_Count, 3u);
}

GYRO_TEST(Signal, EmittingFromASecondThreadAborts)
{
	GYRO_CHECK(AbortsInChild([] {
		Signal<> signal;
		Ticker ticker;
		ticker.m_Link.ConnectTo<&Ticker::OnFired>(signal, ticker);

		// Claims the signal for this thread.
		signal.Emit();

		std::thread intruder{ [&] { signal.Emit(); } };
		intruder.join();
	}));
}

// The mirror of the check above, and the reason the wiring half of it does not exist: decision 41's
// migration destroys the presenter from the composition root while the frame thread is the one that
// has been emitting. ~Signal therefore disconnects every observer from a thread that is not the
// claimed one, on every boot, correctly. A check that caught the hotplug bug would abort here.
GYRO_TEST(Signal, TeardownFromOffTheEmittingThreadIsAllowed)
{
	Ticker ticker;

	{
		Signal<> signal;

		ticker.m_Link.ConnectTo<&Ticker::OnFired>(signal, ticker);

		std::thread frame{ [&] { signal.Emit(); } };
		frame.join();

		GYRO_CHECK_EQ(ticker.m_Count, 1u);

		// The signal dies here, on this thread, having been claimed by the one that just joined.
	}

	GYRO_CHECK(!ticker.m_Link.IsConnected());
}

GYRO_TEST(Signal, WiringIsNotClaimedByAThread)
{
	// The composition root wires on its own thread, the frame thread emits, and teardown happens
	// wherever the owning object dies. Only the emitting thread is fixed.
	Signal<> signal;
	Ticker ticker;

	std::thread root{ [&] { ticker.m_Link.ConnectTo<&Ticker::OnFired>(signal, ticker); } };
	root.join();

	std::thread frame{ [&] { signal.Emit(); } };
	frame.join();

	GYRO_CHECK_EQ(ticker.m_Count, 1u);

	std::thread other{ [&] { ticker.m_Link.Disconnect(); } };
	other.join();

	GYRO_CHECK(signal.IsEmpty());
}
