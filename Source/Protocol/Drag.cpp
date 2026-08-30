#include "Protocol/Drag.h"

#include "Animation/Author/Bundle.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Store.h"

bool WindowDrag::Begin(const SceneStore& scene, EntityId window)
{
	const Entity* const entity = scene.Find(window);

	if (entity == nullptr || entity->Retiring)
	{
		return false;
	}

	// Presentation rather than the model value, per the header: the drag starts from the rectangle the
	// person put their finger on, which for a window still animating is not where it was heading.
	m_Anchor = entity->Translation.Presentation(scene.Now());
	m_From = scene.Pointer().Position();
	m_Window = window;

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
	//
	// Z is the anchor's own: the pointer has no depth, and a window that was lifted stays lifted.
	const Vector3<double> position{
		m_Anchor.X + (at.X - m_From.X),
		m_Anchor.Y + (at.Y - m_From.Y),
		m_Anchor.Z,
	};

	// **`Compositor`, because gyro is handling input it routed** — the author decision 112 gives to
	// exactly this case, and the origin is the instant the hand moved so that anything this composes
	// with reads the same `t₀`. Nothing here is sprung, so the origin is carried rather than used.
	SceneCommit commit{ scene, CommitAuthor::Compositor, origin };

	static_cast<void>(commit.Move(m_Window, position, Immediate()));
}
