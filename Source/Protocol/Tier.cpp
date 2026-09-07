#include "Protocol/Tier.h"

#include <array>
#include <string_view>
#include <utility>

#include "Wayland/Server/ExtForeignToplevelListV1.h"
#include "Wayland/Server/FractionalScaleV1.h"
#include "Wayland/Server/GyroBindingsV1.h"
#include "Wayland/Server/GyroChromeV1.h"
#include "Wayland/Server/GyroSceneV1.h"
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
// linear walk is over twelve rows and runs once per client per global at registry bind, which is not a
// path anything waits on.
//
// **What is absent is as deliberate as what is present.** Docs/Architecture.md#filtered-globals also
// names the System tier's eventual occupants — layer-shell, screencopy, output configuration, the lock
// protocol — and none of them has a row, because `TierOf` already answers `System` for an interface it
// does not know. Writing them down would be choosing between `zwlr_` and `ext_` spellings for
// protocols nothing has been decided about, which is a decision this table has no business making on
// the way past. What *is* written down is the one gyro serves: a row that says `System` and a missing
// row behave identically, and the difference is that one of them was chosen — which is the whole of
// what `Listed` reports and `Tier.Test.cpp` asks about.
constexpr std::array Table{
	// Shared: the vocabulary an application is written against.
	std::pair{ Wayland::Server::WlCompositor::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WlSubcompositor::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WlShm::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::ZwpLinuxDmabufV1::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WpLinuxDrmSyncobjManagerV1::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::XdgWmBase::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WpViewporter::WireName, GlobalTier::Shared },
	std::pair{ Wayland::Server::WpFractionalScaleManagerV1::WireName, GlobalTier::Shared },
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

	// System: a shell's, and an application must never be handed one.
	//
	// The windows on this machine, with their titles, belong to everybody's sessions at once —
	// Protocol/Foreign.h filters the enumeration down to the asking client's own, and the tier is what
	// decides whether it is asked at all.
	std::pair{ Wayland::Server::ExtForeignToplevelListV1::WireName, GlobalTier::System },

	// The keys a shell claims before the window in front of a person sees them, which is a shell's
	// authority stated as plainly as the protocol can state it: an application handed this could take
	// every keystroke on the machine out of every other application's reach and never be found.
	std::pair{ Wayland::Server::GyroBindingsV1::WireName, GlobalTier::System },

	// The surfaces the desktop is made of rather than the windows on it. An application handed this
	// could draw over every window on the machine, stay in front of the one a person clicks, and be in
	// no list that would let them find it and close it — which is the shape of a phishing overlay
	// stated as plainly as a protocol can state it.
	std::pair{ Wayland::Server::GyroChromeManagerV1::WireName, GlobalTier::System },

	// Where every window in a session goes, and what happens to it on the way. An application handed
	// this could move the window somebody is typing into onto a workspace they are not looking at, or
	// simply never place a new one and leave the machine looking as though it had stopped opening
	// windows at all.
	std::pair{ Wayland::Server::GyroSceneV1::WireName, GlobalTier::System },
};

// The row for an interface, or null where the table does not name it. The two callers below want
// opposite halves of the same lookup.
[[nodiscard]] const std::pair<std::string_view, GlobalTier>* Find(std::string_view interface) noexcept
{
	for (const auto& row : Table)
	{
		if (row.first == interface)
		{
			return &row;
		}
	}

	return nullptr;
}
} // namespace

GlobalTier TierOf(std::string_view interface) noexcept
{
	const auto* const row = Find(interface);

	return row == nullptr ? GlobalTier::System : row->second;
}

bool Listed(std::string_view interface) noexcept
{
	return Find(interface) != nullptr;
}
