#include "Protocol/Popup.h"

#include <algorithm>
#include <cstddef>

#include "Protocol/Shell.h"
#include "Scene/Entity.h"
#include "Scene/Reach.h"
#include "Scene/Store.h"

void PopupStack::Push(ClientXdgPopup& popup)
{
	std::erase(m_Grabs, &popup);

	m_Grabs.push_back(&popup);
}

void PopupStack::Remove(ClientXdgPopup& popup) noexcept
{
	std::erase(m_Grabs, &popup);
}

void PopupStack::DismissOutside(const SceneStore& scene, EntityId hit)
{
	if (m_Grabs.empty())
	{
		return;
	}

	// **How deep in the chain the press landed**, walked once from the node that was hit rather than
	// once per popup. Everything above that depth goes; the popup the press was inside, and every parent
	// of it, stays — which is what makes clicking a submenu leave the menu it came out of standing.
	//
	// Capped at `Scene/Reach.h`'s depth for the reason every ancestor walk in this codebase is: an
	// author picks the depth and this is the one process on the machine that cannot overflow a stack.
	std::size_t keep = 0;

	EntityId at = hit;

	for (std::size_t depth = 0; !at.IsNull() && depth < MaxReachDepth; ++depth)
	{
		const auto found = std::find_if(m_Grabs.begin(), m_Grabs.end(), [at](const ClientXdgPopup* popup) noexcept {
			return popup->Node() == at;
		});

		if (found != m_Grabs.end())
		{
			keep = static_cast<std::size_t>(found - m_Grabs.begin()) + 1;

			break;
		}

		const Entity* const entity = scene.Find(at);

		if (entity == nullptr)
		{
			break;
		}

		at = entity->Parent;
	}

	// **Topmost first, which is the order the protocol requires of a client and therefore the order a
	// compositor dismissing on its behalf has to use.** A submenu whose parent went first is a popup
	// with no parent, and the client's own teardown asserts on that.
	while (m_Grabs.size() > keep)
	{
		// `Dismiss` removes the popup from this stack, so the loop condition is what advances it — and
		// re-reading the back each time is what makes it safe for a popup that removes more than itself.
		ClientXdgPopup* const top = m_Grabs.back();

		top->Dismiss();

		if (!m_Grabs.empty() && m_Grabs.back() == top)
		{
			// A popup that did not take itself off the stack would spin here forever, on the dispatch
			// thread, beside a `SCHED_FIFO` frame thread. It cannot happen — `Dismiss` calls `Leave` — and
			// the loop still refuses to be the place it would.
			m_Grabs.pop_back();
		}
	}
}
