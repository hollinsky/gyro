#include "Input/Trigger.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>

namespace Input
{
namespace
{
// One read's worth. A person writes one letter at a time and a script writes a handful; the size is
// what makes a single read the ordinary case rather than a capacity anything is expected to reach.
constexpr std::size_t ReadSize = 64;
} // namespace

ChordTrigger::~ChordTrigger()
{
	// Removed at the end of the run for the reason the path is kept: the next `echo` into a FIFO with
	// no reader blocks until somebody notices, and the person doing that is the one who just watched
	// gyro exit.
	//
	// **Best effort, and a failure is deliberately silent.** This runs during shutdown, the file may
	// already be gone, and there is nothing a person could do about a message at this point in the
	// process's life that they would not rather have as a working exit.
	if (!m_Path.empty())
	{
		::unlink(m_Path.c_str());
	}
}

Result<std::unique_ptr<ChordTrigger>> ChordTrigger::Open(std::string path)
{
	if (path.empty())
	{
		return Failure(EINVAL, "the chord pipe needs a path");
	}

	// **Made with the mode written out rather than left to the umask.** gyro runs as root and the
	// person writing into this is not, so a pipe created 0644 is one nobody can use — and the failure
	// would be a permission error in somebody else's shell rather than anything in gyro's log.
	// `mkfifo` applies the umask anyway, so the mode is set again below on the file that exists.
	if (::mkfifo(path.c_str(), 0666) != 0 && errno != EEXIST)
	{
		return FailFromErrno("creating the chord pipe", std::string_view{ path });
	}

	struct ::stat status = {};

	if (::stat(path.c_str(), &status) != 0)
	{
		return FailFromErrno("looking at the chord pipe", std::string_view{ path });
	}

	// A path that already held something else is refused rather than replaced, because the path came
	// off a command line and a typo naming a real file would otherwise be gyro deleting it.
	if (!S_ISFIFO(status.st_mode))
	{
		return Failure(
			EEXIST, "the chord pipe is a path holding something that is not a fifo", std::string_view{ path }
		);
	}

	if (::chmod(path.c_str(), 0666) != 0)
	{
		return FailFromErrno("setting the mode on the chord pipe", std::string_view{ path });
	}

	// `O_RDWR` is the header's trick: gyro holding a write end means the pipe never has zero writers,
	// so a reader never sees the permanent readability that end-of-file is. `O_NONBLOCK` because this
	// is drained from the thread every keystroke on the machine travels on.
	Fd pipe{ ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC) };

	if (!pipe.IsValid())
	{
		return FailFromErrno("opening the chord pipe", std::string_view{ path });
	}

	// The letters come from the table rather than from a string here, because the one thing a verb
	// added to [Chord.h](Chord.h) and forgotten in this line costs is somebody being told the key they
	// just added does not exist.
	std::string letters;

	for (const Verb& verb : Verbs)
	{
		if (!letters.empty())
		{
			letters += ", ";
		}

		letters += verb.Letter;
	}

	spdlog::info("chord pipe at {}: echo a verb into it, one of {}", std::string_view{ path }, letters);

	return std::unique_ptr<ChordTrigger>{ new ChordTrigger{ std::move(pipe), std::move(path) } };
}

Result<void> ChordTrigger::Drain()
{
	// To empty rather than once, which is `Seam/EventSource.h`'s contract and here is also what keeps
	// a burst of verbs from arriving one per wakeup.
	while (true)
	{
		std::array<char, ReadSize> buffer = {};

		const ::ssize_t read = ::read(m_Pipe.Get(), buffer.data(), buffer.size());

		if (read < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return {};
			}

			if (errno == EINTR)
			{
				continue;
			}

			return FailFromErrno("reading the chord pipe");
		}

		// Not reachable while gyro holds its own write end, and answered anyway: a zero-length read is
		// end-of-file, and returning here rather than looping is what keeps the impossible case from
		// being an infinite loop on the dispatch thread.
		if (read == 0)
		{
			return {};
		}

		for (::ssize_t index = 0; index < read; ++index)
		{
			const char letter = buffer[static_cast<std::size_t>(index)];

			// Whitespace is skipped in silence because `echo` appends a newline, so complaining about it
			// would put a line in the log for every correct use.
			if (letter == '\n' || letter == '\r' || letter == ' ' || letter == '\t')
			{
				continue;
			}

			const ChordAction action = VerbForLetter(letter);

			if (action == ChordAction::None)
			{
				spdlog::info("the chord pipe was sent '{}', which names no verb", letter);

				continue;
			}

			Action.Emit(action);
		}
	}
}
} // namespace Input
