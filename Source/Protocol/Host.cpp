#include "Protocol/Host.h"

#include <cerrno>
#include <utility>

Result<void> ClientHost::Open(SceneStore& scene, ITextures& textures)
{
	// Neither is touched here: a scene made of client windows starts with no clients in it, and both
	// arrive again on every `Advance` — which is where a request that needs them runs. Named rather
	// than dropped so the signature reads as the contract it implements.
	(void)scene;
	(void)textures;

	wl_display* const display = m_Server.Display();

	if (display == nullptr)
	{
		return Failure(EBADF, "advertising globals on a Wayland server that was never opened");
	}

	m_CompositorGlobal = Wayland::Server::WlCompositor::Advertise(*display, CompositorVersion, m_Compositor);

	if (m_CompositorGlobal == nullptr)
	{
		// A compositor with no `wl_compositor` is a socket clients connect to and cannot use, which is
		// worse than one that refused to start: a person sees applications failing to open with no
		// message anywhere that says why. So this is fatal rather than degraded.
		return Failure(ENOMEM, "advertising wl_compositor");
	}

	m_ShmGlobal = Wayland::Server::WlShm::Advertise(*display, ShmVersion, m_Shm);

	if (m_ShmGlobal == nullptr)
	{
		// Fatal for `wl_compositor`'s reason and one more: `wl_shm` is the only way a client hands over
		// pixels at all, so a compositor without it is one every application connects to and then hangs
		// against, with nothing on screen and nothing in a log to explain it.
		return Failure(ENOMEM, "advertising wl_shm");
	}

	return {};
}

Wake ClientHost::Advance(SceneStore& scene, ITextures& textures, Instant now)
{
	(void)scene;
	(void)now;

	// The world, reachable for exactly the length of this call. Every request below runs inside the
	// dispatch, so a `wl_surface.commit` finds the texture space on the stack rather than in a
	// reference this object had to keep — which is the arrangement `ISceneAuthor` is shaped for and
	// [Context.h](Context.h) carries the argument for.
	const HostContext::Dispatching dispatching{ m_Context, textures };

	// **A failed dispatch is swallowed here and cannot be otherwise**, which is `ISceneAuthor`'s shape
	// rather than an omission: `Advance` runs on every wake and returns no `Result`, because a failure
	// on that path is one nothing is in a position to act on. What `Server::Poll` calls a failure is the
	// event loop itself faulting — a client behaving badly is ended inside libwayland and never arrives
	// here — so the reachable case is a broken descriptor, and the answer to that is the same as the
	// answer to no clients at all: author nothing and wait.
	[[maybe_unused]] const Result<void> polled = m_Server.Poll();

	return Wake::Never();
}

Result<std::unique_ptr<ClientHost>> MakeClientHost(std::string_view socket)
{
	auto host = std::make_unique<ClientHost>();

	if (const Result<void> opened = host->Listen(socket); !opened)
	{
		return std::unexpected{ opened.error() };
	}

	return host;
}
