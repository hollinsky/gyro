#include "Compositor/Capture.h"

#include <unistd.h>

#include <array>
#include <cstdio>
#include <format>
#include <span>
#include <string>

#include "Core/FrameSection.h"
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

		for (std::uint64_t press = 0; press < 4; ++press)
		{
			// The clients the tests use rather than a range, now that one of them is about pids being
			// far apart: a sweep wide enough to cover 2222 would be a hundred thousand `unlink`s.
			for (const std::uint32_t client : { std::uint32_t{ 1 }, std::uint32_t{ 1111 }, std::uint32_t{ 2222 } })
			{
				for (std::uint32_t surface = 0; surface < 20; ++surface)
				{
					::unlink(std::format("{}/surface-{:08}-{:08}-{:08}.pam", m_Path, press, client, surface).c_str());
				}
			}
		}

		::rmdir(m_Path.c_str());
	}

	Scratch(const Scratch&) = delete;
	Scratch& operator=(const Scratch&) = delete;

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	// What one buffer's file says, or empty where there is none. The header is where a capture puts the
	// facts a picture cannot carry, so reading it back is how the damage list is checked at all.
	// `client` defaults to the one every test that is not about the collision uses, so the argument
	// appears only where two of them are the point.
	[[nodiscard]] std::string Header(std::uint64_t press, std::uint32_t surface, std::uint32_t client = 1) const
	{
		const std::string file = std::format("{}/surface-{:08}-{:08}-{:08}.pam", m_Path, press, client, surface);

		std::FILE* const open = std::fopen(file.c_str(), "rb");

		if (open == nullptr)
		{
			return {};
		}

		std::string header;
		char byte = 0;

		// To `ENDHDR`, which is the last token before the rows and the only place a text read may stop:
		// past it the file is binary and a `getc` loop would be reading pixels as characters.
		while (std::fread(&byte, 1, 1, open) == 1)
		{
			header += byte;

			if (header.ends_with("ENDHDR\n"))
			{
				break;
			}
		}

		static_cast<void>(std::fclose(open));

		return header;
	}

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

// Seam/Capture.h's frame-thread arm, which is one output and not a press.
GYRO_TEST(Capture, ArmsOneOutputForAnExitWithoutTouchingTheOther)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 2 };

	GYRO_REQUIRE(capture.Open().has_value());
	GYRO_REQUIRE(capture.Remember(0, Small, Xrgb8).has_value());
	GYRO_REQUIRE(capture.Remember(1, Small, Xrgb8).has_value());

	// A window closed on one screen owes nothing to the screen beside it, which is the difference from
	// the chord: a press is about the machine and an exit is about one panel.
	GYRO_CHECK(capture.RequestOutput(0));
	GYRO_CHECK(capture.Wanted(0));
	GYRO_CHECK(!capture.Wanted(1));
}

GYRO_TEST(Capture, RefusesAnExitArmForAnOutputItHasNoSlabFor)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 2 };

	GYRO_REQUIRE(capture.Open().has_value());

	// Past the end, and inside it but never remembered. Both are the ordinary answer rather than a
	// fault — the frame that asked has not been forced to do anything yet.
	GYRO_CHECK(!capture.RequestOutput(0));
	GYRO_CHECK(!capture.RequestOutput(7));
}

// The claim the frame loop's `m_ExitOwed` rests on: an exit cannot displace a capture already running,
// because the file it would land in is named for the press that started it.
GYRO_TEST(Capture, DropsAnExitArmWhileAPressIsOutstanding)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());
	GYRO_REQUIRE(capture.Remember(0, Small, Xrgb8).has_value());
	GYRO_REQUIRE(capture.Request() == 1);

	GYRO_CHECK(!capture.RequestOutput(0));

	// And the reverse, which is the case a person hits by pressing the chord during a fade: the exit
	// took the slot first and the press finds nothing to arm.
	GYRO_CHECK(capture.Wanted(0));
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

// Scene/Capture.h's half. The dispatch thread's two verbs, played here exactly as
// Protocol/Surface.cpp plays them.

namespace
{
constexpr PixelSize<BufferSpace> Tiny{ 2, 2 };

// One committed buffer of a colour, with the damage a client claimed for it.
[[nodiscard]] SurfaceCapture Committed(
	std::uint32_t surface,
	std::span<const std::byte> pixels,
	std::span<const PixelRect<BufferSpace>> damage,
	std::uint32_t client = 1
)
{
	return SurfaceCapture{ .Surface = surface,
		                   .Client = client,
		                   .Size = Tiny,
		                   .Stride = 8,
		                   .Alpha = TextureAlpha::Premultiplied,
		                   .Pixels = pixels,
		                   .Damage = damage };
}

// One committed *descriptor*: an extent, a damage list and an id, and no rows at all. What a
// `zwp_linux_dmabuf_v1` client's commit looks like arriving at Scene/Capture.h.
[[nodiscard]] SurfaceCapture Borrowed(
	std::uint32_t surface,
	TextureId texture,
	std::span<const PixelRect<BufferSpace>> damage,
	std::uint32_t client = 1
)
{
	return SurfaceCapture{ .Surface = surface,
		                   .Client = client,
		                   .Size = Tiny,
		                   .Stride = 0,
		                   .Alpha = TextureAlpha::Premultiplied,
		                   .Pixels = {},
		                   .Texture = texture,
		                   .Damage = damage };
}
} // namespace

// **The buffers are held before any press and written by it**, which is the whole reason this half is
// not armed the way the frame half is: a window that repaints wrong and then goes quiet has nothing
// left to offer by the time somebody reaches for the key.
GYRO_TEST(Capture, WritesTheBufferACommitHandedOverBeforeThePress)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	const std::array<std::byte, 16> pixels{};
	const std::array<PixelRect<BufferSpace>, 1> damage{ PixelRect<BufferSpace>{ { 1, 2 }, { 3, 4 } } };

	capture.Offer(Committed(9, pixels, damage));

	// No output has a slab, so the frame half arms nothing — and the buffer is written anyway, because
	// the two halves answer different questions and a run with no panel still has clients.
	GYRO_CHECK_EQ(capture.Request(), std::size_t{ 0 });

	capture.Close();

	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(capture.Failed(), std::uint64_t{ 0 });

	const std::string header = scratch.Header(1, 9);

	GYRO_REQUIRE(!header.empty());

	// The client's own rectangle, in the client's own numbers. This is the fact the whole capture
	// exists for: whether what the client said it repainted covers the row that went stale.
	GYRO_CHECK(header.contains("# damage 1\n"));
	GYRO_CHECK(header.contains("# damage 1 2 3 4\n"));
	GYRO_CHECK(header.contains("# gyro client 1 surface 9 commit 1 2x2 stride 8 argb8888\n"));
}

// **Newest wins per surface**, so a window redrawing at sixty hertz costs one entry and the press
// finds the buffer it would have been showing rather than the first one it ever sent.
GYRO_TEST(Capture, KeepsOnlyTheNewestBufferOfASurface)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	const std::array<std::byte, 16> pixels{};

	capture.Offer(Committed(3, pixels, {}));
	capture.Offer(Committed(3, pixels, {}));
	capture.Offer(Committed(4, pixels, {}));

	static_cast<void>(capture.Request());
	capture.Close();

	// Two surfaces and three commits, so two files — and the surviving one of surface 3 is the second
	// commit, which is what the ordinal in the header is there to prove.
	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 2 });
	GYRO_CHECK(scratch.Header(1, 3).contains("commit 2 "));
	GYRO_CHECK(scratch.Header(1, 4).contains("commit 3 "));
}

// **Two clients own the same wire id, and both windows survive the press.** This is the one that
// shipped: libwayland mints low object ids per connection, so a terminal and a browser started a
// second apart both call their window `wl_surface@18`, and a table keyed on the id alone kept one
// entry for the pair — each repaint of one discarding the other, and the press writing whichever had
// committed last. On a real desktop that is the window a person pressed the key to look at going
// missing from the directory with nothing said.
GYRO_TEST(Capture, KeepsBothClientsWhereTwoOfThemOwnTheSameWireId)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	const std::array<std::byte, 16> pixels{};

	capture.Offer(Committed(18, pixels, {}, 1111));
	capture.Offer(Committed(18, pixels, {}, 2222));

	// And the newest-wins rule still holds *within* a client, which is the half the pair must not lose:
	// this replaces the first client's entry rather than adding a third.
	capture.Offer(Committed(18, pixels, {}, 1111));

	static_cast<void>(capture.Request());
	capture.Close();

	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 2 });

	// Named apart on disk, so neither file is the other's overwrite.
	GYRO_CHECK(scratch.Header(1, 18, 1111).contains("# gyro client 1111 surface 18 commit 3 "));
	GYRO_CHECK(scratch.Header(1, 18, 2222).contains("# gyro client 2222 surface 18 commit 2 "));
}

// A buffer whose rows the pool could not produce — a client that truncated its own file — must not
// become a file. Protocol/Surface.cpp declines to offer one at all; this is the second refusal, in the
// party that would otherwise read past the end of a span.
GYRO_TEST(Capture, DeclinesABufferShorterThanItSaysItIs)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	const std::array<std::byte, 4> stub{};

	capture.Offer(Committed(2, stub, {}));

	GYRO_CHECK_EQ(capture.Request(), std::size_t{ 0 });

	capture.Close();

	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(capture.Failed(), std::uint64_t{ 0 });
	GYRO_CHECK(scratch.Header(1, 2).empty());
}

// **A descriptor is recorded with no pixels and read back at the press**, which is Seam/Capture.h's
// whole split: nothing is copied at the commit because the buffer is still gyro's to read when
// somebody reaches for the key.
GYRO_TEST(Capture, ReadsADescriptorBackAtThePressRatherThanAtTheCommit)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	const TextureId texture{ 7, 1 };
	const std::array<PixelRect<BufferSpace>, 1> damage{ PixelRect<BufferSpace>{ { 5, 6 }, { 7, 8 } } };

	capture.Offer(Borrowed(11, texture, damage));

	// Nothing is owed before the press: the pixels are the client's and stay the client's.
	GYRO_CHECK(!capture.WantsTexture(texture));

	static_cast<void>(capture.Request());

	// The frame thread's half, played by hand exactly as Frame/Loop.h plays it.
	GYRO_REQUIRE(capture.WantsTexture(texture));

	const TextureSlab slab = capture.ReserveTexture(texture);

	GYRO_REQUIRE(slab.IsValid());

	// Tight, four bytes a sample: the rows a readback produces were laid out by the copy rather than
	// by the client, which is why `Stride` arrived as zero and comes back as eight.
	GYRO_CHECK_EQ(slab.Stride, std::uint32_t{ 8 });
	GYRO_CHECK_EQ(slab.Into.size(), std::size_t{ 16 });

	capture.PublishTexture(texture, true);
	capture.Close();

	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(capture.Failed(), std::uint64_t{ 0 });

	// The damage the client claimed, carried from the commit that dropped it to a file written a
	// keystroke later — which is the half of this a screenshot could never hold.
	const std::string header = scratch.Header(1, 11);

	GYRO_REQUIRE(!header.empty());
	GYRO_CHECK(header.contains("# damage 1\n"));
	GYRO_CHECK(header.contains("# damage 5 6 7 8\n"));
	GYRO_CHECK(header.contains("# gyro client 1 surface 11 commit 1 2x2 stride 8 argb8888\n"));
}

// A texture two draw items name — a window and a thumbnail of it — is offered twice on one frame and
// must be read once. The claim is `ReserveTexture`'s compare-exchange out of `Armed`.
GYRO_TEST(Capture, ReadsATextureOnceHoweverManyItemsNameIt)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	const TextureId texture{ 3, 2 };

	capture.Offer(Borrowed(12, texture, {}));

	static_cast<void>(capture.Request());

	GYRO_REQUIRE(capture.ReserveTexture(texture).IsValid());

	// The second item's ask, which finds the entry already claimed.
	GYRO_CHECK(!capture.ReserveTexture(texture).IsValid());
	GYRO_CHECK(!capture.WantsTexture(texture));

	capture.PublishTexture(texture, true);
	capture.Close();

	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 1 });
}

// A readback the renderer refused — no transfer usage, a copy that timed out — hands the slab back
// unwritten. A file of zeroes beside a picture of a window that is plainly drawn reads as a client
// that committed nothing, which is the very bug somebody would be here to diagnose.
GYRO_TEST(Capture, WritesNothingForARefusedTextureReadback)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	const TextureId texture{ 4, 1 };

	capture.Offer(Borrowed(13, texture, {}));

	static_cast<void>(capture.Request());

	GYRO_REQUIRE(capture.ReserveTexture(texture).IsValid());

	{
		// **Inside a frame section, because that is where Frame/Loop.h calls it from**, and handing the
		// entry back is where this used to free the client's rows: the pending record is a *copy* the
		// press allocated, so the frame thread held the only reference and dropping it ran `~Buffer` —
		// megabytes of a window's pixels released under decision 36's ban. It aborted the compositor on
		// the first refused readback of the first press on real hardware, which is a desktop that
		// vanishes the moment somebody asks for a screenshot.
		const FrameSection section;

		capture.PublishTexture(texture, false);
	}

	capture.Close();

	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(capture.Failed(), std::uint64_t{ 0 });
	GYRO_CHECK(scratch.Header(1, 13).empty());
}

// **An armed texture the frame never drew is taken back at the next press**, which is the whole
// reason `Slot::Reserved` exists: a window occluded or on another panel is never offered to
// `ReserveTexture`, and without this its entry would hold the table shut for the rest of the run.
GYRO_TEST(Capture, AbandonsATextureThePreviousPressNeverDrew)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	const TextureId texture{ 5, 1 };

	capture.Offer(Borrowed(14, texture, {}));

	// The first press arms it and no frame ever asks for it.
	static_cast<void>(capture.Request());

	GYRO_REQUIRE(capture.WantsTexture(texture));

	// The second press takes the entry back and arms it again, so the window is still capturable.
	static_cast<void>(capture.Request());

	GYRO_REQUIRE(capture.WantsTexture(texture));
	GYRO_REQUIRE(capture.ReserveTexture(texture).IsValid());

	capture.PublishTexture(texture, true);
	capture.Close();

	// One file, under the second press, because the first press wrote nothing at all.
	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 1 });
	GYRO_CHECK(scratch.Header(1, 14).empty());
	GYRO_CHECK(!scratch.Header(2, 14).empty());
}

// Neither rows nor an id is a commit that adopted nothing — a full texture space, or a layout the
// renderer refused. There is no picture behind it to go and fetch, and the client has already been
// told with `PostNoMemory`.
GYRO_TEST(Capture, DeclinesACommitThatAdoptedNothing)
{
	Scratch scratch;
	PamCapture capture{ scratch.Path(), 1 };

	GYRO_REQUIRE(capture.Open().has_value());

	capture.Offer(Borrowed(15, TextureId{}, {}));

	static_cast<void>(capture.Request());
	capture.Close();

	GYRO_CHECK_EQ(capture.Buffers(), std::uint64_t{ 0 });
	GYRO_CHECK(scratch.Header(1, 15).empty());
}
