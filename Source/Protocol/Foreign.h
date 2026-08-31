#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/Handle.h"
#include "Wayland/Server/ExtForeignToplevelListV1.h"

class ClientXdgSurface;
class ForeignToplevelGlobal;
class ForeignToplevelList;
class HostContext;

// `ext_foreign_toplevel_list_v1`: the windows on this machine, named and described to a client that is
// not their author.
//
// **This is the first global in the System tier, and being first is most of what it is for.**
// [Tier.h](Tier.h) landed the mechanism with nothing on the far side of it — every interface gyro
// served was one every application may have — so the filter was a rule with no case. This is the case:
// a list of *other people's* windows, with their titles, is the plainest thing on the machine that an
// application must not be handed and a shell cannot work without. A taskbar, a switcher and a window
// menu are all this protocol plus a way to act on the answer.
//
// **Read-only, and the protocol's own minimalism is why it is the right first occupant.** There is no
// request here that changes anything: a client learns that a window exists, what it is called and what
// application it belongs to, and that is the end of the interface. Activating, closing and moving a
// window are window management, which
// [decision 51](../../Docs/Decisions.md#51-the-shell-is-a-per-session-client-gyro-owns-mechanism)
// makes a shell's and which needs the seam that does not exist yet — so enumeration lands first,
// alone, and nothing about it prejudges the protocol that will carry the verbs.
//
// **A window is per session and this list is per connection, so the enumeration is filtered by
// session.** [Decision 21](../../Docs/Decisions.md#21-many-sessions-connected-one-presented-locally)
// serves every user on the machine from one process, so `HostContext::Windows()` is *everybody's*
// windows — and handing one person's switcher the titles of another person's is the exact disclosure
// the per-uid split exists to prevent. Trust is not the check that catches it: a System client is
// trusted with its own session rather than with the machine, and the two rungs answer different
// questions. Under a socket gyro bound itself every client and every window is `SessionId::None`, so
// the comparison passes and a development run enumerates normally.
//
// **Nothing hooks the map and unmap paths, and the walk is a comparison instead.** `Sync` is called
// once per `Advance` and diffs the mapped windows against what each list has already been told, which
// is the shape [Shell.h](Shell.h)'s `SyncWindows` and the seat's focus comparison already have and is
// [decision
// 149](../../Docs/Decisions.md#149-focus-is-state-the-world-holds-and-the-seat-compares-against-it-rather-than-being-told)'s
// argument reused: the commit that mapped a window and the step that notices are the same wakeup of
// the same thread, so there is nothing to observe and no signal to keep in step. What it buys here is
// that `Shell.cpp` does not learn this protocol exists — a window's title changing is already a
// request handler, and a hook in it would be the second place that has to remember to fire.
//
// **The one thing the comparison cannot be late for is the bind**, which is why `OnBound` runs the
// same walk immediately. The protocol tells a client it may bind, roundtrip, and read the answer — and
// a roundtrip's `done` is queued by libwayland while the `bind` is being dispatched, so a first
// announcement deferred to the end of the iteration arrives *after* the reply that says the
// enumeration is complete. A taskbar written exactly as the protocol describes would come up empty and
// stay that way until the next window opened.
//
// **Advertised at version 1, which is the only version there is.** The protocol is staging and says so
// itself; a backward-incompatible change would arrive as a new interface name rather than a version
// bump, so there is no ceiling here to raise later.
inline constexpr std::uint32_t ForeignToplevelVersion = 1;

// A stable name for a window, for the `identifier` event.
//
// **It is the entity's generational handle, because that is already the machine's promise not to reuse
// a name.** The protocol asks for up to 32 printable ASCII bytes, unique per toplevel, never reused
// once the window unmaps, and recommends an opaque generation value in it —
// [Core/Handle.h](../Core/Handle.h) is that specification almost word for word, since an id that
// outlives what it named compares unequal to whatever takes the slot next. Sixteen hex digits of
// generation and index is the whole encoding.
//
// **Derived rather than stored, and that is what makes two lists agree.** The protocol requires every
// handle for one toplevel to carry the same identifier, so a counter minted per handle would be wrong
// and a counter minted per window would have to live on `ClientXdgSurface` — which would put a field
// for this protocol on the object that models xdg-shell. A function of the id needs no storage and
// cannot disagree with itself.
//
// The generation wraps after four billion retirements of one slot, which is the reuse the protocol
// forbids. It is not defended against here: at a window a second that is a hundred and thirty years of
// one slot, and the defence would be a counter with the storage problem above.
[[nodiscard]] std::string ForeignIdentifier(EntityId window);

// One `ext_foreign_toplevel_handle_v1`: one window, as one client has been told about it.
//
// **What it holds is what has been *said* rather than what is true**, which is the distinction
// [Shell.h](Shell.h) draws over `activated` and for the same reason: the world's copy of a title is the
// `xdg_toplevel`'s, and this is the last thing that went out on the wire, so a `title` event is owed
// exactly where the two disagree. Keeping only the fact would mean re-sending every title every
// iteration.
class ForeignToplevelHandle final : public Wayland::Server::ExtForeignToplevelHandleV1Handler
{
public:
	ForeignToplevelHandle(ForeignToplevelList& list, EntityId window) noexcept : m_List{ &list }, m_Window{ window } {}

	void OnGone() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	// The window this handle names, or null once it has been closed.
	[[nodiscard]] EntityId Window() const noexcept { return m_Window; }

	// Whether the window this named has gone. **A closed handle is inert rather than destroyed**: the
	// protocol has the client destroy it, and until it does the object has to exist to answer that
	// request — so it stays in the list and is skipped by everything that matches a window against one.
	[[nodiscard]] bool IsClosed() const noexcept { return m_Closed; }

	// Send the identifier, the title, the app id and the `done` that applies them, in the order the
	// protocol fixes. Called once, immediately after the `toplevel` event that introduced this object.
	void Introduce(const ClientXdgSurface& window);

	// Send whatever of the title and the app id has changed since the last time, and the `done` that
	// applies them — or nothing at all, which is every iteration of a window nobody is renaming.
	void Refresh(const ClientXdgSurface& window);

	// The window is gone: `closed`, and no further events ever.
	void Close();

	// The list went first, which happens when a client destroys its `ext_foreign_toplevel_list_v1`
	// while still holding handles — legal, and explicitly the order the protocol tells a client *not*
	// to use, so it has to be survived rather than assumed away.
	void Forget() noexcept { m_List = nullptr; }

private:
	ForeignToplevelList* m_List = nullptr;

	// Null once closed, so that a handle whose client has not got round to destroying it cannot match a
	// new window that happens to take the same slot.
	EntityId m_Window;

	// The last title and app id sent, which is what `Refresh` compares against. Empty is a real value
	// and not *unset*: a client that has never called `set_title` has no title, and the protocol has
	// nothing to say for that but the empty string.
	std::string m_Title;
	std::string m_AppId;

	bool m_Closed = false;
};

// One client's `ext_foreign_toplevel_list_v1`.
class ForeignToplevelList final : public Wayland::Server::ExtForeignToplevelListV1Handler
{
public:
	ForeignToplevelList(ForeignToplevelGlobal& global, HostContext& context) noexcept
		: m_Global{ &global }, m_Context{ &context }
	{}

	void OnGone() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	// The windows that already exist, announced before the client can ask whether there are any. See
	// the header comment: a roundtrip after the bind is what the protocol tells a client to do, and its
	// reply is already queued by the time this returns.
	void OnBound() override { Sync(); }

	// **`finished` goes out here rather than being deferred**, because the protocol's own sequence is
	// that a client sends `stop`, waits for `finished`, and only then destroys — so a compositor that
	// answered later would be one every client waits on. There is nothing gyro has to finish first: the
	// handles already sent stay valid and the event only promises that no `toplevel` follows.
	void OnStop() override;

	// Bring this client's idea of the windows up to date with the world's, sending nothing where
	// nothing has changed.
	void Sync();

	// A handle let go of its resource. Borrowed pointers throughout: a handle deregisters itself as it
	// dies, and this list tells each of them to forget it if it goes first.
	void Remove(ForeignToplevelHandle& handle) noexcept;

	// The global went first, which is the compositor shutting down rather than anything a client did.
	void Forget() noexcept { m_Global = nullptr; }

private:
	// The handle this list already has for a window, or null — closed handles never match, per
	// `ForeignToplevelHandle::IsClosed`.
	[[nodiscard]] ForeignToplevelHandle* HandleFor(EntityId window) const noexcept;

	ForeignToplevelGlobal* m_Global = nullptr;
	HostContext* m_Context = nullptr;

	// The handles minted on this list. Borrowed, and each removes itself as it dies.
	std::vector<ForeignToplevelHandle*> m_Handles;

	// Whether the client has asked to stop hearing about new windows. The handles it already has go on
	// being kept current: `stop` is about the `toplevel` event and says nothing about the rest.
	bool m_Stopped = false;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class ForeignToplevelGlobal final : public Wayland::Server::ExtForeignToplevelListV1Binding
{
public:
	explicit ForeignToplevelGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	~ForeignToplevelGlobal() override;

	Wayland::Server::ExtForeignToplevelListV1Handler* OnBind(wl_client& client, std::uint32_t version) override;

	// Every bound list brought up to date, called once per `Advance` from `ClientHost`. Costs one walk
	// over the mapped windows per list, and there are no lists at all on a machine with no shell — which
	// is every machine today, and is why it sits on a path that runs on every wakeup.
	void Sync();

	// A list came or went. Borrowed pointers, and the list deregisters itself from `OnGone`.
	void Add(ForeignToplevelList& list);
	void Remove(ForeignToplevelList& list) noexcept;

private:
	HostContext* m_Context = nullptr;

	// The lists bound on this machine. Borrowed, and each removes itself as it dies.
	std::vector<ForeignToplevelList*> m_Lists;
};
