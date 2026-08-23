#include "Gym/Lanes.h"

#include <cerrno>
#include <optional>

#include "Core/ColorState.h"
#include "Geometry/Space.h"
#include "World/Content.h"
#include "World/Elevation.h"
#include "World/Material.h"

namespace
{
// The layout, as fractions of the output's own rectangle. See Gym/Lanes.h for why it is fractions.
//
// Four lanes of fourteen percent each, on a twenty-two percent pitch, starting a tenth of the way
// down: the last lane's bottom edge lands at ninety percent, so the instrument is inset from every
// edge and a marker that has escaped is escaping into visible space rather than off the panel.
constexpr double SideMargin = 0.08;
constexpr double FirstLaneTop = 0.10;
constexpr double LanePitch = 0.22;
constexpr double LaneHeight = 0.14;

// What the fade lane's marker covers of its track. The remainder is a strip of track that is never
// covered, so there is a constant reference swatch beside the fading one — without it, a lane that
// faded to the wrong value would look exactly like a lane that faded to the right one.
constexpr double FadeCoverage = 0.82;

// The rotation lane's marker, as multiples of the lane's height. A bar rather than a square, so that
// the orientation it is at is a thing a person can read off the frame.
constexpr double TurnMarkerWidth = 1.8;
constexpr double TurnMarkerHeight = 0.55;

// The palette. Four hues that nothing else in the scene uses, each as a dim track and a bright marker,
// so a lane is identified by colour and its state by which of the pair is showing.
//
// Every fill spells its colour state rather than taking the default, which is the same value. That is
// not tidiness: `Blit/Blit.cpp` refuses an item whose colour state is not the output's, and a refusal
// loses the whole frame — so the one field that could empty this instrument on the path it exists to
// be looked at on is the one field written out where a reader can check it.
constexpr SolidContent BackdropFill{ .Red = 0.07F, .Green = 0.08F, .Blue = 0.10F, .Color = ColorState::Srgb() };

constexpr SolidContent SlideTrack{ .Red = 0.30F, .Green = 0.18F, .Blue = 0.08F, .Color = ColorState::Srgb() };
constexpr SolidContent SlideMarker{ .Red = 0.98F, .Green = 0.55F, .Blue = 0.15F, .Color = ColorState::Srgb() };

constexpr SolidContent GrowTrack{ .Red = 0.10F, .Green = 0.26F, .Blue = 0.14F, .Color = ColorState::Srgb() };
constexpr SolidContent GrowMarker{ .Red = 0.30F, .Green = 0.85F, .Blue = 0.42F, .Color = ColorState::Srgb() };

constexpr SolidContent FadeTrack{ .Red = 0.16F, .Green = 0.18F, .Blue = 0.32F, .Color = ColorState::Srgb() };
constexpr SolidContent FadeMarker{ .Red = 0.35F, .Green = 0.55F, .Blue = 0.98F, .Color = ColorState::Srgb() };

constexpr SolidContent TurnTrack{ .Red = 0.30F, .Green = 0.12F, .Blue = 0.24F, .Color = ColorState::Srgb() };
constexpr SolidContent TurnMarker{ .Red = 0.95F, .Green = 0.35F, .Blue = 0.72F, .Color = ColorState::Srgb() };

[[nodiscard]] constexpr Vector3<double> At(double x, double y) noexcept
{
	return { x, y, 0.0 };
}

[[nodiscard]] constexpr Size<SurfaceSpace, float> Extent(double width, double height) noexcept
{
	return { static_cast<float>(width), static_cast<float>(height) };
}

// The node's own middle, which is where a scale or a rotation is fixed unless something summoned it
// from somewhere else. Spelled once rather than at each of its two call sites, because a fixed point
// half a marker off is a marker that swings out of its lane and reads as a translation bug.
[[nodiscard]] constexpr Vector3<float> Middle(Size<SurfaceSpace, float> extent) noexcept
{
	return { extent.Width * 0.5F, extent.Height * 0.5F, 0.0F };
}

// A creation sequence that stops at its first refusal.
//
// The store refuses a create only by exhausting the index space, and it refuses a *parent* that is not
// live — but a null parent is legal and means the top level, so a child of a container that failed
// would otherwise be promoted onto the output rather than refused with it. Latching is what keeps a
// half-authored instrument from being a scene at all: after the first failure nothing further is
// created, and `AuthorLanes` answers with an error rather than with a picture missing a lane.
class Builder
{
public:
	explicit Builder(SceneStore& scene) noexcept : m_Scene{ &scene } {}

	[[nodiscard]] EntityId Container(EntityId parent, const NodeProperties& properties)
	{
		return Adopt(m_Ok ? m_Scene->CreateContainer(parent, properties) : std::nullopt);
	}

	[[nodiscard]] EntityId Solid(EntityId parent, const NodeProperties& properties, const SolidContent& content)
	{
		return Adopt(m_Ok ? m_Scene->CreateSolid(parent, properties, content) : std::nullopt);
	}

	[[nodiscard]] bool Ok() const noexcept { return m_Ok; }

private:
	[[nodiscard]] EntityId Adopt(std::optional<EntityId> created) noexcept
	{
		m_Ok = m_Ok && created.has_value();

		return created.value_or(EntityId{});
	}

	SceneStore* m_Scene;
	bool m_Ok = true;
};

// One lane: the track, then the marker on top of it. The marker's extent and offset within the lane
// are the caller's, because that is the whole of what makes the four lanes different from each other.
[[nodiscard]] Lane BuildLane(
	Builder& builder,
	EntityId lane,
	Size<SurfaceSpace, float> track,
	const SolidContent& trackFill,
	const NodeProperties& marker,
	const SolidContent& markerFill
)
{
	Lane built{};

	built.Track = builder.Solid(lane, { .Extent = track }, trackFill);
	built.Marker = builder.Solid(lane, marker, markerFill);

	return built;
}
} // namespace

Result<LaneScene> AuthorLanes(SceneStore& scene)
{
	if (scene.Outputs().empty())
	{
		return Failure(ENODEV, "a gym is laid out against an output, and the store carries none");
	}

	// The first output rather than the union of them. A gym is one instrument to look at, and spreading
	// it across a set would make which lane is on which panel a property of the hotplug order.
	const Rect<GlobalSpace> bounds = scene.Outputs().front().Bounds;

	if (bounds.IsEmpty())
	{
		return Failure(EINVAL, "the first output has no bounds in global space, so a gym has nowhere to lay out");
	}

	const double width = bounds.Extent.Width;
	const double height = bounds.Extent.Height;

	const double laneWidth = width * (1.0 - 2.0 * SideMargin);
	const double laneHeight = height * LaneHeight;

	LaneScene lanes{};
	Builder builder{ scene };

	lanes.Stage = builder.Container({}, { .Position = At(bounds.Left(), bounds.Top()) });
	lanes.Backdrop = builder.Solid(lanes.Stage, { .Extent = Extent(width, height) }, BackdropFill);

	const auto laneAt = [&](int index) {
		return builder.Container(
			lanes.Stage,
			{ .Position = At(width * SideMargin, height * (FirstLaneTop + LanePitch * static_cast<double>(index))) }
		);
	};

	// Translation. The marker is a square that slides the length of its track, and the far end is the
	// track's right edge less the marker's own width so the two land flush — which is what makes an
	// overshoot a marker hanging past the end of its track rather than a marker somewhere in the middle.
	{
		const Size<SurfaceSpace, float> track = Extent(laneWidth, laneHeight);
		const Size<SurfaceSpace, float> marker = Extent(laneHeight, laneHeight);

		lanes.Slide = BuildLane(builder, laneAt(0), track, SlideTrack, { .Extent = marker }, SlideMarker);

		// Derived from the extents that were actually authored rather than from the fractions they came
		// from, because an extent is single precision and a position is double: computing the far end
		// out of the doubles would leave the marker off the end of its own track by whatever the two
		// roundings disagreed about, which is a fraction of a pixel of permanent overhang on the one
		// node whose job is to show where the end of the travel is.
		lanes.SlideNear = At(0.0, 0.0);
		lanes.SlideFar = At(static_cast<double>(track.Width) - static_cast<double>(marker.Width), 0.0);
	}

	// Scale. The track is the marker at full size, in the same place, so a settled marker covers its
	// track exactly and anything else is a difference a person can measure by eye. Centred in the lane
	// rather than at its left edge, and anchored at its own middle, so the growth is symmetric and a
	// marker that grew about the wrong point walks sideways.
	{
		const Size<SurfaceSpace, float> marker = Extent(laneHeight, laneHeight);
		const Vector3<double> centre = At((laneWidth - laneHeight) * 0.5, 0.0);

		lanes.Grow = BuildLane(
			builder,
			laneAt(1),
			Extent(laneWidth, laneHeight),
			GrowTrack,
			{ .Position = centre, .Scale = GrowSmall, .Anchor = Middle(marker), .Extent = marker },
			GrowMarker
		);

		// The track is the lane's full width and the marker sits in the middle of it, so the track alone
		// would not say where full size is. Nothing else is needed: at `GrowFull` the marker's own edges
		// are the answer, and the reference is the pair of them being flush with each other frame to
		// frame rather than a mark drawn under them.
	}

	// Opacity. The marker covers most of its track and never all of it, so the uncovered strip is a
	// constant swatch of the same hue at full strength — without it a lane that faded to the wrong
	// value looks exactly like one that faded to the right value.
	{
		const Size<SurfaceSpace, float> marker = Extent(laneWidth * FadeCoverage, laneHeight);

		lanes.Fade = BuildLane(
			builder,
			laneAt(2),
			Extent(laneWidth, laneHeight),
			FadeTrack,
			{ .Extent = marker, .Opacity = FadeDim },
			FadeMarker
		);
	}

	// Rotation. Authored still, and see Gym/Lanes.h for why: a turning quad is refused by the CPU
	// renderer outright, so only the gym that names rotation drives this and every other gym leaves it
	// as the reference the turning one is read against.
	{
		const Size<SurfaceSpace, float> marker = Extent(laneHeight * TurnMarkerWidth, laneHeight * TurnMarkerHeight);
		const Vector3<double> centre =
			At((laneWidth - static_cast<double>(marker.Width)) * 0.5,
		       (laneHeight - static_cast<double>(marker.Height)) * 0.5);

		lanes.Turn = BuildLane(
			builder,
			laneAt(3),
			Extent(laneWidth, laneHeight),
			TurnTrack,
			{ .Position = centre, .Anchor = Middle(marker), .Extent = marker },
			TurnMarker
		);
	}

	if (!builder.Ok())
	{
		return Failure(ENOSPC, "the entity index space is exhausted, so the gym has no scene");
	}

	return lanes;
}

Result<MaterialOverlay> AuthorMaterialOverlay(SceneStore& scene, const LaneScene& lanes)
{
	if (!scene.IsLive(lanes.Stage))
	{
		return Failure(EINVAL, "a material overlay is laid over a lane scene, and this one names none");
	}

	// Asked again rather than carried on the record, because the output set is replaced whole on
	// hotplug and the stage being live says nothing about the set that placed it.
	if (scene.Outputs().empty())
	{
		return Failure(ENODEV, "a gym is laid out against an output, and the store carries none");
	}

	const Rect<GlobalSpace> bounds = scene.Outputs().front().Bounds;

	const double width = bounds.Extent.Width;
	const double height = bounds.Extent.Height;

	// Two panels side by side over the whole run of lanes, inset from the top and bottom so that the
	// lanes are visibly running out from under them. Side by side rather than stacked because the two
	// materials differ in what they owe rather than in weight — `Glass` sits over content the user
	// arranged and `Smoke` over content gyro did not choose — and reading that difference means seeing
	// the same moving lanes through both at once.
	//
	// **The two panels also sit at the two lifted levels**, which is the only way the light table gets
	// looked at: decision 104's heights and its two constants are numbers Open.md leaves to a sitting
	// with a screen, and ranking one height against another means having both under the same lanes at
	// the same moment. One level twice would show a shadow and settle nothing.
	const double panelTop = height * 0.06;
	const double panelHeight = height * 0.88;
	const double panelWidth = width * 0.24;

	Builder builder{ scene };

	MaterialOverlay overlay{};

	overlay.Glass = builder.Container(
		lanes.Stage,
		{ .Position = At(width * 0.14, panelTop),
	      .Extent = Extent(panelWidth, panelHeight),
	      .Dress = Material::Glass,
	      .Lift = Elevation::Floating }
	);

	overlay.Smoke = builder.Container(
		lanes.Stage,
		{ .Position = At(width * 0.62, panelTop),
	      .Extent = Extent(panelWidth, panelHeight),
	      .Dress = Material::Smoke,
	      .Lift = Elevation::Resting }
	);

	if (!builder.Ok())
	{
		return Failure(ENOSPC, "the entity index space is exhausted, so the gym has no overlay");
	}

	return overlay;
}
