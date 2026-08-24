#include "Protocol/Host.h"

#include <utility>

Result<void> ClientHost::Open(SceneStore& scene, ITextures& textures)
{
	// Neither is touched until there is a global to reach them from. Named rather than dropped so the
	// signature reads as the contract it implements.
	(void)scene;
	(void)textures;

	return {};
}

Wake ClientHost::Advance(SceneStore& scene, ITextures& textures, Instant now)
{
	(void)scene;
	(void)textures;
	(void)now;

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
