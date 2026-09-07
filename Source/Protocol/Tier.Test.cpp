#include "Protocol/Tier.h"

#include <array>
#include <string_view>
#include <utility>

#include "Testing/Test.h"
#include "Wayland/Server/ExtForeignToplevelListV1.h"
#include "Wayland/Server/FractionalScaleV1.h"
#include "Wayland/Server/GyroBindingsV1.h"
#include "Wayland/Server/LinuxDmabufV1.h"
#include "Wayland/Server/LinuxDrmSyncobjV1.h"
#include "Wayland/Server/PresentationTime.h"
#include "Wayland/Server/Viewporter.h"
#include "Wayland/Server/Wayland.h"
#include "Wayland/Server/XdgShell.h"

// **The table's default is a refusal, so what has to be tested is the absence of a row.** `TierOf`
// answers `System` for an interface it does not know, which is the safe direction and is also the
// direction that fails at runtime rather than at review: a global somebody advertised and forgot to
// list would simply stop appearing in every client's registry. The list below is that catch, and it is
// deliberately a second copy of the interfaces `Protocol/Host.cpp` and `Protocol/Output.cpp`
// advertise — a copy that has to be updated in the same commit as the global, and says so by failing.
//
// **The question it asks is `Listed` rather than `Visible`, and the first System-tier global is what
// changed that.** While every global was an application's, asking whether an interface reaches a
// `User` client caught a forgotten row for free: an omission answers `System` and so did nothing else.
// `ext_foreign_toplevel_list_v1` answers `System` on purpose, so the visibility form would have to
// carry an exception for it — and a walk with an exception list is one that stops catching the case it
// exists for the moment somebody adds the second entry to it.

namespace
{
// Every interface gyro puts in front of a client today. Adding a global without adding it here is a
// test that passes and a compositor that hides it; adding it here without a row in the table is this
// test failing with the interface named.
constexpr std::array Advertised{
	Wayland::Server::WlCompositor::WireName,
	Wayland::Server::WlSubcompositor::WireName,
	Wayland::Server::WlShm::WireName,
	Wayland::Server::ZwpLinuxDmabufV1::WireName,
	Wayland::Server::WpLinuxDrmSyncobjManagerV1::WireName,
	Wayland::Server::XdgWmBase::WireName,
	Wayland::Server::WpViewporter::WireName,
	Wayland::Server::WpFractionalScaleManagerV1::WireName,
	Wayland::Server::WpPresentation::WireName,
	Wayland::Server::WlOutput::WireName,
	Wayland::Server::WlDataDeviceManager::WireName,
	Wayland::Server::WlSeat::WireName,
	Wayland::Server::ExtForeignToplevelListV1::WireName,
	Wayland::Server::GyroBindingsV1::WireName,
};
} // namespace

GYRO_TEST(Tier, EveryGlobalGyroAdvertisesSitsInATierSomebodyChose)
{
	for (const std::string_view interface : Advertised)
	{
		// Reported through `GYRO_FAIL` rather than `GYRO_CHECK` so the interface is *named*: whoever added
		// the global needs to be told which one it was, and the loop variable is what a stringified
		// expression would print instead.
		if (!Listed(interface))
		{
			GYRO_FAIL(interface);
		}
	}
}

GYRO_TEST(Tier, AnInterfaceWithNoRowIsRefusedToAnApplication)
{
	// The spelling is deliberately one of the System tier's eventual occupants
	// (Docs/Architecture.md#filtered-globals) rather than nonsense, because that is the case the default
	// exists for: a protocol gyro has not written yet must not become visible by being forgotten.
	GYRO_CHECK(!Listed("zwlr_layer_shell_v1"));
	GYRO_CHECK(TierOf("zwlr_layer_shell_v1") == GlobalTier::System);
	GYRO_CHECK(!Visible(TierOf("zwlr_layer_shell_v1"), Trust::User));
	GYRO_CHECK(Visible(TierOf("zwlr_layer_shell_v1"), Trust::System));
}

GYRO_TEST(Tier, TheWindowListIsTheShellsAndNotAnApplications)
{
	// The first occupant of the tier, and the row that makes `Listed` a different question from
	// `TierOf`: it is `System` because somebody wrote it down, and an application asking for the titles
	// of every window in the session is refused for that reason rather than by omission.
	GYRO_CHECK(Listed(Wayland::Server::ExtForeignToplevelListV1::WireName));
	GYRO_CHECK(TierOf(Wayland::Server::ExtForeignToplevelListV1::WireName) == GlobalTier::System);

	GYRO_CHECK(!Visible(TierOf(Wayland::Server::ExtForeignToplevelListV1::WireName), Trust::User));
	GYRO_CHECK(Visible(TierOf(Wayland::Server::ExtForeignToplevelListV1::WireName), Trust::System));
}

GYRO_TEST(Tier, TheKeysAShellClaimsAreNotAnApplicationsToTake)
{
	// The second occupant, and the one where the tier is doing the most work. An application handed this
	// could claim a chord and take it out of every other application's reach across the whole machine —
	// and unlike a window list, nothing it does with the keystroke is visible to the person whose key
	// went missing.
	GYRO_CHECK(Listed(Wayland::Server::GyroBindingsV1::WireName));
	GYRO_CHECK(TierOf(Wayland::Server::GyroBindingsV1::WireName) == GlobalTier::System);

	GYRO_CHECK(!Visible(TierOf(Wayland::Server::GyroBindingsV1::WireName), Trust::User));
	GYRO_CHECK(Visible(TierOf(Wayland::Server::GyroBindingsV1::WireName), Trust::System));
}

GYRO_TEST(Tier, TheSeatAndTheDataDeviceAreSessionScopedAndStillAdvertised)
{
	// The rung that is easiest to get wrong, because it reads like a restriction and is not: session
	// scoping is about what a global's objects reach rather than about who may bind it, and an
	// application with no clipboard and no keyboard would be the cost of confusing the two.
	GYRO_CHECK(TierOf(Wayland::Server::WlSeat::WireName) == GlobalTier::Session);
	GYRO_CHECK(TierOf(Wayland::Server::WlDataDeviceManager::WireName) == GlobalTier::Session);

	GYRO_CHECK(Visible(GlobalTier::Session, Trust::User));
	GYRO_CHECK(Visible(GlobalTier::Shared, Trust::User));
}

GYRO_TEST(Tier, ASystemClientSeesEverything)
{
	// A shell binds `wl_compositor` like anything else. The tier gates one direction only.
	GYRO_CHECK(Visible(GlobalTier::Shared, Trust::System));
	GYRO_CHECK(Visible(GlobalTier::Session, Trust::System));
	GYRO_CHECK(Visible(GlobalTier::System, Trust::System));
}
