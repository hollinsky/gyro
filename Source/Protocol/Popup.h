#pragma once

#include <vector>

#include "Core/Handle.h"

class SceneStore;
class ClientXdgPopup;

// The popups that have taken a grab, newest last, across every client on the machine.
//
// **A grab is one thing and there is one of it**, which is why this is not a member of a client's
// `xdg_wm_base`. `xdg_popup.grab` is how a toolkit says *this menu owns the input until it goes away*,
// and the protocol makes the stack of them global rather than per connection: a second client's popup
// grabbing while the first's is up is refused, and the compositor dismisses from the top down. So the
// stack lives beside the floor on [Context.h](Context.h) — one per host, which is one per session,
// which today is one per machine.
//
// **What gyro does with it is dismiss on a press outside**, which is the whole of the grab that is
// implemented and the whole of what a menu needs to be usable: a person clicks away and the menu goes.
// A grab in the protocol's full sense also *routes* — the client owning it sees pointer events for its
// own surfaces and nobody else sees any — and that half is not built. The cost is stated rather than
// silent: a press that dismisses a menu is also delivered to whatever it landed on, so clicking
// another application's window while a menu is open both closes the menu and activates that window.
// It is the behaviour a person gets on a Mac and not the one they get on Windows, it takes no
// parameter, and the piece that would make it exact is per-client filtering in
// [Seat.h](Seat.h)'s delivery rather than anything here.
//
// **Nothing here owns a popup.** Each one registers as it maps with a grab and removes itself as it
// unmaps or dies, exactly as the keyboards and pointers do on the seat, so no entry outlives what it
// names.
class PopupStack
{
public:
	// A popup mapped with a grab. Newest on top, which is the order the protocol requires a client to
	// create and destroy them in.
	void Push(ClientXdgPopup& popup);

	void Remove(ClientXdgPopup& popup) noexcept;

	// The popup a new grab must be a child of, or null where there is no grab. The protocol's
	// `not_the_topmost_popup` is this comparison and nothing more.
	[[nodiscard]] ClientXdgPopup* Topmost() const noexcept { return m_Grabs.empty() ? nullptr : m_Grabs.back(); }

	[[nodiscard]] bool IsEmpty() const noexcept { return m_Grabs.empty(); }

	// A press landed on `hit`. Dismiss every grabbing popup the press was not inside, from the top down.
	//
	// **Inside means anywhere in the popup's subtree**, which is what makes a click on a submenu leave
	// its parent menu standing: the ancestor walk from the node that was hit passes through the submenu's
	// container and then the parent's, so the press is inside both and neither is dismissed. A press on
	// the application's own window is inside neither, and the whole chain goes.
	void DismissOutside(const SceneStore& scene, EntityId hit);

private:
	// Oldest first. A vector and a scan, for `SceneFocus`'s reason: the length is the depth of one open
	// menu — two or three — and every verb here runs when a menu opens or closes rather than per frame.
	std::vector<ClientXdgPopup*> m_Grabs;
};
