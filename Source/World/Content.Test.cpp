#include "World/Content.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include "Testing/Test.h"

// The runtime half of Content.h's contract. The sizes, the alignments, and the trivially-copyable
// claim are the static_assert block at the foot of that header and are not repeated here — but the
// claim those assertions are *for* is not a compile-time one, so it is here: these records reach the
// frame thread as bytes at an offset in a snapshot, written by a publisher that value-initialises and
// read by a reader that never calls a constructor.
//
// So the fixture is that boundary, reduced to what it actually does to a record — a memcpy out and a
// memcpy back, at an offset that is not the start of the buffer, because an element run never begins
// at one. What is asserted on the far side is equality, which is the whole promise: nothing about
// either record survives only in the writer's process.
//
// The rest is the defaults, which read better as tests than as assertions because each one is a claim
// about what an *unfinished* record means on screen rather than about what a field is set to.

namespace
{
// A run of content as the waist carries it: a byte buffer, a base offset that is not zero, and an
// element size the writer declared. Deliberately not the real Snapshot — World may not name
// Publication, and the point being made is about the record rather than about the layout around it.
template<typename T>
[[nodiscard]] std::vector<std::byte> Publish(const std::vector<T>& elements, std::size_t offset)
{
	std::vector<std::byte> bytes(offset + elements.size() * sizeof(T));

	for (std::size_t index = 0; index < elements.size(); ++index)
	{
		std::memcpy(bytes.data() + offset + index * sizeof(T), &elements[index], sizeof(T));
	}

	return bytes;
}

template<typename T>
[[nodiscard]] T Read(const std::vector<std::byte>& bytes, std::size_t offset, std::size_t index)
{
	T element;
	std::memcpy(&element, bytes.data() + offset + index * sizeof(T), sizeof(T));
	return element;
}

// A window mid-exit, which is the case decision 95 collapses two node kinds into one for: the same
// record whether the texture is the client's live surface or the snapshot that replaced it. The
// frame rect is inset from the extent by the shadow margin a client declared, because that is the
// arrangement decision 96's rounding has to survive.
[[nodiscard]] ImageContent Window()
{
	return {
		.Texture = TextureId{ 12, 3 },
		.Source = { { 0.0F, 0.0F }, { 1280.0F, 720.0F } },
		.Frame = { { 32.0F, 32.0F }, { 1216.0F, 656.0F } },
		.Color = ColorState::Srgb(),
	};
}
} // namespace

GYRO_TEST(Content, ImageSurvivesTheByteRoundTrip)
{
	const ImageContent window = Window();

	// Two elements at an offset, so a record that read its neighbour's bytes or the buffer's head
	// would not pass by accident.
	const std::vector<ImageContent> run = { window, ImageContent{} };
	const std::vector<std::byte> bytes = Publish(run, 24);

	GYRO_CHECK_EQ(Read<ImageContent>(bytes, 24, 0), window);
	GYRO_CHECK_EQ(Read<ImageContent>(bytes, 24, 1), ImageContent{});

	// Field by field as well as whole, because equality on a trivially-copyable aggregate is the
	// thing being trusted and a memberwise check is what catches a field that crossed as padding.
	const ImageContent read = Read<ImageContent>(bytes, 24, 0);
	GYRO_CHECK_EQ(read.Texture, window.Texture);
	GYRO_CHECK_EQ(read.Source.Extent.Width, 1280.0F);
	GYRO_CHECK_EQ(read.Frame.Origin.X, 32.0F);
	GYRO_CHECK(read.Color == ColorState::Srgb());
}

GYRO_TEST(Content, SolidSurvivesTheByteRoundTrip)
{
	// The letterbox fill: opaque, and in the composite's own state rather than in a client's.
	const SolidContent fill = { 0.05F, 0.05F, 0.06F, 1.0F, ColorState::Composite() };

	const std::vector<SolidContent> run = { fill, SolidContent{} };
	const std::vector<std::byte> bytes = Publish(run, 8);

	GYRO_CHECK_EQ(Read<SolidContent>(bytes, 8, 0), fill);
	GYRO_CHECK_EQ(Read<SolidContent>(bytes, 8, 1), SolidContent{});

	const SolidContent read = Read<SolidContent>(bytes, 8, 0);
	GYRO_CHECK_EQ(read.Alpha, 1.0F);
	GYRO_CHECK(read.Color == ColorState::Composite());
}

GYRO_TEST(Content, AnUnfinishedImageDrawsNothingRatherThanSomethingWrong)
{
	const ImageContent unfinished;

	// No pixels, so the renderer draws nothing and reports nothing — Core/Texture.h's rule, and the
	// only direction that does not put a lifetime bug in front of a person as a wrong picture.
	GYRO_CHECK(unfinished.Texture.IsNull());

	// Both rects empty, and empty is *whole* in both of them. A publisher that filled in a texture and
	// stopped gets the entire image sampled and the entire extent rounded, which is exactly what a
	// client that sets neither a viewport nor a window geometry already means.
	GYRO_CHECK(unfinished.Source.IsEmpty());
	GYRO_CHECK(unfinished.Frame.IsEmpty());

	GYRO_CHECK(unfinished.Color == ColorState::Srgb());
}

GYRO_TEST(Content, AnUnfinishedSolidIsVisible)
{
	const SolidContent unfinished;

	// Opaque black rather than transparent, on World/Node.h's terms for Opacity: a fill that should
	// not have been drawn is a bug somebody sees at once, and a transparent one is a bug somebody
	// bisects for. The value matches Seam/Renderer.h's DrawSolid so that a fill nobody set has one
	// meaning on both sides of the render seam.
	GYRO_CHECK_EQ(unfinished.Alpha, 1.0F);
	GYRO_CHECK_EQ(unfinished.Red, 0.0F);
	GYRO_CHECK_EQ(unfinished.Green, 0.0F);
	GYRO_CHECK_EQ(unfinished.Blue, 0.0F);
}

GYRO_TEST(Content, ColorStateIsPartOfTheRecordAndNotOfTheNode)
{
	// Two images alike in every geometric field and different in what their numbers mean as light are
	// different content, because they are different pixels. This is the assertion that would fail if
	// the state were ever hoisted onto the node "since it is the same for both".
	ImageContent tagged = Window();
	tagged.Color = ColorState::Composite();

	GYRO_CHECK(tagged != Window());
	GYRO_CHECK_EQ(tagged.Source, Window().Source);
	GYRO_CHECK_EQ(tagged.Frame, Window().Frame);
}
