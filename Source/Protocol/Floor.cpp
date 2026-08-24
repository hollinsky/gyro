#include "Protocol/Floor.h"

#include <cerrno>
#include <optional>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"

Result<void> SessionFloor::Open(SceneStore& scene)
{
	if (!m_Container.IsNull())
	{
		return Failure(EEXIST, "the session floor is already authored");
	}

	// A top-level container at the origin with the identity transform. It carries no extent of its own:
	// the floor is where windows hang rather than a surface anything draws, and a container with an
	// extent would be a rectangle a frame treatment could round.
	const std::optional<EntityId> container = scene.CreateContainer(EntityId{}, {});

	if (!container)
	{
		return Failure(ENOSPC, "the entity space is exhausted before a single window exists");
	}

	m_Container = *container;

	return {};
}

void PlaceOnFloor(SceneCommit& commit, const SceneStore& scene, EntityId window, Size<SurfaceSpace, float> natural)
{
	const std::span<const SceneOutput> outputs = scene.Outputs();

	if (outputs.empty())
	{
		return;
	}

	// The output holding the pointer, which with no input devices is the first one. Written as a named
	// step rather than as `front()` inline, because the day there is a pointer this line is the whole
	// of the change.
	const Rect<GlobalSpace> bounds = outputs.front().Bounds;

	const double x = bounds.Left() + (bounds.Extent.Width - static_cast<double>(natural.Width)) * 0.5;
	const double y = bounds.Top() + (bounds.Extent.Height - static_cast<double>(natural.Height)) * 0.5;

	// **Immediate, and the entrance is what is missing rather than what is refused.** Decision 141 has a
	// floor placement stamped with the commit that created the entity, because nothing routed it and
	// there is no earlier moment to point an entrance at — the commit this runs in carries that origin
	// already. What it does not have is a catalog to name the entrance, so the window lands where it
	// belongs instead of growing into it, and the day the catalog arrives this is the one call that
	// changes.
	static_cast<void>(commit.Move(window, { x, y, 0.0 }, Immediate()));
}
