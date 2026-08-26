#include "Seam/Input.h"

#include <vector>

#include "Testing/Test.h"

// The seam only, which for an interface means the vocabulary crossing it: a key is a kernel keycode
// and an instant the device chose, and an implementation is drained like any other source.

namespace
{
class ScriptedInput final : public IInput
{
public:
	[[nodiscard]] RawFd Descriptor() const noexcept override { return RawFd{}; }

	[[nodiscard]] Result<void> Drain() override
	{
		// Copied out first: an observer is entitled to script more input from inside the handler, and
		// walking the vector it is appending to is the reentrancy Core/Signal.h took care to permit.
		const std::vector<KeyEvent> pending = std::move(m_Pending);

		m_Pending.clear();

		for (const KeyEvent& event : pending)
		{
			Key.Emit(event);
		}

		return {};
	}

	void Script(const KeyEvent& event) { m_Pending.push_back(event); }

private:
	std::vector<KeyEvent> m_Pending;
};

struct Recorder
{
	void Observe(const KeyEvent& event) { Seen.push_back(event); }

	std::vector<KeyEvent> Seen;
};
} // namespace

GYRO_TEST(Input, ReportsWhatTheDeviceSaid)
{
	ScriptedInput input;
	Recorder recorder;

	Connection<const KeyEvent&> connection;
	connection.ConnectTo<&Recorder::Observe>(input.Key, recorder);

	const KeyEvent pressed{ .Code = 1, .Pressed = true, .When = Monotonic::FromMicroseconds(1'000) };
	const KeyEvent released{ .Code = 1, .Pressed = false, .When = Monotonic::FromMicroseconds(1'250) };

	input.Script(pressed);
	input.Script(released);

	GYRO_REQUIRE(input.Drain());
	GYRO_REQUIRE(recorder.Seen.size() == 2);
	GYRO_CHECK(recorder.Seen[0] == pressed);
	GYRO_CHECK(recorder.Seen[1] == released);
}

GYRO_TEST(Input, WithNoFileIsAnOrdinarySource)
{
	// An invalid descriptor is what Seam/EventSource.h calls an ordinary answer, and a scripted input
	// is exactly the case it was written for: the events come from a test rather than from a file.
	const ScriptedInput input;

	GYRO_CHECK(!input.Descriptor().IsValid());
}
