#include "Compositor/Capture.h"

#include <unistd.h>

#include <cstdio>
#include <format>
#include <string>

#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The composition root's half of Seam/Capture.h: the arming a chord does, the slab the frame thread
// copies into, and the file that comes out the other side.
//
// **The frame thread's half is played by the test**, which is the whole point of the sink being an
// interface: `Reserve` and `Publish` are called here exactly as Frame/Loop.h calls them, with no
// renderer, no target and no GPU in the way. What that leaves uncovered is the readback itself, which
// is Render/Readback.h's and is checked against a real device in Integration/TargetCapture.Test.cpp.

namespace
{
constexpr PixelFormat Xrgb8{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelSize<DeviceSpace> Small{ 4, 3 };

// Unique to the process, so two runs in parallel do not read each other's captures. Virtual/Pam.
// Test.cpp's `Scratch` for the same reason.
class Scratch
{
public:
	Scratch() : m_Path{ std::format("/tmp/gyro-capture-{}", static_cast<long>(::getpid())) } {}

	~Scratch()
	{
		for (std::uint64_t sequence = 0; sequence < 8; ++sequence)
		{
			::unlink(std::format("{}/frame-{:08}.pam", m_Path, sequence).c_str());
		}

		::rmdir(m_Path.c_str());
	}

	Scratch(const Scratch&) = delete;
	Scratch& operator=(const Scratch&) = delete;

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	[[nodiscard]] bool Holds(std::uint64_t sequence) const
	{
		const std::string file = std::format("{}/frame-{:08}.pam", m_Path, sequence);

		return ::access(file.c_str(), F_OK) == 0;
	}

private:
	std::string m_Path;
};
} // namespace

GYRO_TEST(Capture, WantsNothingUntilTheChordFires)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());
	GYRO_REQUIRE(capture.Remember(0, Small, Xrgb8).has_value());

	// The steady state, and it is the one the frame loop asks about on every frame of every run: no
	// capture is owed, so the ceiling stays the presenter's and nothing is forced to composite.
	GYRO_CHECK(!capture.Wanted(0));
	GYRO_CHECK(capture.Reserve(0, Small, Xrgb8, 16).empty());
}

GYRO_TEST(Capture, ArmsOnlyOutputsItHasASlabFor)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 2 };

	GYRO_REQUIRE(capture.Open().has_value());
	GYRO_REQUIRE(capture.Remember(0, Small, Xrgb8).has_value());

	// One remembered, one not — which is a second panel that came up after the capture was built, or
	// one whose format has no decodable sample width. The chord arms what it can and says how many.
	GYRO_CHECK_EQ(capture.Request(), std::size_t{ 1 });
	GYRO_CHECK(capture.Wanted(0));
	GYRO_CHECK(!capture.Wanted(1));
}

GYRO_TEST(Capture, WritesAFileForACompletedReadback)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());
	GYRO_REQUIRE(capture.Remember(0, Small, Xrgb8).has_value());
	GYRO_REQUIRE(capture.Request() == 1);

	const std::span<std::byte> into = capture.Reserve(0, Small, Xrgb8, 16);

	GYRO_REQUIRE(into.size() == 16U * 3U);

	for (std::byte& byte : into)
	{
		byte = std::byte{ 0x40 };
	}

	capture.Publish(0, 7, true);

	// Joins the writer, which is what makes the assertion below a fact rather than a race.
	capture.Close();

	GYRO_CHECK_EQ(capture.Written(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(capture.Failed(), std::uint64_t{ 0 });
	GYRO_CHECK(scratch.Holds(7));

	// And the slot is idle again, so the next press is armable.
	GYRO_CHECK(!capture.Wanted(0));
}

// A readback the renderer refused — no `--capture` on the device, a copy that timed out — must leave
// no file at all. A half-written PAM in a directory somebody is watching reads as a compositor that
// drew half a frame, which is a worse report than nothing.
GYRO_TEST(Capture, WritesNothingForARefusedReadback)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());
	GYRO_REQUIRE(capture.Remember(0, Small, Xrgb8).has_value());
	GYRO_REQUIRE(capture.Request() == 1);
	GYRO_REQUIRE(!capture.Reserve(0, Small, Xrgb8, 16).empty());

	capture.Publish(0, 3, false);
	capture.Close();

	GYRO_CHECK_EQ(capture.Written(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(capture.Failed(), std::uint64_t{ 0 });
	GYRO_CHECK(!scratch.Holds(3));
}

// A mode set since the last `Remember`. Growing the slab here would be an allocation on the frame
// thread, which Core/FrameSection.h aborts on — so the capture is dropped and the arming with it.
GYRO_TEST(Capture, RefusesAShapeItsSlabWasNotSizedFor)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());
	GYRO_REQUIRE(capture.Remember(0, Small, Xrgb8).has_value());
	GYRO_REQUIRE(capture.Request() == 1);

	GYRO_CHECK(capture.Reserve(0, PixelSize<DeviceSpace>{ 8, 3 }, Xrgb8, 32).empty());

	// Disarmed by the refusal rather than left armed, so the next frame does not go on forcing a full
	// composite for a picture that can never be taken.
	GYRO_CHECK(!capture.Wanted(0));
}

// A second press while one is outstanding. The header's argument: a screenshot is one instant a
// person chose, so the useful behaviour on a slow disk is to do nothing rather than to photograph a
// moment that has already passed.
GYRO_TEST(Capture, DropsASecondPressWhileOneIsOutstanding)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());
	GYRO_REQUIRE(capture.Remember(0, Small, Xrgb8).has_value());
	GYRO_REQUIRE(capture.Request() == 1);

	GYRO_CHECK_EQ(capture.Request(), std::size_t{ 0 });
}
