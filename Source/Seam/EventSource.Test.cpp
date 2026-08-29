#include "Seam/EventSource.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Seam/PresentationInfo.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// This interface is two functions and an argument, and the argument is the part worth testing: a
// source is the *backend's* rather than an output's, because the descriptor it wraps is one file
// serving every presenter on the device. So what is here is a fake device with two presenters on one
// queue, and the assertions that would catch a drain which delivered a flip to the wrong one, stopped
// before the queue was empty, or could only run on a source that had a file to wait on at all.
//
// The presenter is a stub rather than Presenter.Test.cpp's fake. Nothing here presents; what is
// needed is somewhere for a signal to be emitted from, and the smallest thing with that property says
// so more clearly than a second copy of a backend.

namespace
{
class StubPresenter final : public IPresenter
{
public:
	[[nodiscard]] std::span<const RenderTarget> Targets() const override { return {}; }

	[[nodiscard]] std::optional<std::uint32_t> AcquireTarget() override { return std::nullopt; }

	Result<void> Present(std::span<const PresentLayer>, PresentTrace) override { return {}; }

	void Reconfigure(const OutputConfiguration&) override {}
};

// One device, two outputs, one queue of completions across both — which is the arrangement KMS
// actually has and the one the nested backend has for two host windows on a connection.
class FakeDevice final : public IEventSource
{
public:
	struct Completion
	{
		std::uint32_t Output = 0;
		std::uint64_t Sequence = 0;
	};

	explicit FakeDevice(RawFd descriptor) noexcept : m_Descriptor{ descriptor } {}

	[[nodiscard]] RawFd Descriptor() const noexcept override { return m_Descriptor; }

	Result<void> Drain() override
	{
		if (m_Failure != 0)
		{
			return std::unexpected{ Error{ m_Failure, "draining the fake device" } };
		}

		// To empty, never once. A drain that returned after the first event would leave the loop's
		// poll immediately readable again.
		for (const Completion& completion : m_Queued)
		{
			PresentationInfo info;
			info.Sequence = completion.Sequence;
			info.HardwareClock = true;

			m_Presenters[completion.Output].Presented.Emit(info);
		}

		m_Queued.clear();
		++m_Drains;

		return {};
	}

	void Queue(std::uint32_t output, std::uint64_t sequence) { m_Queued.push_back({ output, sequence }); }

	void FailWith(int code) noexcept { m_Failure = code; }

	[[nodiscard]] IPresenter& Output(std::uint32_t index) noexcept { return m_Presenters[index]; }

	[[nodiscard]] int Drains() const noexcept { return m_Drains; }

private:
	std::array<StubPresenter, 2> m_Presenters;
	std::vector<Completion> m_Queued;
	RawFd m_Descriptor;
	int m_Failure = 0;
	int m_Drains = 0;
};

class Watcher
{
public:
	void Watch(IPresenter& presenter) { m_Presented.ConnectTo<&Watcher::OnPresented>(presenter.Presented, *this); }

	void OnPresented(const PresentationInfo& info) { Frames.push_back(info); }

	std::vector<PresentationInfo> Frames;

private:
	Connection<const PresentationInfo&> m_Presented;
};
} // namespace

// The claim decision 80 rests on. One descriptor serves every presenter on the device, so the drain
// is where a completion is attributed to an output — and an implementation that broadcast instead
// would pass every test that only ever built one output.
GYRO_TEST(EventSource, OneSourceFrontsEveryPresenterOnItsDevice)
{
	FakeDevice device{ RawFd{ 7 } };

	Watcher first;
	Watcher second;
	first.Watch(device.Output(0));
	second.Watch(device.Output(1));

	device.Queue(1, 4210);

	GYRO_REQUIRE(device.Drain().has_value());

	GYRO_CHECK_EQ(first.Frames.size(), std::size_t{ 0 });
	GYRO_REQUIRE_EQ(second.Frames.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(second.Frames[0].Sequence, std::uint64_t{ 4210 });
}

// One call empties the queue. The hazard this guards is not a lost event — a level-triggered poll
// would wake again and the event would arrive late rather than never — it is the loop spinning at
// whatever rate the source produces, which presents as power draw rather than as a missed frame.
GYRO_TEST(EventSource, DrainsToEmptyRatherThanOnce)
{
	FakeDevice device{ RawFd{ 7 } };

	Watcher watcher;
	watcher.Watch(device.Output(0));

	device.Queue(0, 1);
	device.Queue(0, 2);
	device.Queue(0, 3);

	GYRO_REQUIRE(device.Drain().has_value());

	GYRO_CHECK_EQ(watcher.Frames.size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(device.Drains(), 1);
}

// A wakeup that some other source caused reaches every source, because the loop does not ask which
// one was ready. Finding nothing is the ordinary result and it is success.
GYRO_TEST(EventSource, AnEmptyDrainSucceeds)
{
	FakeDevice device{ RawFd{ 7 } };

	Watcher watcher;
	watcher.Watch(device.Output(0));

	GYRO_CHECK(device.Drain().has_value());
	GYRO_CHECK_EQ(watcher.Frames.size(), std::size_t{ 0 });
}

// The headless shape, and the second argument against handing the loop a readiness set. Nothing
// becomes readable when a simulated flip falls due, so a source with no descriptor could never be in
// such a set — and headless would present nothing, which is the configuration every scheduling test
// runs on.
GYRO_TEST(EventSource, ASourceNeedNotBePollable)
{
	FakeDevice device{ RawFd{} };

	Watcher watcher;
	watcher.Watch(device.Output(0));

	GYRO_CHECK(!device.Descriptor().IsValid());

	device.Queue(0, 99);

	GYRO_REQUIRE(device.Drain().has_value());
	GYRO_REQUIRE_EQ(watcher.Frames.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(watcher.Frames[0].Sequence, std::uint64_t{ 99 });
}

// Present()'s vocabulary, because it is the same file failing the same ways. The distinction that
// matters to a caller is that this is not a refusal to retry: the device is gone, and that is the
// composition root's problem rather than the loop's.
GYRO_TEST(EventSource, AFailedDrainReportsAtTheCallSite)
{
	FakeDevice device{ RawFd{ 7 } };
	device.FailWith(ENODEV);

	const Result<void> drained = device.Drain();

	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), ENODEV);
}
