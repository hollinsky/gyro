#pragma once

#include <algorithm>
#include <vector>

#include "Core/Handle.h"

// Who the keyboard is talking to, as the world holds it.
//
// **Focus is the compositor's state and the shell's model**, which is the split
// [Architecture.md](../../Docs/Architecture.md#the-shell) already draws over input: gyro owns routing,
// hit-testing and grabs, and a shell declares in advance what focus *follows*. Decision 96 puts it on
// gyro's side from the other direction — the focus outline is drawn by the compositor because focus is
// state the compositor already owns — so this is the one copy of that state and everything else asks it.
//
// **It is in `Scene` rather than in `Protocol` because a focused window is a fact about the world and
// not about a connection.** The focus ring, `Animation/Author/Catalog.h`'s `FocusChange`, and the exit
// that has to hand focus on to somebody are all authored here; a `wl_keyboard.enter` is one consumer of
// the fact and, on a machine with a recovery console or a splash on screen, not even the only kind of
// window there is. `Protocol` reads it inside the `Advance` it already holds the store for, so no signal
// crosses and no observer has to be wired: the seat compares what it last told its clients against
// `Focused()` once per dispatch iteration, which is the same iteration the keystroke that changed it
// arrived in.
//
// **The policy while there is no shell is a stack, newest on top.** It is the Floorplanner's rule
// (141) applied to focus rather than to placement, and for the same reason: a window that just opened
// takes focus, and when it closes focus falls to whatever a person was using before it rather than to
// nothing. Both take no parameter, which is the whole of what gyro can honestly decide without the
// shell that owns the model.
//
// **Click-to-focus is the third rule of that shape and it is deliberately not here**, because it needs
// the pointer and this class has never seen one: [Protocol/Floor.h](../Protocol/Floor.h) holds it
// beside the placement, so both stand-ins are in one file and leave together when a shell declares a
// model. Decision 162 also says why raising is a verb of the store's rather than something this class
// does on the way past. Follows-mouse is still nobody's — it needs a preference and there is nowhere
// to keep one until there is a session.
//
// **Rejected: focus as a flag on the entity.** It reads naturally — one `bool` beside `Retiring` — and
// it makes *who is focused* a scan of the world, which is decision 115's rejected axis in a second
// place. Worse, two flags set at once is a representable state, and the bug it produces is two windows
// with a focus ring and a keyboard talking to the wrong one.
class SceneFocus
{
public:
	// The window the keyboard is on, or null where nothing is focusable.
	[[nodiscard]] EntityId Focused() const noexcept { return m_Stack.empty() ? EntityId{} : m_Stack.back(); }

	// A window became focusable, which is *mapped* for a client's toplevel. Takes focus, per the stack
	// policy above, and moves an entity already in the stack to the top rather than adding it twice.
	void Offer(EntityId id)
	{
		if (id.IsNull())
		{
			return;
		}

		std::erase(m_Stack, id);
		m_Stack.push_back(id);
	}

	// A window stopped being focusable: unmapped, retired, or destroyed. Focus falls to the entity
	// beneath it, which is where a person was before this one opened.
	//
	// **It runs at retirement rather than at destruction**, and decision 114's two steps are why: a
	// closing window is still on screen for as long as its exit takes, and keystrokes must not go on
	// reaching it while it collapses. The store calls this from `Retire`, so no author has to remember.
	void Withdraw(EntityId id) noexcept { std::erase(m_Stack, id); }

	// Put focus on an entity that is already focusable, which is the verb a shell declaring a model
	// calls and the one an alt-tab lands on. False where the entity is not in the stack — focusing a
	// window that has never been mapped is a request with no window behind it, and answering it by
	// adding one here would put focus somewhere nothing can draw a ring around.
	bool Focus(EntityId id)
	{
		const auto at = std::find(m_Stack.begin(), m_Stack.end(), id);

		if (at == m_Stack.end())
		{
			return false;
		}

		std::rotate(at, at + 1, m_Stack.end());

		return true;
	}

	// Whether this entity is one of the windows that could take focus, which is the question the pointer
	// asks on the way up from what it hit: a click lands on a surface and focus belongs to the window
	// around it. `Scene/Hit.h` is the caller and does the walk.
	//
	// **A membership test rather than a window onto the stack**, which stays private for the reason
	// below — and rather than letting the caller try `Focus` and read the answer, which would move focus
	// as a side effect of asking whether it could.
	[[nodiscard]] bool Contains(EntityId id) const noexcept
	{
		return std::find(m_Stack.begin(), m_Stack.end(), id) != m_Stack.end();
	}

	// How many windows could take focus. The stack itself stays private: the order below the top is
	// gyro's fallback and not a list anything else should be making decisions from.
	[[nodiscard]] std::size_t Count() const noexcept { return m_Stack.size(); }

private:
	// Newest last. A vector and a scan, because the length is the windows on the machine and every verb
	// here runs when one opens or closes rather than per frame or per keystroke.
	std::vector<EntityId> m_Stack;
};
