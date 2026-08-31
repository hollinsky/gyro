#include "Protocol/Tier.h"

#include <array>
#include <utility>

#include "Wayland/Server/LinuxDmabufV1.h"
#include "Wayland/Server/LinuxDrmSyncobjV1.h"
#include "Wayland/Server/PresentationTime.h"
#include "Wayland/Server/Viewporter.h"
#include "Wayland/Server/Wayland.h"
#include "Wayland/Server/XdgShell.h"

namespace
{
// Every interface gyro advertises, and the tier it sits in.
//
// **The names come from the bindings rather than from string literals here**, so a protocol whose wire
// name changes between one generation and the next cannot leave a row silently matching nothing. The
// linear walk is over eleven rows and runs once per client per global at registry bind, which is not a
// path anything waits on.
//
// **What is absent is as deliberate as what is present.** Docs/Architecture.md#filtered-globals also
// names the System tier's eventual occupants — layer-shell, foreign-toplevel management, screencopy,
// output configuration, the lock protocol — and none of them has a row, because `TierOf` already
// answers `System` for an interface it does not know. Writing them down would be choosing between
// `zwlr_` and `ext_` spellings for protocols nothing has been decided about, which is a decision this
// table has no business making on the way past.
constexpr std::array Table{
	// Shared: the vocabulary an application is written against.
	std::pair{ Wayland::Server::WlCompositor::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WlSubcompositor::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WlShm::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::ZwpLinuxDmabufV1::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WpLinuxDrmSyncobjManagerV1::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::XdgWmBase::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WpViewporter::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WpPresentation::WireName, GlobalTier::Shared },

	// Shared rather than System, and it is the one row worth arguing. A monitor is a fact an
	// application needs — which panel a window is on decides its scale and its refresh — and every
	// toolkit binds this. What Docs/Architecture.md puts in the System tier is *output configuration*,
	// which is a different protocol saying what a monitor should become rather than what it is.
	std::pair{ Wayland::Server::WlOutput::WireName, GlobalTier::Shared },

	// Session-scoped: advertised to everyone, reaching one session's own.
	std::pair{ Wayland::Server::WlDataDeviceManager::WireName, GlobalTier::Session },

	// The seat is not in Docs/Architecture.md's list and is put here rather than in `Shared`, because
	// what it carries is one session's keyboard, pointer and touchscreen — the keystrokes of whoever is
	// in front of the machine — and the tier is where that fact is recorded even while both rungs are
	// advertised alike.
	std::pair{ Wayland::Server::WlSeat::WireName, GlobalTier::Session },
};
} // namespace

GlobalTier TierOf(std::string_view interface) noexcept
{
	for (const auto& [name, tier] : Table)
	{
		if (name == interface)
		{
			return tier;
		}
	}

	return GlobalTier::System;
}
