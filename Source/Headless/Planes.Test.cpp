#include "Headless/Planes.h"

#include <array>
#include <cerrno>
#include <cstdint>

#include "Geometry/Space.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"

// What the catalog is for is stated in its header and is worth restating as the thing these cases
// check: the descriptor filters and the test decides. So the interesting assertions are the two
// failure vocabularies staying apart — a set that cannot be expressed is EINVAL and a controller that
// declined a legal one is EBUSY — because an assigner is written to retry the second and to treat the
// first as its own bug.

namespace
{
constexpr PixelFormat Xrgb{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelFormat Argb{ FormatArgb8888, 0, ModifierLinear };

RenderTarget Target(PixelSize<DeviceSpace> size, PixelFormat format = Xrgb)
{
	// The pointer and length are what RenderTarget::IsValid asks for; nothing here reads the pixels.
	static std::array<std::byte, 16> storage{};

	return RenderTarget{ .Size = size,
		                 .Format = format,
		                 .Memory = MappedImage{ .Pixels = storage.data(), .Stride = 4, .Length = storage.size() } };
}

PresentLayer Layer(std::uint32_t target, Rect<DeviceSpace> source, PixelRect<DeviceSpace> destination)
{
	return PresentLayer{
		.Target = target, .Acquire = SyncPoint::Immediate(), .Source = source, .Destination = destination, .Damage = {}
	};
}
} // namespace

GYRO_TEST(Planes, TheOrdinaryCatalogTakesAFullScreenComposite)
{
	PlaneCatalog catalog = PlaneCatalog::Typical({ 2560, 1440 });
	const std::array<RenderTarget, 1> targets{ Target({ 2560, 1440 }) };
	const std::array<PresentLayer, 1> layers{
		Layer(0, { {}, { 2560.0F, 1440.0F } }, { {}, { 2560, 1440 } }),
	};

	GYRO_CHECK(catalog.Test(layers, targets).has_value());
	GYRO_CHECK_EQ(catalog.Planes().size(), std::size_t{ 4 });
}

// An empty source means the whole image, per Seam/Presenter.h, so the extent to check is the target's.
GYRO_TEST(Planes, AnEmptySourceIsTheWholeImage)
{
	PlaneCatalog catalog = PlaneCatalog::Typical({ 800, 600 });
	const std::array<RenderTarget, 1> targets{ Target({ 800, 600 }) };
	const std::array<PresentLayer, 1> layers{ Layer(0, {}, { {}, { 800, 600 } }) };

	GYRO_CHECK(catalog.Test(layers, targets).has_value());
}

// The primary does not scale, which is the constraint that makes a promotion decision interesting at
// all — the sc7180 arrangement Docs/Architecture.md quotes.
GYRO_TEST(Planes, APlaneWithoutAScalerRefusesAResample)
{
	PlaneCatalog catalog = PlaneCatalog::Typical({ 800, 600 });
	const std::array<RenderTarget, 1> targets{ Target({ 800, 600 }) };
	const std::array<PresentLayer, 1> layers{ Layer(0, { {}, { 800.0F, 600.0F } }, { {}, { 400, 300 } }) };

	const Result<void> verdict = catalog.Test(layers, targets);

	GYRO_REQUIRE(!verdict.has_value());
	GYRO_CHECK_EQ(verdict.error().Code(), EINVAL);
}

// The overlay does, inside its ratio bounds, and refuses outside them.
GYRO_TEST(Planes, AScalingPlaneHonoursItsRatioBounds)
{
	PlaneCatalog catalog = PlaneCatalog::Typical({ 800, 600 });
	const std::array<RenderTarget, 2> targets{ Target({ 800, 600 }), Target({ 400, 400 }, Argb) };

	const std::array<PresentLayer, 2> inside{
		Layer(0, { {}, { 800.0F, 600.0F } }, { {}, { 800, 600 } }),
		Layer(1, { {}, { 400.0F, 400.0F } }, { {}, { 800, 800 } }),
	};

	GYRO_CHECK(catalog.Test(inside, targets).has_value());

	// Twelve times up is past the overlay's three, so the descriptor eliminates it without a test.
	const std::array<PresentLayer, 2> outside{
		Layer(0, { {}, { 800.0F, 600.0F } }, { {}, { 800, 600 } }),
		Layer(1, { {}, { 400.0F, 400.0F } }, { {}, { 4800, 4800 } }),
	};

	GYRO_CHECK(!catalog.Test(outside, targets).has_value());
}

// A format the plane does not advertise is eliminated cheaply, which is the descriptor's whole job.
GYRO_TEST(Planes, APlaneRefusesAFormatItDoesNotAdvertise)
{
	PlaneCatalog catalog = PlaneCatalog::Typical({ 800, 600 });
	const std::array<RenderTarget, 1> targets{ Target({ 800, 600 }, Argb) };
	const std::array<PresentLayer, 1> layers{ Layer(0, {}, { {}, { 800, 600 } }) };

	// The primary in the typical catalog is XR24 only, and the composite arrives as AR24.
	GYRO_CHECK(!catalog.Test(layers, targets).has_value());
}

// More layers than planes is a bug in whatever produced the list rather than a condition to retry.
GYRO_TEST(Planes, MoreLayersThanPlanesIsNotExpressible)
{
	PlaneCatalog catalog = PlaneCatalog::Typical({ 800, 600 }, 0);
	const std::array<RenderTarget, 1> targets{ Target({ 800, 600 }) };
	const std::array<PresentLayer, 2> layers{
		Layer(0, {}, { {}, { 800, 600 } }),
		Layer(0, {}, { {}, { 800, 600 } }),
	};

	const Result<void> verdict = catalog.Test(layers, targets);

	GYRO_REQUIRE(!verdict.has_value());
	GYRO_CHECK_EQ(verdict.error().Code(), EINVAL);
}

// The case the file exists for. The layers were legal, the controller declined, and the frame's budget
// was already planned around a promotion that is not going to happen.
GYRO_TEST(Planes, AScriptedRefusalIsTransientAndNotAnExpressibilityFailure)
{
	PlaneCatalog catalog = PlaneCatalog::Typical({ 800, 600 });
	const std::array<RenderTarget, 1> targets{ Target({ 800, 600 }) };
	const std::array<PresentLayer, 1> layers{ Layer(0, {}, { {}, { 800, 600 } }) };

	catalog.RefuseNext(2);

	const Result<void> first = catalog.Test(layers, targets);
	GYRO_REQUIRE(!first.has_value());
	GYRO_CHECK_EQ(first.error().Code(), EBUSY);

	GYRO_CHECK(!catalog.Test(layers, targets).has_value());

	// And then it stops, because a scripted refusal is a transient by construction.
	GYRO_CHECK(catalog.Test(layers, targets).has_value());
}

// An empty list is not a cheap frame; it is a commit that says nothing, which no controller takes.
GYRO_TEST(Planes, AnEmptyLayerListIsRefused)
{
	PlaneCatalog catalog = PlaneCatalog::Typical({ 800, 600 });
	const std::array<RenderTarget, 1> targets{ Target({ 800, 600 }) };

	GYRO_CHECK(!catalog.Test({}, targets).has_value());
}
