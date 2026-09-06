#include "Protocol/Chrome.h"

#include "Protocol/Shell.h"

void ClientChrome::OnGone()
{
	if (m_Toplevel != nullptr)
	{
		m_Toplevel->ForgetChrome();
	}

	delete this;
}

void ClientChrome::OnDestroy()
{
	// **The resource is still alive here**, which is what makes the error postable at all: the generated
	// dispatch calls this and then destroys the object, so a destructor request is the last moment a
	// compositor can object to itself being destroyed.
	if (m_Toplevel != nullptr && m_Toplevel->IsMapped())
	{
		Object().PostError(
			Wayland::Server::GyroChromeV1Error::Mapped, "gyro_chrome_v1.destroy on a toplevel that is mapped"
		);
	}
}

void ClientChrome::OnSetMaterial(Wayland::Server::GyroChromeV1Material material)
{
	const std::optional<Material> dress = MaterialOf(material);

	if (!dress)
	{
		Object().PostError(
			Wayland::Server::GyroChromeV1Error::BadMaterial,
			"gyro_chrome_v1.set_material with a material gyro has no name for"
		);

		return;
	}

	// **Nothing happens where the toplevel has already gone**, rather than the client's connection
	// ending: it broke the destroy ordering, and it is about to destroy this object too. Ending it over
	// a request that changes nothing would turn a client's own tidy-up bug into a desktop disappearing.
	if (m_Toplevel != nullptr)
	{
		m_Toplevel->StageMaterial(*dress);
	}
}

Wayland::Server::GyroChromeV1Handler* ChromeManager::OnGetChrome(Wayland::Server::XdgToplevel toplevel)
{
	// **Every refusal below still returns an object**, which is `xdg_surface.get_popup`'s rule in this
	// same tree: the client has already spent an id and libwayland needs something behind it. A chrome
	// object with no toplevel accepts `set_material` and does nothing with it, and the connection is over
	// in any case — `PostError` is what ends it.
	auto* const window = static_cast<ClientXdgToplevel*>(toplevel.Implementation());

	// Not a client error and not reachable from a well-formed connection: libwayland type-checks an
	// argument declared as an `xdg_toplevel` before this runs, so the only way here is a toplevel whose
	// handler has already gone — and there is nothing left to declare chrome.
	if (window == nullptr)
	{
		return new ClientChrome{ nullptr };
	}

	// **Before the first map rather than at any time**, because what this changes is decided when the
	// surface enters the world: which root it hangs on, whether the walk visits it, whether it is
	// enumerated. A toplevel already on screen would have to be moved between roots and out of two
	// lists while a person was looking at it.
	if (window->IsMapped())
	{
		Object().PostError(
			Wayland::Server::GyroChromeManagerV1Error::AlreadyMapped,
			"gyro_chrome_manager_v1.get_chrome on a toplevel that has already been mapped"
		);

		return new ClientChrome{ nullptr };
	}

	auto* const chrome = new ClientChrome{ window };

	if (!window->BecomeChrome(*chrome))
	{
		Object().PostError(
			Wayland::Server::GyroChromeManagerV1Error::AlreadyChrome,
			"gyro_chrome_manager_v1.get_chrome on a toplevel that already has a chrome object"
		);

		// Unlinked rather than left pointing at a toplevel whose own link names the *first* chrome
		// object: the toplevel clears one pointer when it goes, and a second holder of it would be the
		// dangling this whole pair of back links exists to prevent.
		chrome->Forget();
	}

	return chrome;
}

Wayland::Server::GyroChromeManagerV1Handler* ChromeGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	// **The global holds nothing and the manager holds nothing**, so neither is registered anywhere and
	// dropping either changes nothing about a surface already declared. What a surface is, is on the
	// surface — see `ClientXdgToplevel::IsChrome`.
	return new ChromeManager{};
}
