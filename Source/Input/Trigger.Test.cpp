#include "Input/Trigger.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "Testing/Test.h"

// The development pipe: that a letter written into it is the verb that letter is printed on, that
// what is not a verb is skipped rather than guessed at, and that a writer coming and going does not
// leave the descriptor permanently readable — which is the failure the whole `O_RDWR` open exists to
// prevent, and the one that would be a spin on the dispatch thread rather than a wrong answer.

namespace
{
using namespace Input;

// A path in the directory a test is allowed to write to, unique per test so that two running at once
// do not share a pipe.
[[nodiscard]] std::string TemporaryPath(const char* name)
{
	const char* const directory = std::getenv("TMPDIR");

	return std::string{ directory != nullptr ? directory : "/tmp" } + "/gyro-chord-" + name + "-" +
	       std::to_string(::getpid());
}

// What the trigger emitted, in order.
class Asked
{
public:
	explicit Asked(ChordTrigger& trigger) { m_Slot.ConnectTo<&Asked::Observe>(trigger.Action, *this); }

	void Observe(ChordAction action) { m_Seen.push_back(action); }

	[[nodiscard]] const std::vector<ChordAction>& Seen() const noexcept { return m_Seen; }

private:
	std::vector<ChordAction> m_Seen;
	Connection<ChordAction> m_Slot;
};

// One `echo`, opened and closed exactly as a shell would.
[[nodiscard]] bool Write(const std::string& path, std::string_view text)
{
	const int descriptor = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);

	if (descriptor < 0)
	{
		return false;
	}

	const ::ssize_t written = ::write(descriptor, text.data(), text.size());

	::close(descriptor);

	return written == static_cast<::ssize_t>(text.size());
}
} // namespace

GYRO_TEST(ChordTrigger, TurnsALetterIntoTheVerbThatLetterIsPrintedOn)
{
	const std::string path = TemporaryPath("verbs");

	Result<std::unique_ptr<ChordTrigger>> trigger = ChordTrigger::Open(path);

	GYRO_REQUIRE(trigger.has_value());

	Asked asked{ **trigger };

	GYRO_REQUIRE(Write(path, "s\n"));
	GYRO_REQUIRE((*trigger)->Drain().has_value());

	GYRO_REQUIRE_EQ(asked.Seen().size(), std::size_t{ 1 });
	GYRO_CHECK(asked.Seen().front() == ChordAction::Screenshot);
}

GYRO_TEST(ChordTrigger, ReadsEveryVerbInOneWrite)
{
	const std::string path = TemporaryPath("burst");

	Result<std::unique_ptr<ChordTrigger>> trigger = ChordTrigger::Open(path);

	GYRO_REQUIRE(trigger.has_value());

	Asked asked{ **trigger };

	// The drain goes to empty rather than reading once, so three verbs written together arrive
	// together rather than one per wakeup.
	GYRO_REQUIRE(Write(path, "s t\nq\n"));
	GYRO_REQUIRE((*trigger)->Drain().has_value());

	GYRO_REQUIRE_EQ(asked.Seen().size(), std::size_t{ 3 });
	GYRO_CHECK(asked.Seen()[0] == ChordAction::Screenshot);
	GYRO_CHECK(asked.Seen()[1] == ChordAction::Trace);
	GYRO_CHECK(asked.Seen()[2] == ChordAction::Quit);
}

GYRO_TEST(ChordTrigger, SkipsALetterThatNamesNoVerb)
{
	const std::string path = TemporaryPath("stranger");

	Result<std::unique_ptr<ChordTrigger>> trigger = ChordTrigger::Open(path);

	GYRO_REQUIRE(trigger.has_value());

	Asked asked{ **trigger };

	// **Skipped rather than taken as the nearest verb.** A person who typed the wrong letter gets
	// nothing, which is the same answer the keyboard gives a mistyped verb behind the leader.
	GYRO_REQUIRE(Write(path, "w\n"));
	GYRO_REQUIRE((*trigger)->Drain().has_value());

	GYRO_CHECK(asked.Seen().empty());
}

GYRO_TEST(ChordTrigger, StaysQuietWhenTheLastWriterLeaves)
{
	const std::string path = TemporaryPath("quiet");

	Result<std::unique_ptr<ChordTrigger>> trigger = ChordTrigger::Open(path);

	GYRO_REQUIRE(trigger.has_value());

	Asked asked{ **trigger };

	GYRO_REQUIRE(Write(path, "t\n"));
	GYRO_REQUIRE((*trigger)->Drain().has_value());
	GYRO_REQUIRE_EQ(asked.Seen().size(), std::size_t{ 1 });

	// **The failure this is really about.** With no write end of gyro's own, the descriptor is at
	// end-of-file the moment that `echo` exits — permanently readable, so a loop polling it would
	// spin for the rest of the run. What proves it is not is that the pipe is *unreadable* here: a
	// poll with no timeout finds nothing rather than finding zero bytes.
	::pollfd waiting = { .fd = (*trigger)->Descriptor().Value, .events = POLLIN, .revents = 0 };

	GYRO_CHECK_EQ(::poll(&waiting, 1, 0), 0);

	// And a second drain still says nothing happened rather than reporting the read failed.
	GYRO_CHECK((*trigger)->Drain().has_value());
	GYRO_CHECK_EQ(asked.Seen().size(), std::size_t{ 1 });
}

GYRO_TEST(ChordTrigger, RefusesAPathHoldingSomethingThatIsNotAPipe)
{
	const std::string path = TemporaryPath("regular");

	const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	GYRO_REQUIRE(descriptor >= 0);

	::close(descriptor);

	// **Refused rather than replaced**, because the path came off a command line: a typo naming a
	// real file has to be an error rather than gyro deleting it.
	const Result<std::unique_ptr<ChordTrigger>> trigger = ChordTrigger::Open(path);

	GYRO_CHECK(!trigger.has_value());

	::unlink(path.c_str());
}

GYRO_TEST(ChordTrigger, TakesOverAPipeAPreviousRunLeftBehind)
{
	const std::string path = TemporaryPath("stale");

	GYRO_REQUIRE(::mkfifo(path.c_str(), 0600) == 0 || errno == EEXIST);

	// A run that was killed rather than shut down leaves the file, and the next one must open it
	// instead of refusing to start — which is the whole difference between `EEXIST` being ordinary
	// here and being the refusal above.
	Result<std::unique_ptr<ChordTrigger>> trigger = ChordTrigger::Open(path);

	GYRO_REQUIRE(trigger.has_value());

	Asked asked{ **trigger };

	GYRO_REQUIRE(Write(path, "q"));
	GYRO_REQUIRE((*trigger)->Drain().has_value());

	GYRO_REQUIRE_EQ(asked.Seen().size(), std::size_t{ 1 });
	GYRO_CHECK(asked.Seen().front() == ChordAction::Quit);
}

GYRO_TEST(ChordTrigger, TakesThePipeWithItWhenTheRunEnds)
{
	const std::string path = TemporaryPath("removed");

	{
		Result<std::unique_ptr<ChordTrigger>> trigger = ChordTrigger::Open(path);

		GYRO_REQUIRE(trigger.has_value());
	}

	// A pipe left behind is somebody's next `echo` blocking forever with no reader on the far end.
	struct ::stat status = {};

	GYRO_CHECK(::stat(path.c_str(), &status) != 0);
}
