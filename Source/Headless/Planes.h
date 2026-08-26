#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"

// The synthetic plane catalog: what this simulated controller can put on a plane, and what it refuses.
//
// **It exists before the assigner does, on purpose.** Docs/Architecture.md#what-to-build-before-it-is-
// needed puts it on the list of things that are cheap now and expensive to retrofit, and gives the
// reason: without it "the multi-layer path is code that has never run", which widens
// [decision 5](../../Docs/Decisions.md)'s exposure — a DRM backend designed from specification with no
// hardware spike — rather than paying it down. So the catalog is written now and the party that
// consumes a refusal is written later.
//
// **A capability descriptor filters; the test decides.** That is the same section's rule and the whole
// shape of this file. Real controllers constrain on per-CRTC bandwidth, core clock, line width, scaler
// ratios that vary with format, and pipes that serve two rectangles when both are small enough — none
// of which is a per-plane flag. A descriptor rich enough to be an oracle is a descriptor that lies, so
// the one below eliminates the obviously impossible cheaply and `Test` is the only authority.
//
// **Which makes the scripted refusal the point of the file rather than a testing convenience.** On
// hardware, `TEST_ONLY` can reject a layer set the descriptor admitted, and it does so *after* the
// frame's budget was planned against a promotion that is not going to happen. An assigner written
// against a catalog that never surprises it is an assigner that has never handled the case it exists
// to handle, and that case first runs on a machine with a monitor. `RefuseNext` is how it runs here.

// Sized rather than measured, and the sizing is a real controller's: Docs/Architecture.md#direct-
// scanout-is-conditional quotes an sc7180 tablet at one scaling pipe against three flat ones, so a
// catalog past eight is past anything gyro has been pointed at.
inline constexpr std::size_t MaxPlanes = 8;

// The formats one plane advertises. Four is what the floor tier and the console name between them, and
// a catalog that needs more is describing a device this one is not.
inline constexpr std::size_t MaxPlaneFormats = 4;

// What a plane is for. It is here because the *count* of each kind is the constraint that bites — one
// primary, N overlays, one cursor is the shape of every controller gyro will meet — and not because
// anything downstream branches on the name.
enum class PlaneKind : std::uint8_t
{
	Primary,
	Overlay,
	Cursor,
};

struct PlaneCapability
{
	PlaneKind Kind = PlaneKind::Overlay;

	// Whether the pipe has a scaler at all. The ratio bounds below are meaningless without it, which is
	// why they are not consulted when it is false rather than being set to a degenerate range.
	bool Scales = false;

	// The scaler's limits, as the destination extent over the source extent, in sixteenths — integers
	// because a ratio bound compared in floating point is a bound that admits a different set on two
	// machines. A pipe that scales from a third to three times is `{ 5, 48 }`.
	std::uint32_t MinRatio16 = 16;
	std::uint32_t MaxRatio16 = 16;

	// The largest source rectangle the pipe will read. Line width is the real constraint on hardware
	// and height rarely is, but both are here because a catalog that only expressed the constraint that
	// usually binds would make the other one untestable.
	std::uint32_t MaxSourceWidth = 0;
	std::uint32_t MaxSourceHeight = 0;

	std::array<PixelFormat, MaxPlaneFormats> Formats{};
	std::uint32_t FormatCount = 0;

	[[nodiscard]] constexpr bool Accepts(PixelFormat format) const noexcept
	{
		for (std::uint32_t index = 0; index < std::min<std::uint32_t>(FormatCount, MaxPlaneFormats); ++index)
		{
			if (Formats[index] == format)
			{
				return true;
			}
		}

		return false;
	}

	// The cheap filter. Everything it can answer from the layer and the descriptor alone, and nothing
	// that would require knowing what the other planes are doing — which is the division of labour the
	// file header describes, and the reason this is a separate function from `Test`.
	[[nodiscard]] bool CouldExpress(const PresentLayer& layer, const RenderTarget& target) const noexcept
	{
		if (!Accepts(target.Format))
		{
			return false;
		}

		// An empty source means the whole image, per Seam/Presenter.h, so the extent to check is the
		// target's rather than the rectangle's.
		const float sourceWidth =
			layer.Source.IsEmpty() ? static_cast<float>(target.Size.Width) : layer.Source.Extent.Width;
		const float sourceHeight =
			layer.Source.IsEmpty() ? static_cast<float>(target.Size.Height) : layer.Source.Extent.Height;

		if (sourceWidth <= 0.0F || sourceHeight <= 0.0F || layer.Destination.Extent.IsEmpty())
		{
			return false;
		}

		if (sourceWidth > static_cast<float>(MaxSourceWidth) || sourceHeight > static_cast<float>(MaxSourceHeight))
		{
			return false;
		}

		const float widthRatio = static_cast<float>(layer.Destination.Extent.Width) / sourceWidth;
		const float heightRatio = static_cast<float>(layer.Destination.Extent.Height) / sourceHeight;
		const bool resamples = widthRatio != 1.0F || heightRatio != 1.0F;

		if (!resamples)
		{
			return true;
		}

		if (!Scales)
		{
			return false;
		}

		return InRatio(widthRatio) && InRatio(heightRatio);
	}

private:
	[[nodiscard]] constexpr bool InRatio(float ratio) const noexcept
	{
		const float scaled = ratio * 16.0F;

		return scaled >= static_cast<float>(MinRatio16) && scaled <= static_cast<float>(MaxRatio16);
	}
};

class PlaneCatalog
{
public:
	PlaneCatalog() = default;

	// Silently full past `MaxPlanes`, because a catalog is built once at construction from a
	// configuration the composition root wrote, and a hard failure there would be a startup abort for a
	// typo in a test fixture.
	constexpr void Add(const PlaneCapability& plane) noexcept
	{
		if (m_Count < MaxPlanes)
		{
			m_Planes[m_Count] = plane;
			++m_Count;
		}
	}

	[[nodiscard]] constexpr std::span<const PlaneCapability> Planes() const noexcept
	{
		return { m_Planes.data(), m_Count };
	}

	// Refuse the next `commits` layer sets that would otherwise have been accepted.
	//
	// **`EBUSY` rather than `EINVAL`, and the difference is the whole of what this simulates.**
	// Seam/Presenter.h separates a transient refusal from a set that cannot be expressed, and an
	// assigner is written to expect the first and to treat the second as its own bug. A scripted
	// refusal is the first: the layers were legal, the controller declined, and the frame's budget was
	// already planned around a promotion that is not happening.
	constexpr void RefuseNext(std::uint32_t commits) noexcept { m_Refusals = commits; }

	// Whether this controller will take these layers, bottom first. The only authority; see the header.
	//
	// Layers take planes in order, which is KMS's own arrangement — z is the list order on both sides,
	// so a greedy walk is the assignment rather than an approximation of one. A set longer than the
	// catalog is `EINVAL` for the same reason a set naming an unusable format is: it is a bug in
	// whatever produced it rather than a condition to retry.
	[[nodiscard]] Result<void> Test(std::span<const PresentLayer> layers, std::span<const RenderTarget> targets)
	{
		if (layers.empty() || layers.size() > m_Count)
		{
			return Failure(EINVAL, "layer count is not expressible on this controller");
		}

		for (std::size_t index = 0; index < layers.size(); ++index)
		{
			// The instrument allocates nothing and imports nothing, so a promoted texture has no image
			// behind it here. Refused rather than resolved, so a sweep never charges a cost for a layer
			// a real device would have had to import first.
			if (layers[index].Target.IsTexture())
			{
				return Failure(EINVAL, "a headless plane has no scanout for a promoted texture");
			}

			if (layers[index].Target.Index >= targets.size())
			{
				return Failure(EINVAL, "layer names a target the presenter does not own");
			}

			if (!m_Planes[index].CouldExpress(layers[index], targets[layers[index].Target.Index]))
			{
				return Failure(EINVAL, "layer is not expressible on the plane it would take");
			}
		}

		if (m_Refusals > 0)
		{
			--m_Refusals;

			return Failure(EBUSY, "controller refused a legal layer set");
		}

		return {};
	}

	// One primary plane that does not scale, `overlays` that do, and a cursor. The ordinary catalog, so
	// that a test wanting the ordinary thing does not spell one out and quietly pick different limits
	// from the test beside it.
	[[nodiscard]] static PlaneCatalog Typical(PixelSize<DeviceSpace> resolution, std::uint32_t overlays = 2)
	{
		const PixelFormat format{ FormatXrgb8888, 0, ModifierLinear };
		const auto width = static_cast<std::uint32_t>(std::max(resolution.Width, 0));
		const auto height = static_cast<std::uint32_t>(std::max(resolution.Height, 0));

		PlaneCatalog catalog;
		catalog.Add(
			{ .Kind = PlaneKind::Primary,
		      .MaxSourceWidth = width,
		      .MaxSourceHeight = height,
		      .Formats = { format },
		      .FormatCount = 1 }
		);

		for (std::uint32_t index = 0; index < overlays; ++index)
		{
			catalog.Add(
				{ .Kind = PlaneKind::Overlay,
			      .Scales = true,
			      .MinRatio16 = 5,
			      .MaxRatio16 = 48,
			      .MaxSourceWidth = width,
			      .MaxSourceHeight = height,
			      .Formats = { format, PixelFormat{ FormatArgb8888, 0, ModifierLinear } },
			      .FormatCount = 2 }
			);
		}

		catalog.Add(
			{ .Kind = PlaneKind::Cursor,
		      .MaxSourceWidth = 256,
		      .MaxSourceHeight = 256,
		      .Formats = { PixelFormat{ FormatArgb8888, 0, ModifierLinear } },
		      .FormatCount = 1 }
		);

		return catalog;
	}

private:
	std::array<PlaneCapability, MaxPlanes> m_Planes{};
	std::size_t m_Count = 0;
	std::uint32_t m_Refusals = 0;
};
