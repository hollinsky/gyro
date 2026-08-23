#include "Seam/Importer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

#include "Core/Fd.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The claim this file makes is the one Seam/RenderTarget.h makes at the other end of the composite,
// and it is worth making twice because the two ends differ in who chooses the kind. A target's memory
// kind is the backend's and is constant for a binding; a *source's* is the individual client
// buffer's, so both implementations will be handed both and neither may guess. What is tested is
// therefore the same boundary — ask which kind, get a null pointer rather than a plausible-looking
// reinterpretation of the other one — plus the validity rule, which exists so that a source built in
// pieces by a demarshaller is caught where it is imported rather than where it eventually samples
// nothing.

namespace
{
alignas(std::max_align_t) std::array<std::byte, 4096> Pool{};

[[nodiscard]] TextureSource Shm()
{
	return { { 32, 32 }, { FormatArgb8888, 0, ModifierLinear }, MappedPixels{ Pool.data(), 128, 0, Pool.size() } };
}

[[nodiscard]] TextureSource Dmabuf()
{
	DmabufImage image;
	image.Planes[0] = { RawFd{ 9 }, 0, 7680 };
	image.PlaneCount = 1;

	return { { 1920, 1080 }, { FormatXrgb8888, 0, ModifierLinear }, image };
}
} // namespace

GYRO_TEST(TextureSource, AClientMappingAnswersOnlyAsAMapping)
{
	const TextureSource source = Shm();

	GYRO_REQUIRE(source.AsMapped() != nullptr);
	GYRO_CHECK(source.AsDmabuf() == nullptr);
	GYRO_CHECK(source.IsMapped());
	GYRO_CHECK_EQ(source.AsMapped()->Bytes().size(), Pool.size());
	GYRO_CHECK(source.IsValid());
}

GYRO_TEST(TextureSource, AClientDmabufAnswersOnlyAsADmabuf)
{
	const TextureSource source = Dmabuf();

	GYRO_REQUIRE(source.AsDmabuf() != nullptr);
	GYRO_CHECK(source.AsMapped() == nullptr);
	GYRO_CHECK(!source.IsMapped());
	GYRO_CHECK_EQ(source.AsDmabuf()->Used().size(), std::size_t{ 1 });
	GYRO_CHECK(source.IsValid());
}

// Every field a demarshaller fills in separately, each one left out in turn. A commit that arrives
// before its buffer, a format nobody negotiated, and a plane count of zero are all things a client can
// produce, and none of them may reach an importer as an image that merely samples black.
GYRO_TEST(TextureSource, AHalfBuiltSourceIsNotImportable)
{
	TextureSource noExtent = Shm();
	noExtent.Size = {};
	GYRO_CHECK(!noExtent.IsValid());

	TextureSource noFormat = Shm();
	noFormat.Format = {};
	GYRO_CHECK(!noFormat.IsValid());

	TextureSource noPixels = Shm();
	noPixels.Memory = MappedPixels{};
	GYRO_CHECK(!noPixels.IsValid());

	TextureSource noPlanes = Dmabuf();
	noPlanes.Memory = DmabufImage{};
	GYRO_CHECK(!noPlanes.IsValid());

	// A plane count past the array is the one that would read off the end of it rather than merely
	// import nothing, which is why it is refused here and not left to whichever importer indexes first.
	TextureSource tooManyPlanes = Dmabuf();
	std::get<DmabufImage>(tooManyPlanes.Memory).PlaneCount = static_cast<std::uint32_t>(MaxImagePlanes) + 1;
	GYRO_CHECK(!tooManyPlanes.IsValid());
}

// The identity is the caller's and the importer is only told it — Seam/Importer.h's rule that a
// device migration must not renumber anything. Nothing here can assert that across a real migration,
// so what it holds is the property the rule rests on: the id is an ordinary generational handle whose
// equality is the whole handle, so a slot reused under a new generation is a different texture.
GYRO_TEST(TextureSource, AnIdIsTheWholeHandle)
{
	constexpr TextureId first{ 4, 1 };
	constexpr TextureId reused{ 4, 2 };

	GYRO_CHECK(!(first == reused));
	GYRO_CHECK(!first.IsNull());
	GYRO_CHECK(TextureId{}.IsNull());
}
