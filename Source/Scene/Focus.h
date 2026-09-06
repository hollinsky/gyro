#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "Core/Handle.h"
#include "Core/Session.h"
#include "Scene/Output.h"

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
// **Cycling is the fourth rule of that shape and it *is* here**, unlike the click, because it needs
// nothing this class has not got: `Alt+Tab` walks the same stack the fallback already reads, in the
// order a person's own history put it in. [Input/Chord.h](../Input/Chord.h) carries why the binding is
// a held modifier and not a verb behind the leader; what it costs here is a cursor, and the argument
// for it being a window rather than a position is at `CycleNext`.
//
// **One stack for the machine, and what bounds it is which sessions are on screen rather than a
// second stack per session.** There is one keyboard, so *who is typed into* is one answer however
// many sessions decision 21 has alive at once — but an entry belonging to a session no output is
// showing must not be that answer, or locking the screen would leave a person's keystrokes going to
// the window that was in front when they locked it. So an entry carries the session it was offered
// for and the queries below step over the ones nobody is looking at, which is the same rule
// `Scene/Hit.h` applies to the pointer: a session is reachable exactly where it is presented.
//
// **The session is carried on the entry rather than resolved from the tree at every query**, and what
// makes the copy safe is that it cannot go stale: a window is offered focus when it maps, into the
// floor of the session its own connection belongs to (decision 23 makes the listening socket the
// identity), and nothing reparents a mapped toplevel between sessions. `SessionId::None` is gyro's
// own and is always reachable, which is what keeps the splash, the recovery console and every author
// with no session behind it working unchanged.
//
// What a focusable thing is, and the walk through the windows is the only reader of it.
//
// **It exists because a launcher needs one half of focus and not the other.** A shell's own surfaces
// (187) have to take the keyboard when they map — a launcher a person cannot type into is not one —
// and must never be somewhere `Alt+Tab` can stop, because the shell drawing the switcher would then
// be in its own list. One stack answers both questions and this is what tells them apart, rather than
// a second stack that would have to be kept in step with this one across every map, unmap and
// retirement.
enum class FocusKind
{
	// An application's window, which is what cycling is for and what every caller but one means.
	Window,

	// A surface the shell draws: a launcher, a panel, an overview. It takes the keyboard and the walk
	// steps over it, so dismissing it puts a person back on the window they were using rather than
	// wherever a cycle had wandered to.
	Chrome,
};

// **Rejected: focus as a flag on the entity.** It reads naturally — one `bool` beside `Retiring` — and
// it makes *who is focused* a scan of the world, which is decision 115's rejected axis in a second
// place. Worse, two flags set at once is a representable state, and the bug it produces is two windows
// with a focus ring and a keyboard talking to the wrong one.
class SceneFocus
{
	// One entry, which is an entity and whether the walk through the windows visits it.
	//
	// **The private section comes first here, which is the one place in this file that is about the
	// compiler rather than about focus**: every verb below looks an entity up, and a lookup helper with
	// a deduced return type has to be declared before the bodies that call it.
	struct Entry
	{
		EntityId Id{};
		FocusKind Kind = FocusKind::Window;

		// Whose window this is. `None` is gyro's own and is reachable wherever anything is.
		SessionId Session = SessionId::None;
	};

	using Entries = std::vector<Entry>;

	[[nodiscard]] Entries::iterator Find(EntityId id) noexcept
	{
		return std::find_if(m_Stack.begin(), m_Stack.end(), [id](const Entry& entry) noexcept {
			return entry.Id == id;
		});
	}

	[[nodiscard]] Entries::const_iterator Find(EntityId id) const noexcept
	{
		return std::find_if(m_Stack.begin(), m_Stack.end(), [id](const Entry& entry) noexcept {
			return entry.Id == id;
		});
	}

	void Erase(EntityId id) noexcept
	{
		std::erase_if(m_Stack, [id](const Entry& entry) noexcept { return entry.Id == id; });
	}

	// One session into one of the lists above, skipping `None` — which needs no entry, being gyro's own
	// and reachable wherever anything is — and skipping a session already there, since two monitors
	// showing one session is ordinary.
	static void Record(std::vector<SessionId>& into, SessionId session)
	{
		if (session != SessionId::None && std::find(into.begin(), into.end(), session) == into.end())
		{
			into.push_back(session);
		}
	}

	// Whether an output is showing this entry's session, which is what makes it something the keyboard
	// may be on. Linear in the outputs, which is the monitors on the desk.
	[[nodiscard]] bool Reachable(const Entry& entry) const noexcept
	{
		return entry.Session == SessionId::None ||
		       std::find(m_Presented.begin(), m_Presented.end(), entry.Session) != m_Presented.end();
	}

public:
	// The window the keyboard is on, or null where nothing is focusable.
	//
	// **A cycle in flight answers here rather than somewhere else**, which is the whole of what makes
	// `Alt+Tab` feel like one gesture: the window a person has stepped onto is lit, has the keyboard and
	// is drawn in front, and letting go of `Alt` changes nothing they can see. The alternative — leaving
	// focus behind until the release — puts the keyboard on one window while another is raised in front
	// of it, and a cycle whose release is never seen leaves a person typing into something they cannot
	// see. Nested gyro loses that release routinely (175), so this is the case rather than the corner.
	[[nodiscard]] EntityId Focused() const noexcept
	{
		const auto candidate = Find(m_Candidate);

		if (!m_Candidate.IsNull() && candidate != m_Stack.end() && Reachable(*candidate))
		{
			return m_Candidate;
		}

		// The newest entry anybody is looking at. An output being reassigned takes every window of the
		// session it was showing out of reach in one step, and focus falls through them to whatever is
		// still presented — which is nothing at all on a machine whose only screen has just been locked.
		for (auto entry = m_Stack.rbegin(); entry != m_Stack.rend(); ++entry)
		{
			if (Reachable(*entry))
			{
				return entry->Id;
			}
		}

		return EntityId{};
	}

	// Which sessions the outputs are showing, which is the whole of what this class knows about screens.
	//
	// **The store calls it, for the reason it reconfines the pointer there**: an output set replaced by
	// a hotplug and an output handed to a session are both facts nobody offering a window should have to
	// remember to restate. `Scene/Store.h`'s `SetOutputs` and `SetOutputSession` are the two writers.
	void Present(std::span<const SceneOutput> outputs)
	{
		m_Presented.clear();
		m_Leaving.clear();

		for (const SceneOutput& output : outputs)
		{
			Record(m_Presented, output.Session);
			Record(m_Leaving, output.Outgoing);
		}
	}

	// The window that is not where the keyboard is any more but must not be told so yet.
	//
	// **A session on its way off a screen is still on it, and a client told it lost focus says so on
	// the glass** — a terminal's caret stops, a titlebar greys, a text selection loses its colour — in
	// the middle of an animation whose whole point is that the screen a person is leaving looks the way
	// they left it. So the keyboard crosses to the arriving session at once (188) and the notice to the
	// departing one waits for the fade, which is `Protocol/Seat.h`'s withheld `wl_keyboard.leave` and
	// the `activated` state `Protocol/Shell.h` holds against it.
	//
	// **One window rather than the session's whole set**, because it is the only one that was ever told
	// it had focus: the rest are already unfocused and have nothing to be spared. Null outside a
	// transition, which is every frame today.
	//
	// It is not a licence to deliver anything: keystrokes route by `Focused()` alone, so what this
	// buys the departing client is a lie about focus for the length of a fade and never a key.
	[[nodiscard]] EntityId Leaving() const noexcept
	{
		for (auto entry = m_Stack.rbegin(); entry != m_Stack.rend(); ++entry)
		{
			if (!Reachable(*entry) && std::find(m_Leaving.begin(), m_Leaving.end(), entry->Session) != m_Leaving.end())
			{
				return entry->Id;
			}
		}

		return EntityId{};
	}

	// A window became focusable, which is *mapped* for a client's toplevel. Takes focus, per the stack
	// policy above, and moves an entity already in the stack to the top rather than adding it twice.
	//
	// **The kind defaults to `Window` because that is what offering has always meant here**, and chrome
	// is the exception that has to name itself: there is exactly one caller that passes anything else,
	// and a default the other way would make every test and every future author opt out of being the
	// shell.
	void Offer(EntityId id, FocusKind kind = FocusKind::Window, SessionId session = SessionId::None)
	{
		if (id.IsNull())
		{
			return;
		}

		Erase(id);
		m_Stack.push_back(Entry{ .Id = id, .Kind = kind, .Session = session });

		// A window opening ends a walk through the windows, because the walk is about which of the ones
		// already there a person meant and this is a new answer to that. Without it the cycle's cursor
		// would go on holding focus and the application somebody just launched would open behind it.
		m_Candidate = EntityId{};
	}

	// A window stopped being focusable: unmapped, retired, or destroyed. Focus falls to the entity
	// beneath it, which is where a person was before this one opened.
	//
	// **It runs at retirement rather than at destruction**, and decision 114's two steps are why: a
	// closing window is still on screen for as long as its exit takes, and keystrokes must not go on
	// reaching it while it collapses. The store calls this from `Retire`, so no author has to remember.
	void Withdraw(EntityId id) noexcept { Erase(id); }

	// Put focus on an entity that is already focusable, which is the verb a shell declaring a model
	// calls and the one an alt-tab lands on. False where the entity is not in the stack — focusing a
	// window that has never been mapped is a request with no window behind it, and answering it by
	// adding one here would put focus somewhere nothing can draw a ring around.
	bool Focus(EntityId id)
	{
		const auto at = Find(id);

		if (at == m_Stack.end())
		{
			return false;
		}

		std::rotate(at, at + 1, m_Stack.end());

		// Anything that names a window outright — a click, a shell, the landing of a cycle — settles where
		// focus is, so there is nothing left to be walking. It matters for the click: a person who
		// abandons a half-finished `Alt+Tab` by reaching for the mouse would otherwise keep typing into
		// the window the cursor was left on.
		m_Candidate = EntityId{};

		return true;
	}

	// One step of `Alt+Tab`: the window after the one the cycle is on, and the one before it. The answer
	// is what the caller raises, and it is null only where there are no windows at all.
	//
	// **The walk does not reorder anything, which is the difference between cycling and swapping.**
	// `Focus` moves its target to the top, so stepping with it would make every second press go back
	// where it came from and a third window would be unreachable. So the cursor is a window rather than
	// a position in the stack, and the order underneath it stays exactly as a person's history left it
	// until `EndCycle` writes the landing into it.
	//
	// **A cursor that is a window rather than an index is also what survives a window closing under it.**
	// An application that exits mid-gesture takes its entry out of the stack, and an index into it would
	// then be pointing at somebody else — where a name that is no longer there simply resumes the walk
	// from the focused window, which is what a person would expect of the one that just vanished.
	//
	// **Chrome is stepped over rather than skipped once**, which is why the walk is a loop and not one
	// modular step: a session with a launcher and a panel up has two entries the cycle must pass in a
	// single press, and stopping on either would put focus on the shell's furniture. Where there is no
	// window at all — a person pressing `Alt+Tab` with nothing but a panel on screen — the answer is
	// null and nothing is cycling, rather than a walk that spins.
	EntityId CycleNext() noexcept { return Step(true); }
	EntityId CyclePrevious() noexcept { return Step(false); }

	// The hand came off `Alt`. Where the cycle landed becomes the most recent window, so the *next*
	// gesture starts from here — and where nothing was cycling, or where the window it was on has gone,
	// this is null and changes nothing.
	EntityId EndCycle()
	{
		const EntityId landed = m_Candidate;

		m_Candidate = EntityId{};

		return (!landed.IsNull() && Focus(landed)) ? landed : EntityId{};
	}

	// Whether this entity is one of the windows that could take focus, which is the question the pointer
	// asks on the way up from what it hit: a click lands on a surface and focus belongs to the window
	// around it. `Scene/Hit.h` is the caller and does the walk.
	//
	// **A membership test rather than a window onto the stack**, which stays private for the reason
	// below — and rather than letting the caller try `Focus` and read the answer, which would move focus
	// as a side effect of asking whether it could.
	[[nodiscard]] bool Contains(EntityId id) const noexcept { return Find(id) != m_Stack.end(); }

	// How many windows could take focus. The stack itself stays private: the order below the top is
	// gyro's fallback and not a list anything else should be making decisions from.
	[[nodiscard]] std::size_t Count() const noexcept { return m_Stack.size(); }

private:
	// Both directions of the walk. Forward is *down* the stack, which is towards the window a person used
	// before this one, and both ends wrap.
	EntityId Step(bool forward) noexcept
	{
		if (m_Stack.empty())
		{
			return EntityId{};
		}

		const auto count = static_cast<std::ptrdiff_t>(m_Stack.size());
		const auto at = Find(m_Candidate);
		std::ptrdiff_t from = (at == m_Stack.end()) ? count - 1 : at - m_Stack.begin();

		// At most one lap. Every step lands somewhere the walk has not been this press, so a stack that is
		// all chrome terminates having moved nothing rather than looking for a window that is not there.
		for (std::ptrdiff_t taken = 0; taken < count; ++taken)
		{
			from = (from + (forward ? count - 1 : 1)) % count;

			const Entry& entry = m_Stack[static_cast<std::size_t>(from)];

			// Chrome is stepped over, and so is a window of a session nobody is looking at — the walk is
			// through what a person could be shown, and a locked screen has nothing on it to walk through.
			if (entry.Kind == FocusKind::Window && Reachable(entry))
			{
				m_Candidate = entry.Id;

				return m_Candidate;
			}
		}

		return EntityId{};
	}

	// Newest last. A vector and a scan, because the length is the windows on the machine and every verb
	// here runs when one opens or closes rather than per frame or per keystroke.
	Entries m_Stack;

	// Where a walk through the windows has got to, and null when nobody is walking. It is deliberately
	// not a position: see `CycleNext`.
	EntityId m_Candidate;

	// The sessions the outputs are showing, without `None`, which needs no entry because it is reachable
	// wherever anything is. Empty is the ordinary state of a machine before an agent has connected, and
	// it is what makes every author with no session behind it — the splash, the console, a gym — read
	// exactly as it did before this class knew what a session was.
	std::vector<SessionId> m_Presented;

	// The sessions an output is moving away from, which is the same list one transition later. A
	// session can be in both — two monitors, one of them switching — and `Reachable` wins there, since
	// a person is still looking at it somewhere and the keyboard has somewhere honest to be.
	std::vector<SessionId> m_Leaving;
};
