#include "Protocol/Floor.h"

#include <algorithm>
#include <cerrno>
#include <optional>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Scene/Entity.h"
#include "Scene/Hit.h"
#include "Scene/Output.h"
#include "Scene/Reach.h"

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
	// The chrome root (187), created after the floor so that it is the later root and therefore the one
	// in front (55). It carries no extent for the same reason the floor does not.
	const std::optional<EntityId> chrome = scene.CreateContainer(EntityId{}, {});

	// **Both are given up together, and a session with one of them is never a state anything sees.** A
	// floor with no chrome root above it would take a shell's launcher onto the floor at its first
	// commit, where a click on a window would put the window in front of it.
	const auto abandon = [&scene, &container, &chrome](int error, const char* what) {
		SceneCommit undo{ scene, CommitAuthor::Compositor, scene.Now() };

		static_cast<void>(undo.Retire(*container));

		if (chrome)
		{
			static_cast<void>(undo.Retire(*chrome));
		}

		return Failure(error, what);
	};

	if (!chrome)
	{
		return abandon(ENOSPC, "the entity space is exhausted before a single window exists");
	}

	// Unreachable while the lines above created live roots, and undone rather than ignored: a root
	// nothing can attribute would collect a session's windows and be drawn on every screen. Retired
	// rather than freed because the store's writable side is the commit scope's (112), and a container
	// with nothing under it is swept on the very next serialisation.
	if (!scene.SetSession(*container, session) || !scene.SetSession(*chrome, session))
	{
		return abandon(EINVAL, "attributing a floor to its session");
	}

	m_Floors.push_back(Floor{ .Session = session, .Container = *container, .Chrome = *chrome });

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

		// The shell's own surfaces go with the windows and in the same transaction, so a session ending is
		// one screen emptying rather than the panel outliving what was under it by a frame.
		static_cast<void>(closing.Retire(found->Chrome));
	}

	m_Floors.erase(found);
}

const SessionFloors::Floor* SessionFloors::Find(SessionId session) const noexcept
{
	const auto found = std::find_if(m_Floors.begin(), m_Floors.end(), [session](const Floor& floor) noexcept {
		return floor.Session == session;
	});

	return found == m_Floors.end() ? nullptr : &*found;
}

EntityId SessionFloors::Container(SessionId session) const noexcept
{
	const Floor* const found = Find(session);

	return found == nullptr ? EntityId{} : found->Container;
}

EntityId SessionFloors::Chrome(SessionId session) const noexcept
{
	const Floor* const found = Find(session);

	return found == nullptr ? EntityId{} : found->Chrome;
}

const SceneOutput* OutputFor(const SceneStore& scene, SessionId session, EntityId window)
{
	const std::span<const SceneOutput> outputs = scene.Outputs();

	// **The first output showing this session, and nothing at all where none is.** Taking
	// `outputs.front()` regardless would name a monitor this session is not being shown on — for
	// `PlaceOnFloor` below that is an application a person launched, running and drawing, on a screen
	// they are not looking at and cannot bring it to.
	const auto shown = std::find_if(outputs.begin(), outputs.end(), [session](const SceneOutput& output) noexcept {
		return output.Session == session;
	});

	if (shown == outputs.end())
	{
		return nullptr;
	}

	const Coverage cover = window.IsNull() ? Coverage{} : Cover(scene, window);

	if (!cover.Definite)
	{
		return &*shown;
	}

	// **Most of the window rather than its top-left corner**, which is the difference a person notices
	// when they drag a window between two panels of different scales: the corner crosses at the moment
	// a sliver has, and constraining against the new screen while nine tenths of the window is still on
	// the old one is a window that changes what it can be for no reason the hand can see.
	const SceneOutput* best = &*shown;
	double covered = 0.0;

	for (const SceneOutput& output : outputs)
	{
		if (output.Session != session)
		{
			continue;
		}

		const double width =
			std::min(cover.Bounds.Right(), output.Bounds.Right()) - std::max(cover.Bounds.Left(), output.Bounds.Left());
		const double height =
			std::min(cover.Bounds.Bottom(), output.Bounds.Bottom()) - std::max(cover.Bounds.Top(), output.Bounds.Top());

		const double area = width > 0.0 && height > 0.0 ? width * height : 0.0;

		if (area > covered)
		{
			covered = area;
			best = &output;
		}
	}

	// A window that has been dragged off every screen keeps the session's first, which is the same
	// answer an unplaced one gets and for the same reason: there is nothing else true to say, and the
	// alternative is telling a client its bounds are unknown at the moment it is furthest from being
	// able to work them out itself.
	return best;
}

void PlaceOnFloor(
	SceneCommit& commit,
	const SceneStore& scene,
	SessionId session,
	EntityId window,
	Size<SurfaceSpace, float> natural
)
{
	// The output holding the pointer, which with no input devices is the first one shown. Asked with no
	// window because there is nothing in the world yet to ask about — this call is what puts it
	// somewhere — and written as a named step rather than inline, because the day there is a pointer
	// this line is the whole of the change.
	const SceneOutput* const shown = OutputFor(scene, session, EntityId{});

	// A window that stays unplaced is invisible until an output arrives, which is the state a session
	// switched away from is already in.
	if (shown == nullptr)
	{
		return;
	}

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
