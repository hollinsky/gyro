#include "Protocol/Foreign.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <vector>

#include "Core/Session.h"
#include "Protocol/Context.h"
#include "Protocol/Shell.h"

namespace
{
// Whether this surface is a window this protocol describes, and one this client is entitled to hear
// about.
//
// **Four questions and the last two are the ones that matter.** A popup is in the same registry as a
// window — `HostContext::Windows()` is every mapped `xdg_surface` — and the protocol is about
// toplevels, so a menu is not enumerated. An unmapped surface is not in the registry at all and the
// null check is belt and braces. The session comparison is [Foreign.h](Foreign.h)'s: one process
// serves every user on this machine, and a list must not describe a window belonging to somebody else.
//
// **And chrome is not a window** (187). The client most likely to be holding one of these lists is the
// same shell that drew the panel and the launcher, so without this the first thing a switcher shows a
// person is the switcher — and closing that entry is a shell being asked to close itself.
[[nodiscard]] bool Enumerable(const ClientXdgSurface& window, const HostContext& context, SessionId session) noexcept
{
	const ClientXdgToplevel* const toplevel = window.Toplevel();

	if (toplevel == nullptr || window.Window().IsNull() || toplevel->IsChrome())
	{
		return false;
	}

	return context.Session(window.Base().WireClient()) == session;
}
} // namespace

std::string ForeignIdentifier(EntityId window)
{
	// Generation first, so that two identifiers minted a moment apart differ in their leading digits —
	// a person reading a log or a `wayland-info` dump is comparing by eye, and two names differing in
	// the last character are two names that look the same.
	return std::format("{:08x}{:08x}", window.Generation, window.Index);
}

void ForeignToplevelHandle::OnGone()
{
	if (m_List != nullptr)
	{
		m_List->Remove(*this);
	}

	delete this;
}

void ForeignToplevelHandle::Introduce(const ClientXdgSurface& window)
{
	const ClientXdgToplevel* const toplevel = window.Toplevel();

	if (toplevel == nullptr || !Object().IsValid())
	{
		return;
	}

	m_Title = toplevel->Title();
	m_AppId = toplevel->AppId();

	// **`identifier` first and only ever here**, which the protocol states outright: it is sent when the
	// handle is created and never again, because it names the window rather than describing it.
	Object().Identifier(ForeignIdentifier(m_Window).c_str());
	Object().Title(m_Title.c_str());
	Object().AppId(m_AppId.c_str());
	Object().Done();
}

void ForeignToplevelHandle::Refresh(const ClientXdgSurface& window)
{
	const ClientXdgToplevel* const toplevel = window.Toplevel();

	if (m_Closed || toplevel == nullptr || !Object().IsValid())
	{
		return;
	}

	bool changed = false;

	if (toplevel->Title() != m_Title)
	{
		m_Title = toplevel->Title();

		Object().Title(m_Title.c_str());

		changed = true;
	}

	if (toplevel->AppId() != m_AppId)
	{
		m_AppId = toplevel->AppId();

		Object().AppId(m_AppId.c_str());

		changed = true;
	}

	// **One `done` for both, and none where neither moved.** The event is what applies a change, so a
	// pair that arrived together has to be applied together — a switcher that redrew between the title
	// and the app id would draw one frame of a window labelled with the wrong icon. And a window nobody
	// is renaming costs nothing at all, which is what puts this walk on a path that runs every wakeup.
	if (changed)
	{
		Object().Done();
	}
}

void ForeignToplevelHandle::Close()
{
	if (m_Closed)
	{
		return;
	}

	m_Closed = true;

	// **Null before the event rather than after**, so that nothing reachable from the client's own
	// destroy — which may run inside the flush this event is written into — can match this handle
	// against a window again.
	m_Window = {};

	if (Object().IsValid())
	{
		Object().Closed();
	}
}

void ForeignToplevelList::OnGone()
{
	// **The handles are told rather than destroyed.** They are the client's objects and it may still
	// destroy them in any order it likes — the protocol asks for the handles first and explicitly
	// survives a client that does not — so what dies here is this list's claim on them and nothing else.
	for (ForeignToplevelHandle* const handle : m_Handles)
	{
		handle->Forget();
	}

	if (m_Global != nullptr)
	{
		m_Global->Remove(*this);
	}

	delete this;
}

void ForeignToplevelList::OnStop()
{
	if (m_Stopped)
	{
		return;
	}

	m_Stopped = true;

	if (Object().IsValid())
	{
		Object().Finished();
	}
}

void ForeignToplevelList::Remove(ForeignToplevelHandle& handle) noexcept
{
	std::erase(m_Handles, &handle);
}

ForeignToplevelHandle* ForeignToplevelList::HandleFor(EntityId window) const noexcept
{
	const auto matches = [window](const ForeignToplevelHandle* const handle) noexcept {
		return !handle->IsClosed() && handle->Window() == window;
	};
	const auto at = std::find_if(m_Handles.begin(), m_Handles.end(), matches);

	return at != m_Handles.end() ? *at : nullptr;
}

void ForeignToplevelList::Sync()
{
	wl_client* const client = Object().IsValid() ? Object().WireClient() : nullptr;

	if (client == nullptr || m_Context == nullptr)
	{
		return;
	}

	const SessionId session = m_Context->Session(client);

	// Iterated over a copy for `SyncWindows`' reason: an event is a wire write, and a client whose
	// connection has already failed is torn down inside libwayland — which runs handlers that unmap
	// windows, and so edits the registry underneath this walk.
	const std::vector<ClientXdgSurface*> windows{ m_Context->Windows().begin(), m_Context->Windows().end() };

	// **The closures come first, because a slot can be reused inside one iteration.** A window retiring
	// and another opening in the same wakeup is ordinary — closing an application and clicking the next
	// one is two events a person produces in a moment — and the generation makes the two ids distinct,
	// so nothing is actually confused by the other order. What it would produce is a `toplevel` event
	// for the new window ahead of the `closed` for the old one, which reads on the far side as two
	// windows briefly existing.
	for (ForeignToplevelHandle* const handle : m_Handles)
	{
		if (handle->IsClosed())
		{
			continue;
		}

		const auto lives = [&](const ClientXdgSurface* const window) noexcept {
			return window->Window() == handle->Window() && Enumerable(*window, *m_Context, session);
		};

		if (std::none_of(windows.begin(), windows.end(), lives))
		{
			handle->Close();
		}
	}

	for (ClientXdgSurface* const window : windows)
	{
		if (!Enumerable(*window, *m_Context, session))
		{
			continue;
		}

		if (ForeignToplevelHandle* const existing = HandleFor(window->Window()); existing != nullptr)
		{
			existing->Refresh(*window);

			continue;
		}

		// **`stop` bounds the new windows and nothing else**, which is what the request says: a client
		// that has been sent `finished` goes on being told about the windows it already holds handles
		// for, because those are objects it is still using and the alternative is a switcher whose
		// labels freeze the moment it stops wanting new entries.
		if (m_Stopped)
		{
			continue;
		}

		auto* const handle = new ForeignToplevelHandle{ *this, window->Window() };

		// An id of zero is how a resource the client never asked for is made — the `new_id` in an event
		// is one the server picks — and the version is this list's own, since a handle is an extension of
		// the object that announced it and cannot be newer than what the client bound.
		const Wayland::Server::ExtForeignToplevelHandleV1 created =
			Wayland::Server::ExtForeignToplevelHandleV1::Create(*client, Object().Version(), 0, *handle);

		if (!created.IsValid())
		{
			// `Create` has already ended the client, and nothing adopted the handler. Deleted directly
			// rather than through `OnGone`, which is the bind path's contract and not this one's: this
			// object was never in the list to be taken out of it.
			delete handle;

			return;
		}

		m_Handles.push_back(handle);

		Object().Toplevel(created);

		handle->Introduce(*window);
	}
}

ForeignToplevelGlobal::~ForeignToplevelGlobal()
{
	// The host is going away, which happens with the display still up in a test and after it in a run.
	// Either way a list that outlived this object would call back into freed memory from its `OnGone`.
	for (ForeignToplevelList* const list : m_Lists)
	{
		list->Forget();
	}
}

Wayland::Server::ExtForeignToplevelListV1Handler*
ForeignToplevelGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	auto* const list = new ForeignToplevelList{ *this, *m_Context };

	// **Registered before the bind returns, so that `OnBound`'s first walk is not a special case.**
	// libwayland calls `OnBound` immediately after this handler is bound to its resource, and that call
	// is what announces the windows already open — see [Foreign.h](Foreign.h) for why it cannot wait for
	// the end of the iteration.
	Add(*list);

	return list;
}

void ForeignToplevelGlobal::Sync()
{
	// Over a copy, because a list whose client has failed is destroyed inside the write below.
	const std::vector<ForeignToplevelList*> lists{ m_Lists.begin(), m_Lists.end() };

	for (ForeignToplevelList* const list : lists)
	{
		list->Sync();
	}
}

void ForeignToplevelGlobal::Add(ForeignToplevelList& list)
{
	m_Lists.push_back(&list);
}

void ForeignToplevelGlobal::Remove(ForeignToplevelList& list) noexcept
{
	std::erase(m_Lists, &list);
}
