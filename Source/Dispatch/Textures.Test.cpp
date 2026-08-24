#include "Dispatch/Textures.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <vector>

#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Seam/Importer.h"
#include "Testing/Test.h"

// The minter, checked for the two properties nothing else can check for it: that a texture is on every
// renderer or on none, and that it is forgotten strictly after the frame thread has moved past every
// snapshot that could name it.
//
// The watermark half is the one worth the machinery. It is a rule about two threads that this object
// implements alone — the frame side never calls anything here — so a test is the only place the
// ordering is observable at all, and under a sanitiser the alternative is a use-after-free in whichever
// window happened to be on screen.

namespace
{
// An importer that remembers, and refuses on request. Standing in for `Blit`, which is the real one and
// belongs to a module this may not name.
class FakeImporter final : public ITextureImporter
{
public:
	[[nodiscard]] Result<void> Adopt(TextureId id, const TextureSource& source) override
	{
		if (m_Refusing)
		{
			return Failure(ENOMEM, "this importer is refusing");
		}

		if (id.IsNull() || !source.IsValid())
		{
			return Failure(EINVAL, "an importer takes an id and a described source");
		}

		Held.push_back(id);

		return {};
	}

	void Forget(TextureId id) noexcept override
	{
		Forgotten.push_back(id);

		for (std::size_t index = 0; index < Held.size(); ++index)
		{
			if (Held[index] == id)
			{
				Held.erase(Held.begin() + static_cast<std::ptrdiff_t>(index));

				return;
			}
		}
	}

	void Refuse(bool refusing) noexcept { m_Refusing = refusing; }

	[[nodiscard]] bool Holds(TextureId id) const noexcept
	{
		for (const TextureId held : Held)
		{
			if (held == id)
			{
				return true;
			}
		}

		return false;
	}

	std::vector<TextureId> Held;
	std::vector<TextureId> Forgotten;

private:
	bool m_Refusing = false;
};

constexpr std::int32_t Width = 4;
constexpr std::int32_t Height = 3;
constexpr std::uint32_t Stride = Width * 4;

const std::array<std::byte, Stride * Height> Pixels{};

[[nodiscard]] PixelSize<BufferSpace> Extent() noexcept
{
	return { Width, Height };
}
} // namespace

// Every renderer or none. A texture that exists on one and not another is a window that is on one panel
// and missing from the other, which the caller cannot see and cannot act on.
GYRO_TEST(DispatchTextures, AnAdoptReachesEveryRendererAndARefusalUndoesTheOnesThatTookIt)
{
	FakeImporter first;
	FakeImporter second;

	const std::array<ITextureImporter*, 2> importers{ &first, &second };
	TextureRegistry textures{ importers };

	const Result<TextureId> both = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(both);
	GYRO_CHECK(first.Holds(*both) && second.Holds(*both));

	second.Refuse(true);

	const Result<TextureId> refused = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(!refused);
	GYRO_CHECK_EQ(refused.error().Code(), ENOMEM);

	// The one that took it was told to let go, and the id space did not lose a slot to a texture that
	// does not exist.
	GYRO_CHECK_EQ(first.Held.size(), 1U);
	GYRO_CHECK_EQ(first.Forgotten.size(), 1U);
	GYRO_CHECK_EQ(textures.Live(), 1U);
}

GYRO_TEST(DispatchTextures, PixelsThatDoNotDescribeTheImageAreRefusedBeforeAnyImporterSeesThem)
{
	FakeImporter importer;
	const std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry textures{ importers };

	GYRO_CHECK_EQ(textures.Adopt({ 0, 0 }, Stride, Pixels, TextureAlpha::Premultiplied).error().Code(), EINVAL);
	GYRO_CHECK_EQ(textures.Adopt(Extent(), Stride - 4, Pixels, TextureAlpha::Premultiplied).error().Code(), EINVAL);
	GYRO_CHECK_EQ(
		textures.Adopt({ Width, Height + 1 }, Stride, Pixels, TextureAlpha::Premultiplied).error().Code(), EINVAL
	);
	GYRO_CHECK(importer.Held.empty());
}

// A run with no renderer that can sample is a run where a scene of images would draw nothing and say
// nothing. Refusing at the adopt is what turns that into a compositor that will not start.
GYRO_TEST(DispatchTextures, WithNoRendererThereIsNothingToImportInto)
{
	TextureRegistry textures{ {} };

	const Result<TextureId> none = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(!none);
	GYRO_CHECK_EQ(none.error().Code(), ENODEV);
}

// The whole point of the object. A retired texture is still adopted, still named by whatever snapshots
// are in flight, and is let go on the step the watermark says the frame thread is past them.
GYRO_TEST(DispatchTextures, ARetiredTextureIsForgottenOnlyOnceTheWatermarkHasPassedTheSealedSequence)
{
	FakeImporter importer;
	const std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry textures{ importers };

	const Result<TextureId> id = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(id);

	textures.Retire(*id);

	// Retired and not yet sealed: the author has given it up, and no sequence has been published that
	// omits it, so there is nothing yet that could make it safe.
	GYRO_CHECK_EQ(textures.Retiring(), 1U);
	GYRO_CHECK(importer.Holds(*id));

	// Sequence 7 is the first snapshot that does not name it, so 6 is the last that does.
	textures.Seal(7);

	textures.Reclaim(6);
	GYRO_CHECK(importer.Holds(*id));

	// A watermark *equal* to the seal is the frame thread rendering from the snapshot without it, which
	// is the first instant nothing in flight can be sampling those pixels.
	textures.Reclaim(7);
	GYRO_CHECK(!importer.Holds(*id));
	GYRO_CHECK_EQ(textures.Live(), 0U);
	GYRO_CHECK_EQ(textures.Retiring(), 0U);
}

// A texture nobody retired is not swept, however far the watermark runs. The rule is about safety and
// never about age.
GYRO_TEST(DispatchTextures, ALiveTextureSurvivesEveryWatermark)
{
	FakeImporter importer;
	const std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry textures{ importers };

	const Result<TextureId> id = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(id);

	textures.Seal(1);
	textures.Reclaim(1000);

	GYRO_CHECK(importer.Holds(*id));
	GYRO_CHECK_EQ(textures.Live(), 1U);
}

// A client's swap, as the registry sees it: two ids alive at once for exactly as long as it takes the
// frame thread to move on, which is what makes the old buffer still drawable while the new one imports.
GYRO_TEST(DispatchTextures, TheOldAndTheNewAreBothAdoptedAcrossASwap)
{
	FakeImporter importer;
	const std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry textures{ importers };

	const Result<TextureId> first = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(first);

	const Result<TextureId> second = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(second);
	GYRO_CHECK(*first != *second);

	textures.Retire(*first);
	textures.Seal(2);

	GYRO_CHECK(importer.Holds(*first) && importer.Holds(*second));

	textures.Reclaim(2);

	GYRO_CHECK(!importer.Holds(*first));
	GYRO_CHECK(importer.Holds(*second));
}

// Decision 41's migration, which is the reason the import verb is at the waist at all: the renderers are
// destroyed and rebuilt, and every window's pixels have to be there afterwards under the same ids.
GYRO_TEST(DispatchTextures, ARebuiltRendererIsHandedEveryLiveImageAgainUnderTheSameId)
{
	FakeImporter before;
	const std::array<ITextureImporter*, 1> first{ &before };
	TextureRegistry textures{ first };

	const Result<TextureId> kept = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);
	const Result<TextureId> dropped = textures.Adopt(Extent(), Stride, Pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(kept && dropped);

	textures.Retire(*dropped);
	textures.Seal(3);
	textures.Reclaim(3);

	FakeImporter after;
	const std::array<ITextureImporter*, 1> second{ &after };

	GYRO_REQUIRE(textures.Rebind(second).has_value());

	// The same id, because Core/Texture.h's space is dispatch's and survives the device — a renaming
	// here would be every node in the scene pointing at nothing at exactly the handoff decision 41
	// requires to be invisible.
	GYRO_CHECK(after.Holds(*kept));
	GYRO_CHECK_EQ(after.Held.size(), 1U);
}
