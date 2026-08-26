#include "Drm/Scanout.h"

#include <fcntl.h>

#include <print>
#include <string>

#include "Core/Fd.h"
#include "Drm/Dumb.h"
#include "Seam/Importer.h"
#include "Testing/Test.h"

// Decision 153's importer: a client's dmabuf becoming a framebuffer a display engine can read.
//
// **Two halves with different gates**, which is Drm/Dumb.Test.cpp's split for its reason. What this
// table refuses — a null id, a mapped buffer, an id it never took — is decidable with no card, and
// those run everywhere. What it *builds* needs a card node, and the interesting property there is the
// one the class comment exists for: two ids over one buffer object must not close each other's GEM
// handle. CI has no card, so that half prints a named skip rather than passing quietly.

namespace
{
using namespace Drm;

[[nodiscard]] Fd OpenAnyCard()
{
	for (int minor = 0; minor < 4; ++minor)
	{
		const std::string path = "/dev/dri/card" + std::to_string(minor);

		if (Fd card{ ::open(path.c_str(), O_RDWR | O_CLOEXEC) }; card.IsValid())
		{
			return card;
		}
	}

	return {};
}

// A panel that never lets go, which is the condition `Sweep` has to respect: removing a framebuffer
// a display engine is reading disables the plane it is on rather than freeing memory.
class Scanning final : public IScanoutHold
{
public:
	[[nodiscard]] bool Holds(std::uint32_t) const noexcept override { return true; }
};

[[nodiscard]] TextureSource Describe(const DmabufBuffer& buffer)
{
	const RenderTarget target = buffer.Describe();

	return TextureSource{ .Size = { target.Size.Width, target.Size.Height },
		                  .Format = target.Format,
		                  .Memory = *target.AsDmabuf() };
}

constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
} // namespace

GYRO_TEST(Scanout, RefusesWhatHasNothingToScanOut)
{
	const Fd nothing;
	DrmScanout scanout{ nothing };

	// A `wl_shm` buffer's pixels were copied into gyro's own memory at commit and there is no
	// descriptor under them. Refused by name, so a surface is sampled and never promoted.
	std::byte pixels[16]{};
	const TextureSource mapped{ .Size = { 2, 2 },
		                        .Format = Linear,
		                        .Memory = MappedPixels{ pixels, 8, 0, sizeof(pixels) } };

	GYRO_CHECK(!scanout.Adopt(TextureId{ 0, 1 }, mapped));
	GYRO_CHECK(!scanout.Adopt(TextureId{}, mapped));
	GYRO_CHECK_EQ(scanout.Held(), 0U);
}

GYRO_TEST(Scanout, AnIdItNeverTookNamesNothing)
{
	const Fd nothing;
	DrmScanout scanout{ nothing };

	// Core/Texture.h's rule reaching the display side: an id that resolves to nothing is a partition
	// `DrmOutput::Expressible` refuses, rather than a plane programmed with somebody else's pixels.
	GYRO_CHECK_EQ(scanout.Find(TextureId{ 3, 1 }), 0U);

	scanout.Forget(TextureId{ 3, 1 });

	GYRO_CHECK_EQ(scanout.Held(), 0U);
	GYRO_CHECK_EQ(scanout.Retiring(), 0U);
}

GYRO_TEST(Scanout, TwoIdsOverOneBufferKeepTheirOwnFramebuffers)
{
	const Fd card = OpenAnyCard();

	if (!card.IsValid())
	{
		std::println("skipped: no /dev/dri/card node");

		return;
	}

	DumbAllocator allocator{ card.Borrow() };
	Result<DmabufBuffer> buffer = allocator.Allocate({ 64, 64 }, Linear.Code, { &Linear.Modifier, 1 });

	if (!buffer)
	{
		std::println("skipped: the card would not allocate a dumb buffer ({})", buffer.error().Context());

		return;
	}

	DrmScanout scanout{ card };

	const TextureId first{ 0, 1 };
	const TextureId second{ 1, 1 };
	const TextureSource source = Describe(*buffer);

	GYRO_REQUIRE(scanout.Adopt(first, source).has_value());
	GYRO_REQUIRE(scanout.Adopt(second, source).has_value());
	GYRO_CHECK_EQ(scanout.Held(), 2U);

	const std::uint32_t framebuffer = scanout.Find(second);

	GYRO_CHECK(framebuffer != 0);

	// The bug the refcount exists to prevent: `drmPrimeFDToHandle` hands back the *same* GEM handle
	// for two imports of one dmabuf, so a table that closed its handle here would invalidate the other
	// entry — a window going black when an unrelated one closes.
	scanout.Forget(first);

	GYRO_CHECK_EQ(scanout.Held(), 1U);
	GYRO_CHECK_EQ(scanout.Find(second), framebuffer);

	// And the handle is still the card's: a framebuffer over a closed one cannot be built again.
	GYRO_CHECK(scanout.Adopt(first, source).has_value());
	GYRO_CHECK_EQ(scanout.Held(), 2U);
}

GYRO_TEST(Scanout, AFramebufferAPanelIsReadingIsNotRemoved)
{
	const Fd card = OpenAnyCard();

	if (!card.IsValid())
	{
		std::println("skipped: no /dev/dri/card node");

		return;
	}

	DumbAllocator allocator{ card.Borrow() };
	Result<DmabufBuffer> buffer = allocator.Allocate({ 64, 64 }, Linear.Code, { &Linear.Modifier, 1 });

	if (!buffer)
	{
		std::println("skipped: the card would not allocate a dumb buffer ({})", buffer.error().Context());

		return;
	}

	DrmScanout scanout{ card };
	Scanning panel;

	scanout.Attach(panel);

	GYRO_REQUIRE(scanout.Adopt(TextureId{ 0, 1 }, Describe(*buffer)).has_value());

	// The id is dead from the call onward and the framebuffer is not: the panel is still reading it,
	// and `drm_mode_rmfb` here would disable the plane rather than free anything.
	scanout.Forget(TextureId{ 0, 1 });

	GYRO_CHECK_EQ(scanout.Held(), 0U);
	GYRO_CHECK_EQ(scanout.Find(TextureId{ 0, 1 }), 0U);
	GYRO_CHECK_EQ(scanout.Retiring(), 1U);

	// An output being torn down is the moment it stops answering, and what it was the last reader of
	// is free that instant.
	scanout.Detach(panel);

	GYRO_CHECK_EQ(scanout.Retiring(), 0U);
}
