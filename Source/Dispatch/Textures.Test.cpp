#include "Dispatch/Textures.h"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Scene/Textures.h"
#include "Seam/Allocator.h"
#include "Seam/Importer.h"
#include "Seam/Scanout.h"
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
// The display engine's half of decision 153, standing in for `Drm/Scanout.h` — which belongs to a
// module this may not name, and which is the whole reason the offer is made through an interface.
class FakeScanout final : public IScanoutImporter
{
public:
	[[nodiscard]] Result<void> Adopt(TextureId id, const TextureSource& source) override
	{
		Offered.push_back(id);

		// What a real one refuses: `wl_shm` pixels are copied into gyro's own memory at commit and there
		// is nothing there a panel could scan out of.
		if (source.IsMapped() || m_Refusing)
		{
			return Failure(EINVAL, "not a descriptor this display engine can take");
		}

		Held.push_back(id);

		return {};
	}

	void Forget(TextureId id) noexcept override { Forgotten.push_back(id); }

	void Refuse(bool refusing) noexcept { m_Refusing = refusing; }

	std::vector<TextureId> Offered;
	std::vector<TextureId> Held;
	std::vector<TextureId> Forgotten;

private:
	bool m_Refusing = false;
};

// A client's buffer, and the party that has to be told when nobody is reading it.
class CountingRelease final : public ITextureRelease
{
public:
	void OnTextureReleased() noexcept override { ++Released; }

	std::size_t Released = 0;
};

[[nodiscard]] Fd MakeDescriptor(std::size_t size)
{
	const int descriptor = ::memfd_create("gyro-texture-test", MFD_CLOEXEC);

	if (descriptor < 0)
	{
		return {};
	}

	Fd fd{ descriptor };

	return ::ftruncate(descriptor, static_cast<off_t>(size)) == 0 ? std::move(fd) : Fd{};
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

// Descriptors go in, the same layout comes out the far side, and the caller's own descriptors are its
// own again. The last of those is the one a test is for: the registry duplicates, so a caller that
// closes its fd the instant `Adopt` returns must not have taken the image away from the renderer.
GYRO_TEST(Textures, ADescriptorIsDuplicatedRatherThanTakenAndTheLayoutSurvives)
{
	FakeImporter importer;
	std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry registry{ importers };

	Fd fd = MakeDescriptor(64U * 32U * 4U);

	GYRO_REQUIRE(fd.IsValid());

	const std::array<TexturePlane, 1> planes{ TexturePlane{
		.Descriptor = fd.Borrow(), .Offset = 0, .Stride = 64U * 4U } };

	const Result<TextureId> adopted = registry.Adopt(
		PixelSize<BufferSpace>{ 64, 32 }, TextureFormat{ .Code = FormatArgb8888, .Modifier = 0 }, planes, nullptr
	);

	GYRO_REQUIRE(adopted.has_value());
	GYRO_CHECK(importer.Holds(*adopted));

	// The caller's descriptor, closed. What the renderer holds was duplicated out of it.
	fd = Fd{};

	GYRO_CHECK_EQ(registry.Live(), std::uint32_t{ 1 });
}

// The scanout importer is offered every descriptor and answers for none of them, which is the
// difference between a window that is composited and a window that is not on screen at all.
GYRO_TEST(Textures, AScanoutRefusalLeavesTheImageDrawableEverywhereElse)
{
	FakeImporter importer;
	FakeScanout scanout;
	std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry registry{ importers, {}, &scanout };

	scanout.Refuse(true);

	const Fd fd = MakeDescriptor(64U * 32U * 4U);

	GYRO_REQUIRE(fd.IsValid());

	const std::array<TexturePlane, 1> planes{ TexturePlane{
		.Descriptor = fd.Borrow(), .Offset = 0, .Stride = 64U * 4U } };

	const Result<TextureId> adopted = registry.Adopt(
		PixelSize<BufferSpace>{ 64, 32 }, TextureFormat{ .Code = FormatArgb8888, .Modifier = 0 }, planes, nullptr
	);

	GYRO_REQUIRE(adopted.has_value());
	GYRO_CHECK_EQ(scanout.Offered.size(), std::size_t{ 1 });
	GYRO_CHECK(scanout.Held.empty());
	GYRO_CHECK(importer.Holds(*adopted));
}

// A mapped buffer is never offered *where there is nothing to put it in*: gyro's own heap holds
// nothing a display engine could scan out of, and offering one would be an ioctl per `wl_shm` commit
// for an answer that is always no. With an allocator behind it the bytes stop being mapped before they
// get here, which is the test below.
GYRO_TEST(Textures, MappedPixelsAreNeverOfferedToTheDisplayEngine)
{
	FakeImporter importer;
	FakeScanout scanout;
	std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry registry{ importers, {}, &scanout };

	const std::array<std::byte, 4U * 4U * 4U> pixels{};

	GYRO_REQUIRE(registry.Adopt(PixelSize<BufferSpace>{ 4, 4 }, 16, pixels, TextureAlpha::Premultiplied).has_value());
	GYRO_CHECK(scanout.Offered.empty());
}

// The release is owed to the watermark and to nothing else, and the display engine gives the id up in
// the same step — which is what keeps a framebuffer from outliving the buffer behind it.
GYRO_TEST(Textures, AReclaimForgetsTheFramebufferAndTellsWhoeverIsOwedARelease)
{
	FakeImporter importer;
	FakeScanout scanout;
	CountingRelease release;
	std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry registry{ importers, {}, &scanout };

	const Fd fd = MakeDescriptor(64U * 32U * 4U);

	GYRO_REQUIRE(fd.IsValid());

	const std::array<TexturePlane, 1> planes{ TexturePlane{
		.Descriptor = fd.Borrow(), .Offset = 0, .Stride = 64U * 4U } };

	const Result<TextureId> adopted = registry.Adopt(
		PixelSize<BufferSpace>{ 64, 32 }, TextureFormat{ .Code = FormatArgb8888, .Modifier = 0 }, planes, &release
	);

	GYRO_REQUIRE(adopted.has_value());
	GYRO_CHECK_EQ(scanout.Held.size(), std::size_t{ 1 });

	registry.Retire(*adopted);
	registry.Seal(7);

	// Below the stamp: the frame thread may still be recording from a snapshot that names it.
	registry.Reclaim(6);

	GYRO_CHECK_EQ(release.Released, std::size_t{ 0 });
	GYRO_CHECK(scanout.Forgotten.empty());

	registry.Reclaim(7);

	GYRO_CHECK_EQ(release.Released, std::size_t{ 1 });
	GYRO_CHECK_EQ(scanout.Forgotten.size(), std::size_t{ 1 });
	GYRO_CHECK(!importer.Holds(*adopted));
}

// A client that destroyed its `wl_buffer` while a frame was still drawn from it. The id lives on and
// retires normally; what stops is the callback into an object that is gone.
GYRO_TEST(Textures, AbandonDropsTheNotificationAndLeavesTheIdAlone)
{
	FakeImporter importer;
	std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry registry{ importers };

	const Fd fd = MakeDescriptor(64U * 32U * 4U);

	GYRO_REQUIRE(fd.IsValid());

	const std::array<TexturePlane, 1> planes{ TexturePlane{
		.Descriptor = fd.Borrow(), .Offset = 0, .Stride = 64U * 4U } };

	{
		CountingRelease release;

		const Result<TextureId> adopted = registry.Adopt(
			PixelSize<BufferSpace>{ 64, 32 }, TextureFormat{ .Code = FormatArgb8888, .Modifier = 0 }, planes, &release
		);

		GYRO_REQUIRE(adopted.has_value());

		registry.Retire(*adopted);
		registry.Abandon(release);
	}

	registry.Seal(3);
	registry.Reclaim(3);

	GYRO_CHECK_EQ(registry.Live(), std::uint32_t{ 0 });
}

namespace
{
// A provider that maps what it allocates, standing in for the DRM dumb buffer — the one allocation on
// a card whose purpose is software drawing that hardware scans out.
//
// The pitch is deliberately wider than the image. A real display engine fetches in a width of its own
// choosing and a dumb buffer comes back with the pitch the kernel picked, so an authored image copied
// row by row and one copied as a block differ on every machine but the developer's.
class FakeAllocator final : public IDmabufAllocator
{
public:
	[[nodiscard]] Result<DmabufBuffer>
	Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) override
	{
		if (!FirstSupported(*this, code, modifiers).IsValid())
		{
			return Failure(EINVAL, "this provider does linear and nothing else");
		}

		Pitch = static_cast<std::uint32_t>(size.Width) * 4U + Padding;

		const std::size_t length = static_cast<std::size_t>(Pitch) * static_cast<std::size_t>(size.Height);

		Fd descriptor = MakeDescriptor(length);

		if (!descriptor.IsValid())
		{
			return Failure(ENOMEM, "no memory for a fake allocation");
		}

		void* const pixels = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor.Borrow().Value, 0);

		if (pixels == MAP_FAILED)
		{
			return Failure(ENOMEM, "a fake allocation that could not be mapped");
		}

		++Allocations;

		// A second handle on the same pages, so a test can read back what was written into them after the
		// registry has finished with the buffer and let it go.
		Witness = Duplicate(descriptor.Borrow());
		Length = length;

		return DmabufBuffer{ std::move(descriptor),
			                 size,
			                 PixelFormat{ .Code = code, .Modifier = ModifierLinear },
			                 Pitch,
			                 Mapping{ static_cast<std::byte*>(pixels), length } };
	}

	[[nodiscard]] bool Supports(PixelFormat format) const noexcept override
	{
		return format.Modifier == ModifierLinear;
	}

	[[nodiscard]] std::string_view Name() const noexcept override { return "fake"; }

	// What the allocation holds, read back through `Witness`. Empty where nothing was allocated.
	[[nodiscard]] std::span<const std::byte> Wrote() const noexcept
	{
		if (!Witness.IsValid())
		{
			return {};
		}

		void* const pixels = ::mmap(nullptr, Length, PROT_READ, MAP_SHARED, Witness.Borrow().Value, 0);

		return pixels == MAP_FAILED ? std::span<const std::byte>{} :
		                              std::span<const std::byte>{ static_cast<const std::byte*>(pixels), Length };
	}

	std::uint32_t Padding = 64;
	std::uint32_t Pitch = 0;
	std::size_t Allocations = 0;
	Fd Witness;
	std::size_t Length = 0;
};
} // namespace

// **The whole of the cursor's problem, and it is not the cursor's.** Everything gyro draws for itself
// arrived as heap memory and so could never be scanned out, which on a screen with a pointer over a
// window took the *window* off its plane too — the promoted set is a suffix, so an unscannable top
// item refuses everything under it. An author still hands over bytes and says what the top one means;
// where the machine has somewhere to put them, they land in an allocation a display engine can read.
GYRO_TEST(Textures, AnAuthoredImageGoesWhereADisplayEngineCanReadIt)
{
	FakeImporter importer;
	FakeScanout scanout;
	FakeAllocator allocator;
	std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry registry{ importers, {}, &scanout, &allocator };

	std::array<std::byte, 4U * 4U * 4U> pixels{};

	pixels[0] = std::byte{ 0xAB };
	// The first texel of the second row, which is where a copy that ignored the allocator's pitch would
	// put the byte above instead.
	pixels[16] = std::byte{ 0xCD };

	const Result<TextureId> adopted =
		registry.Adopt(PixelSize<BufferSpace>{ 4, 4 }, 16, pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(adopted.has_value());
	GYRO_CHECK_EQ(allocator.Allocations, std::size_t{ 1 });

	// Offered and taken, which is the point: a mapped source is the one thing a scanout importer always
	// refuses, and this one is a descriptor by the time it is offered.
	GYRO_REQUIRE_EQ(scanout.Offered.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(scanout.Held.size(), std::size_t{ 1 });
	GYRO_CHECK(importer.Holds(*adopted));

	// **Row by row against the allocator's own pitch**, which is the half that fails quietly: a copy that
	// took the author's stride for the destination's writes every row further left than the last, and
	// what that draws is a pointer sheared diagonally across the screen.
	const std::span<const std::byte> wrote = allocator.Wrote();

	GYRO_REQUIRE(!wrote.empty());
	GYRO_CHECK_EQ(wrote[0], std::byte{ 0xAB });
	GYRO_CHECK_EQ(wrote[allocator.Pitch], std::byte{ 0xCD });
}

// **A refusal is ordinary and the image is composited, which is where it already was.** A provider that
// allocates something the processor cannot write into is the failure worth naming separately: the
// allocation succeeds, so a caller that trusted it would write a pointer glyph into nothing and put a
// transparent rectangle on the screen where the pointer is.
GYRO_TEST(Textures, AnImageThatCannotBeMappedStaysInGyrosOwnMemory)
{
	class Unmappable final : public IDmabufAllocator
	{
	public:
		[[nodiscard]] Result<DmabufBuffer>
		Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t>) override
		{
			Fd descriptor =
				MakeDescriptor(static_cast<std::size_t>(size.Width) * static_cast<std::size_t>(size.Height) * 4U);

			return DmabufBuffer{ std::move(descriptor),
				                 size,
				                 PixelFormat{ .Code = code, .Modifier = ModifierLinear },
				                 static_cast<std::uint32_t>(size.Width) * 4U,
				                 Mapping{} };
		}

		[[nodiscard]] bool Supports(PixelFormat) const noexcept override { return true; }
		[[nodiscard]] std::string_view Name() const noexcept override { return "unmappable"; }
	};

	FakeImporter importer;
	FakeScanout scanout;
	Unmappable allocator;
	std::array<ITextureImporter*, 1> importers{ &importer };
	TextureRegistry registry{ importers, {}, &scanout, &allocator };

	const std::array<std::byte, 4U * 4U * 4U> pixels{};
	const Result<TextureId> adopted =
		registry.Adopt(PixelSize<BufferSpace>{ 4, 4 }, 16, pixels, TextureAlpha::Premultiplied);

	GYRO_REQUIRE(adopted.has_value());
	GYRO_CHECK(scanout.Offered.empty());
	GYRO_CHECK(importer.Holds(*adopted));
}
