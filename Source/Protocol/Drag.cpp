#include "Protocol/Drag.h"

#include <algorithm>

#include "Animation/Author/Bundle.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Store.h"

namespace
{
// The smallest window a gesture will ask for. One rather than zero because zero on the wire is *pick
// your own size*, which is the opposite of what a person pulling an edge is saying.
constexpr float SmallestWindow = 1.0F;
} // namespace

bool WindowDrag::BeginMove(const SceneStore& scene, EntityId window)
{
	const Entity* const entity = scene.Find(window);

	if (entity == nullptr || entity->Retiring)
	{
		return false;
	}

	// Presentation rather than the model value, per the header: the gesture starts from the rectangle
	// the person put their finger on, which for a window still animating is not where it was heading.
	m_Anchor = entity->Translation.Presentation(scene.Now());
	m_Extent = entity->Extent;
	m_From = scene.Pointer().Position();
	m_Wanted = m_Extent;
	m_Edges = {};
	m_Resizing = false;
	m_Window = window;

	return true;
}

bool WindowDrag::BeginResize(const SceneStore& scene, EntityId window, ResizeEdges edges)
{
	if (!edges.Any() || !BeginMove(scene, window))
	{
		return false;
	}

	m_Edges = edges;
	m_Resizing = true;

	return true;
}

void WindowDrag::Track(SceneStore& scene, Instant origin)
{
	if (m_Window.IsNull())
	{
		return;
	}

	const Entity* const entity = scene.Find(m_Window);

	if (entity == nullptr || entity->Retiring)
	{
		End();

		return;
	}

	const Point<GlobalSpace> at = scene.Pointer().Position();

	// **The displacement is added to the anchor rather than composed through the parent**, and that is
	// exact for as long as a window hangs on a floor: a floor is a top-level container at the origin
	// with the identity transform ([Floor.h](Floor.h)), so a translation on the window is global space
	// with the axes unchanged. The day a shell puts a window inside a container that is scaled or
	// rotated — a workspace mid-swipe is the obvious one — this line becomes an unprojection of the
	// pointer into the parent's space, which is `Scene/Hit.h`'s arithmetic pointed the other way.
	const double dx = at.X - m_From.X;
	const double dy = at.Y - m_From.Y;

	if (m_Resizing)
	{
		// An edge that is being pulled moves by the whole displacement and the one opposite it does not
		// move at all, so each axis grows by the travel toward the far side. Pulling left or up is the
		// negative of pulling right or down, which is the only thing the sign here is saying.
		const double width = static_cast<double>(m_Extent.Width) + (m_Edges.Right ? dx : m_Edges.Left ? -dx : 0.0);
		const double height = static_cast<double>(m_Extent.Height) + (m_Edges.Bottom ? dy : m_Edges.Top ? -dy : 0.0);

		m_Wanted = { std::max(static_cast<float>(width), SmallestWindow),
			         std::max(static_cast<float>(height), SmallestWindow) };

		// **And nothing is written.** The extent is the client's to produce and the position follows the
		// size it produced (166), so a resize's whole effect on the world happens in the commit that
		// answers the configure this size is about to become.
		return;
	}

	// Z is the anchor's own: the pointer has no depth, and a window that was lifted stays lifted.
	const Vector3<double> position{ m_Anchor.X + dx, m_Anchor.Y + dy, m_Anchor.Z };

	// **`Compositor`, because gyro is handling input it routed** — the author decision 112 gives to
	// exactly this case, and the origin is the instant the hand moved so that anything this composes
	// with reads the same `t₀`. Nothing here is sprung, so the origin is carried rather than used.
	SceneCommit commit{ scene, CommitAuthor::Compositor, origin };

	static_cast<void>(commit.Move(m_Window, position, Immediate()));
}

Offset<GlobalSpace> WindowDrag::Between(Size<SurfaceSpace, float> from, Size<SurfaceSpace, float> to) const noexcept
{
	const Vector3<double> before = Anchored(from);
	const Vector3<double> after = Anchored(to);

	// Z is left out because a window is anchored in the plane: a resize changes no elevation, and the
	// spaces a popup is placed in are two dimensional in any case.
	return { after.X - before.X, after.Y - before.Y };
}

Vector3<double> WindowDrag::Anchored(Size<SurfaceSpace, float> actual) const noexcept
{
	// The fixed edge is the one the pointer is not on, so the origin moves by exactly the difference
	// between the extent this gesture started with and the extent the client came back with. Right and
	// bottom hold the origin still, which is why they are not written out.
	const double x = m_Edges.Left ? m_Anchor.X + static_cast<double>(m_Extent.Width - actual.Width) : m_Anchor.X;
	const double y = m_Edges.Top ? m_Anchor.Y + static_cast<double>(m_Extent.Height - actual.Height) : m_Anchor.Y;

	return { x, y, m_Anchor.Z };
}
