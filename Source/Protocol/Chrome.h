#pragma once

#include <cstdint>
#include <optional>

#include "Wayland/Server/GyroChromeV1.h"
#include "World/Material.h"

class ClientXdgToplevel;

// `gyro_chrome_v1`: the surfaces a shell draws, which are not windows.
//
// **What this protocol is for is one declaration rather than several.** A shell's launcher differs
// from an application's window in four ways — it draws above every window, it is not enumerated, an
// `Alt+Tab` never lands on it, and it can be made of glass — and a protocol that offered those as four
// independent requests would be one a shell forgets a member of. What a person would see if it did is
// their own launcher listed in the switcher the same shell is drawing, or a panel that fell behind the
// window they just clicked.
//
// **It is not layer-shell, and the difference is decision 51.** `zwlr_layer_shell` and `ext_layer_shell`
// carry anchors, exclusive zones and keyboard-interactivity modes, which are the window management
// decision 51 hands to a shell — a compositor implementing them is implementing a layout policy on the
// shell's behalf. What is here instead is the part that has to be the compositor's because only the
// compositor can be it: where a surface sits in the world, whether it is a window, and what it is made
// of. Placement is deliberately absent and Docs/Open.md carries what that costs a panel.
//
// **It is an object on an `xdg_toplevel` rather than a role of its own**, which is `xdg_decoration`'s
// shape and is what keeps the mapping handshake singular. A role would mean a second configure
// sequence, a second acknowledgement and a second set of ways to get it wrong, for a surface that
// wants exactly what `xdg_toplevel` already does: be told to pick its own size, and be told when it
// has the keyboard.
//
// **A material is a name and never a number**, which is decision 33 seen from the client's side: the
// compositor is the one party that can make every glass surface on the machine look like the same
// glass at the same moment, across two renderers and whatever the device in front of a person will
// actually do. A shell able to ask for a 32 pixel blur is a shell whose panel stops matching the
// notification beside it the first time either is edited.

inline constexpr std::uint32_t ChromeVersion = 1;

// The wire material as the world's, or nothing where the client named one gyro does not know.
//
// **A `std::optional` rather than a fall back to `None`**, because the two are different facts about
// the client: a shell that asked for a material this compositor has never heard of has been built
// against a newer protocol than the one it bound, and drawing nothing would leave it looking correct
// to itself and wrong on screen. The protocol makes it an error, so this returns nothing and the
// caller posts one.
[[nodiscard]] constexpr std::optional<Material> MaterialOf(Wayland::Server::GyroChromeV1Material material) noexcept
{
	switch (material)
	{
		case Wayland::Server::GyroChromeV1Material::None:
			return Material::None;
		case Wayland::Server::GyroChromeV1Material::Glass:
			return Material::Glass;
		case Wayland::Server::GyroChromeV1Material::Smoke:
			return Material::Smoke;
	}

	return std::nullopt;
}

// One `gyro_chrome_v1`: a toplevel the shell declared to be chrome, and the object its material is set
// through.
//
// **It holds the toplevel and the toplevel holds it, and both links are cleared by whichever goes
// first.** The protocol says this object must be destroyed before the `xdg_toplevel` it was made from,
// and a client is perfectly able to do the opposite — libwayland will destroy objects in whatever
// order the client asks, and an ordering rule the compositor enforces by dereferencing is a crash
// rather than an enforcement.
class ClientChrome final : public Wayland::Server::GyroChromeV1Handler
{
public:
	explicit ClientChrome(ClientXdgToplevel* toplevel) noexcept : m_Toplevel{ toplevel } {}

	void OnGone() override;

	// **The one destroy in gyro's own protocols that can fail.** Chrome is fixed for the life of a
	// surface, so there is no state to give a mapped one back to: a launcher that stopped being chrome
	// while it was on screen would fall behind the windows and appear in the switcher, in the middle of
	// a person looking at it. The resource is still alive when this runs, so the error goes out on it.
	void OnDestroy() override;

	void OnSetMaterial(Wayland::Server::GyroChromeV1Material material) override;

	// The toplevel went away first. Called by `ClientXdgToplevel`'s own teardown, and after it every
	// request here is answered by doing nothing — the client has already broken the ordering rule and
	// ending its connection over an object it is about to destroy anyway would be a worse answer than
	// ignoring it.
	void Forget() noexcept { m_Toplevel = nullptr; }

private:
	ClientXdgToplevel* m_Toplevel = nullptr;
};

// The `gyro_chrome_manager_v1` a client bound: a factory and nothing else.
//
// It holds no state at all, which is what makes dropping it harmless — a shell that declares its panel
// chrome and then destroys the manager keeps a panel that is chrome, exactly as a shell that drops
// `gyro_bindings_v1` keeps the chords it claimed (186). That rule is stated in both protocols because
// getting it wrong is silent in both.
class ChromeManager final : public Wayland::Server::GyroChromeManagerV1Handler
{
public:
	void OnGone() override { delete this; }

	void OnDestroy() override {}

	Wayland::Server::GyroChromeV1Handler* OnGetChrome(Wayland::Server::XdgToplevel toplevel) override;
};

// The global. One per compositor, System tier, so an application is never offered it — a client able
// to declare itself chrome could draw over every window on the machine and be in no window list.
class ChromeGlobal final : public Wayland::Server::GyroChromeManagerV1Binding
{
public:
	Wayland::Server::GyroChromeManagerV1Handler* OnBind(wl_client& client, std::uint32_t version) override;
};
