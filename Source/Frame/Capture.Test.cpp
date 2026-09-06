#include "Frame/Capture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "Core/ColorState.h"
#include "Publication/Snapshot.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Exit.h"
#include "World/Node.h"

// What a person sees is what each of these is written against.
//
// A capture that repeats is a fade that stutters for as long as it lasts, because the copy is the
// size of the screen. A capture that is remembered without having happened is a window fading out as
// a rectangle of whatever the device last had in that memory. A capture that follows the *rectangle*
// rather than the reservation is one window wearing the picture of the one that closed before it. A
// capture taken on one screen and not the other is a window that leaves twice, differently, across
// the seam a person has both halves of in view.
//
// **The snapshot is laid out by hand rather than published**, which is the layering: the publisher is
// a dispatch half and `CheckLayering.cmake` forbids the frame side naming it. The two sides are
// proved to agree in Source/Integration/SceneRoundTrip.Test.cpp.

namespace
{
// One published snapshot with the three runs a capture reads out of it.
class Wire
{
public:
	void PutNodes(std::span<const Node> elements) { Stage(m_Nodes, elements); }

	void PutImages(std::span<const ImageContent> elements) { Stage(m_Images, elements); }

	void PutExits(std::span<const ExitSnapshot> elements) { Stage(m_Exits, elements); }

	[[nodiscard]] SnapshotReader Read()
	{
		std::size_t cursor = sizeof(SnapshotHeader);

		for (Staged* run : { &m_Nodes, &m_Images, &m_Exits })
		{
			cursor = Place(*run, cursor);
		}

		SnapshotHeader header{};
		header.Sequence = 1;
		header.ByteSize = static_cast<std::uint32_t>(cursor);
		header.Nodes = m_Nodes.Entry;
		header.Images = m_Images.Entry;
		header.Exits = m_Exits.Entry;

		m_Store.assign((cursor + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t), std::max_align_t{});

		std::byte* const base = reinterpret_cast<std::byte*>(m_Store.data());
		std::memcpy(base, &header, sizeof(SnapshotHeader));

		for (const Staged* run : { &m_Nodes, &m_Images, &m_Exits })
		{
			if (run->Entry.Count != 0)
			{
				std::memcpy(base + run->Entry.Offset, run->Bytes.data(), run->Bytes.size());
			}
		}

		return SnapshotReader{ std::span<const std::byte>{ base, cursor } };
	}

private:
	struct Staged
	{
		std::vector<std::byte> Bytes;
		RunEntry Entry;
	};

	template<typename T>
	static void Stage(Staged& run, std::span<const T> elements)
	{
		run.Bytes.resize(elements.size() * sizeof(T));

		if (!elements.empty())
		{
			std::memcpy(run.Bytes.data(), elements.data(), run.Bytes.size());
		}

		run.Entry = { 0, static_cast<std::uint32_t>(elements.size()), sizeof(T), alignof(T) };
	}

	[[nodiscard]] static std::size_t Place(Staged& run, std::size_t cursor) noexcept
	{
		if (run.Entry.Count == 0)
		{
			return cursor;
		}

		const std::size_t at = Detail::AlignUp(cursor, run.Entry.ElementAlign);
		run.Entry.Offset = static_cast<std::uint32_t>(at);

		return at + run.Bytes.size();
	}

	Staged m_Nodes;
	Staged m_Images;
	Staged m_Exits;
	std::vector<std::max_align_t> m_Store;
};

constexpr TextureId Atlas{ 4, 1 };
constexpr TextureId Surface{ 9, 1 };
constexpr PixelRect<BufferSpace> Slot{ { 0, 0 }, { 400, 300 } };

// A window on its way out: a container with the pixels hanging under it, which is decision 111's
// toplevel and the shape decision 46 reserves against.
[[nodiscard]] std::array<Node, 2> Closing()
{
	std::array<Node, 2> nodes{};

	nodes[0].SubtreeLength = 1;
	nodes[0].Kind = NodeKind::Container;
	nodes[0].Exit = 0;

	nodes[1].Kind = NodeKind::Image;
	nodes[1].Content = 0;

	return nodes;
}

[[nodiscard]] std::array<ImageContent, 1> Pixels()
{
	return { ImageContent{ .Texture = Surface,
		                   .Source = { { 0.0F, 0.0F }, { 400.0F, 300.0F } },
		                   .Frame = {},
		                   .Color = ColorState::Srgb() } };
}

[[nodiscard]] ExitSnapshot Reserved(std::uint32_t reservation, std::uint32_t output = 0)
{
	return ExitSnapshot{ .Node = 0, .Output = output, .Reservation = reservation, .Texture = Atlas, .Slot = Slot };
}
} // namespace

GYRO_TEST(ExitCapture, TheSnapshotIsTakenFromThePixelsUnderTheClosingWindow)
{
	const std::array<Node, 2> nodes = Closing();
	const std::array<ImageContent, 1> images = Pixels();
	const std::array<ExitSnapshot, 1> exits{ Reserved(11) };

	Wire wire;
	wire.PutNodes(nodes);
	wire.PutImages(images);
	wire.PutExits(exits);

	const SnapshotReader snapshot = wire.Read();

	ExitCaptures captures;
	const std::span<const SnapshotCapture> owed = captures.Gather(snapshot, 0);

	GYRO_REQUIRE_EQ(owed.size(), std::size_t{ 1 });

	// The retiring root is a container and carries no pixels of its own, so what is copied is the
	// surface below it — read out of the run rather than published beside the rectangle.
	GYRO_CHECK(owed[0].From == Surface);
	GYRO_CHECK(owed[0].Into == Atlas);
	GYRO_CHECK(owed[0].Slot == Slot);
	GYRO_CHECK(owed[0].Source == images[0].Source);
}

GYRO_TEST(ExitCapture, AWindowsLastFrameIsCopiedOnceRatherThanOnEveryFrameOfItsExit)
{
	const std::array<Node, 2> nodes = Closing();
	const std::array<ImageContent, 1> images = Pixels();
	const std::array<ExitSnapshot, 1> exits{ Reserved(11) };

	Wire wire;
	wire.PutNodes(nodes);
	wire.PutImages(images);
	wire.PutExits(exits);

	const SnapshotReader snapshot = wire.Read();

	ExitCaptures captures;

	GYRO_REQUIRE_EQ(captures.Gather(snapshot, 0).size(), std::size_t{ 1 });

	captures.Landed(1);

	// The window goes on leaving for another thirty frames and the run goes on naming it. Copying it
	// again would be the whole cost of the exit, on every frame of the exit — which is the animation
	// stuttering for exactly as long as somebody is watching it.
	GYRO_CHECK(captures.Gather(snapshot, 0).empty());
	GYRO_CHECK(captures.Holds(0, 11));
}

GYRO_TEST(ExitCapture, ACopyTheRendererDidNotTakeIsStillOwed)
{
	const std::array<Node, 2> nodes = Closing();
	const std::array<ImageContent, 1> images = Pixels();
	const std::array<ExitSnapshot, 1> exits{ Reserved(11) };

	Wire wire;
	wire.PutNodes(nodes);
	wire.PutImages(images);
	wire.PutExits(exits);

	const SnapshotReader snapshot = wire.Read();

	ExitCaptures captures;

	GYRO_REQUIRE_EQ(captures.Gather(snapshot, 0).size(), std::size_t{ 1 });

	// A renderer with no snapshot path takes none, and a frame it refused took none either. Believing
	// otherwise is a window fading out as a rectangle of whatever the device last left there.
	captures.Landed(0);

	GYRO_CHECK_EQ(captures.Gather(snapshot, 0).size(), std::size_t{ 1 });
	GYRO_CHECK(!captures.Holds(0, 11));
}

GYRO_TEST(ExitCapture, TheNextWindowToTakeTheSameRectangleIsCopiedIntoItAgain)
{
	const std::array<Node, 2> nodes = Closing();
	const std::array<ImageContent, 1> images = Pixels();

	Wire first;
	first.PutNodes(nodes);
	first.PutImages(images);

	const std::array<ExitSnapshot, 1> mine{ Reserved(11) };
	first.PutExits(mine);

	const SnapshotReader before = first.Read();

	ExitCaptures captures;

	GYRO_REQUIRE_EQ(captures.Gather(before, 0).size(), std::size_t{ 1 });
	captures.Landed(1);

	// That exit finished, the shelf gave its texels to the next window to close, and the rectangle and
	// the atlas are the same two values as before. Only the count differs — which is the whole reason
	// it crosses, because following the rectangle would put the first window's picture on the second
	// one for the length of its fade.
	Wire second;
	second.PutNodes(nodes);
	second.PutImages(images);

	const std::array<ExitSnapshot, 1> theirs{ Reserved(12) };
	second.PutExits(theirs);

	const SnapshotReader after = second.Read();

	GYRO_CHECK_EQ(captures.Gather(after, 0).size(), std::size_t{ 1 });

	// And the window that has gone is no longer remembered, so the memory is the size of what is
	// leaving rather than of everything that ever left this screen.
	GYRO_CHECK(!captures.Holds(0, 11));
}

GYRO_TEST(ExitCapture, AWindowAcrossTheSeamIsCopiedIntoEachScreensOwnAtlas)
{
	const std::array<Node, 2> nodes = Closing();
	const std::array<ImageContent, 1> images = Pixels();
	const std::array<ExitSnapshot, 2> exits{ Reserved(11, 0), Reserved(12, 1) };

	Wire wire;
	wire.PutNodes(nodes);
	wire.PutImages(images);
	wire.PutExits(exits);

	const SnapshotReader snapshot = wire.Read();

	ExitCaptures captures;

	// One copy each, on the frame each screen composites — decision 190, arriving as two pieces of
	// work rather than one, because a rectangle in one screen's atlas addresses nothing in the other's.
	GYRO_CHECK_EQ(captures.Gather(snapshot, 0).size(), std::size_t{ 1 });
	captures.Landed(1);

	GYRO_CHECK_EQ(captures.Gather(snapshot, 1).size(), std::size_t{ 1 });
	captures.Landed(1);

	GYRO_CHECK(captures.Holds(0, 11) && captures.Holds(1, 12));

	// Neither screen has done the other's work.
	GYRO_CHECK(!captures.Holds(0, 12) && !captures.Holds(1, 11));
}

GYRO_TEST(ExitCapture, AScreenWithNoAtlasCopiesNothingAndItsWindowsCut)
{
	const std::array<Node, 2> nodes = Closing();
	const std::array<ImageContent, 1> images = Pixels();

	// A reservation whose output the device had no room to give an atlas to. Decision 46's exhaustion
	// answer is the same whether the room ran out or was never there: the window finishes at once.
	std::array<ExitSnapshot, 1> exits{ Reserved(11) };
	exits[0].Texture = {};

	Wire wire;
	wire.PutNodes(nodes);
	wire.PutImages(images);
	wire.PutExits(exits);

	const SnapshotReader snapshot = wire.Read();

	ExitCaptures captures;

	GYRO_CHECK(captures.Gather(snapshot, 0).empty());
}

GYRO_TEST(ExitCapture, AClosingWindowWithNoPixelsUnderItIsNotCopiedAndIsNotForgotten)
{
	std::array<Node, 1> nodes{};
	nodes[0].Kind = NodeKind::Container;
	nodes[0].Exit = 0;

	const std::array<ExitSnapshot, 1> exits{ Reserved(11) };

	Wire wire;
	wire.PutNodes(nodes);
	wire.PutExits(exits);

	const SnapshotReader snapshot = wire.Read();

	ExitCaptures captures;

	// Nothing to photograph. It is left owed rather than marked done, so a window whose pixels arrive
	// a frame later is still captured — and one whose pixels never arrive simply never fades from an
	// empty rectangle.
	GYRO_CHECK(captures.Gather(snapshot, 0).empty());
	GYRO_CHECK(!captures.Holds(0, 11));
}

GYRO_TEST(ExitCapture, ASubtreeClaimingMoreNodesThanTheRunHoldsCopiesNothing)
{
	std::array<Node, 2> nodes = Closing();

	// Decision 90's check: the frame thread validates what it walks. A length that runs off the end of
	// the run would otherwise have this scan reading whatever bytes followed and putting them on a
	// screen.
	nodes[0].SubtreeLength = 40;

	const std::array<ImageContent, 1> images = Pixels();
	const std::array<ExitSnapshot, 1> exits{ Reserved(11) };

	Wire wire;
	wire.PutNodes(nodes);
	wire.PutImages(images);
	wire.PutExits(exits);

	const SnapshotReader snapshot = wire.Read();

	ExitCaptures captures;

	GYRO_CHECK(captures.Gather(snapshot, 0).empty());
}

GYRO_TEST(ExitCapture, ANodeIndexThatNamesNothingCopiesNothing)
{
	const std::array<Node, 2> nodes = Closing();
	const std::array<ImageContent, 1> images = Pixels();

	std::array<ExitSnapshot, 1> exits{ Reserved(11) };
	exits[0].Node = 900;

	Wire wire;
	wire.PutNodes(nodes);
	wire.PutImages(images);
	wire.PutExits(exits);

	const SnapshotReader snapshot = wire.Read();

	ExitCaptures captures;

	GYRO_CHECK(captures.Gather(snapshot, 0).empty());
}
