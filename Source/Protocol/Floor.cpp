#include "Protocol/Floor.h"

#include <algorithm>
#include <cerrno>
#include <optional>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Scene/Entity.h"
#include "Scene/Hit.h"
#include "Scene/Output.h"

Result<void> SessionFloors::Open(SceneStore& scene, SessionId session)
{
	if (!Container(session).IsNull())
	{
		return Failure(EEXIST, "a second floor for a session that already has one");
	}

	// A top-level container at the origin with the identity transform. It carries no extent of its own:
	// the floor is where windows hang rather than a surface anything draws, and a container with an
	// extent would be a rectangle a frame treatment could round.
	const std::optional<EntityId> container = scene.CreateContainer(EntityId{}, {});

	if (!container)
	{
		return Failure(ENOSPC, "the entity space is exhausted before a single window exists");
	}

	// The floor is a root, so this is the one call that puts the session on the world. Everything
	// hanging under it is in that session by being under it, which is why no window is ever asked.
	if (!scene.SetSession(*container, session))
	{
		// Unreachable while the line above created a live root, and undone rather than ignored: a floor
		// nothing can attribute would collect a session's windows and be drawn on every screen. Retired
		// rather than freed because the store's writable side is the commit scope's (112), and a
		// container with nothing under it is swept on the very next serialisation.
		SceneCommit undo{ scene, CommitAuthor::Compositor, scene.Now() };

		static_cast<void>(undo.Retire(*container));

		return Failure(EINVAL, "attributing a floor to its session");
	}

	m_Floors.push_back(Floor{ .Session = session, .Container = *container });

	return {};
}

void SessionFloors::Close(SceneStore& scene, SessionId session) noexcept
{
	const auto found = std::find_if(m_Floors.begin(), m_Floors.end(), [session](const Floor& floor) noexcept {
		return floor.Session == session;
	});

	if (found == m_Floors.end())
	{
		return;
	}

	// Retired rather than destroyed, so the windows still on it play whatever exit they are owed and are
	// freed when they have settled (114). The record leaves now regardless: a session that has ended
	// must not be found by a commit arriving from a client that has not been dropped yet.
	//
	// **Its own transaction, because a session ending is gyro's and not a client's.** The origin is now
	// rather than an input timestamp for the reason the Floorplanner's placement has none: nothing
	// routed this, and what ended the session was a socket closing.
	{
		SceneCommit closing{ scene, CommitAuthor::Compositor, scene.Now() };

		static_cast<void>(closing.Retire(found->Container));
	}

	m_Floors.erase(found);
}

EntityId SessionFloors::Container(SessionId session) const noexcept
{
	const auto found = std::find_if(m_Floors.begin(), m_Floors.end(), [session](const Floor& floor) noexcept {
		return floor.Session == session;
	});

	return found == m_Floors.end() ? EntityId{} : found->Container;
}

void PlaceOnFloor(
	SceneCommit& commit,
	const SceneStore& scene,
	SessionId session,
	EntityId window,
	Size<SurfaceSpace, float> natural
)
{
	const std::span<const SceneOutput> outputs = scene.Outputs();

	// **The first output showing this session, and no placement at all where none is.** Centring on
	// `outputs.front()` regardless would put a window on a monitor its own session is not being shown
	// on — an application a person launched, running and drawing, on a screen they are not looking at
	// and cannot bring it to. A window that stays unplaced is invisible until an output arrives, which
	// is the state a session switched away from is already in.
	const auto shown = std::find_if(outputs.begin(), outputs.end(), [session](const SceneOutput& output) noexcept {
		return output.Session == session;
	});

	if (shown == outputs.end())
	{
		return;
	}

	// The output holding the pointer, which with no input devices is the first one shown. Written as a
	// named step rather than inline, because the day there is a pointer this line is the whole of the
	// change.
	const Rect<GlobalSpace> bounds = shown->Bounds;

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

void FocusByClick(SceneStore& scene, EntityId hit)
{
	const EntityId window = FocusTargetFor(scene, hit);

	if (window.IsNull())
	{
		return;
	}

	// Both, and in this order only because the answer reads better that way: `Focus` is refused for an
	// entity the stack does not hold, and `FocusTargetFor` found this one in it, so neither call can
	// fail here and neither depends on the other having run.
	static_cast<void>(scene.Focus().Focus(window));
	static_cast<void>(scene.Raise(window));
}
