#include "Nested/Host.h"

#include <cerrno>
#include <optional>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Nested/Peer.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"
#include "Wayland/LinuxDmabufV1.h"
#include "Wayland/PresentationTime.h"
#include "Wayland/Wayland.h"
#include "Wayland/XdgShell.h"

// The connection coming up, against the host Nested/Peer.h plays.
//
// **What this file is about is the *refusals*.** A host that comes up is the case a real session
// covers on every run; the cases nobody meets until they meet them are the ones a compositor cannot
// be asked to produce — a `zwp_linux_dmabuf_v1` a version too old to have feedback, a set with no
// format in it, no syncobj protocol at all — and each of those decides how the whole session behaves
// afterwards. Getting a sentence out of them rather than a stall is the difference between somebody
// switching desktops and somebody reading a stack trace.

namespace
{
using namespace Wayland;

// The host and the connection, brought up together. Held as a pair because the peer has to be pumped
// between the client's requests and its own answers, and a test that forgot would hang on the
// roundtrip's timeout rather than fail.
struct Session
{
	Nested::Peer Host;
	MonotonicClock Clock;
	Nested::NestedHost Client{ Clock };

	[[nodiscard]] Result<void> Open()
	{
		if (const Result<void> opened = Host.Open(); !opened)
		{
			return opened;
		}

		// `Open` roundtrips twice — the globals, then the feedback — and both are questions this peer
		// is the answer to, so it runs on a thread for the duration. See `PumpWhile`.
		Result<void> result{};

		Nested::PumpWhile(Host, [&] { result = Client.Open(); });

		return result;
	}
};
} // namespace

GYRO_TEST(NestedHost, AHostThatOffersEverythingComesUp)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());

	// Every global a nested output drives, bound at what both ends know rather than at what the
	// bindings describe: the peer advertises `wl_compositor` 6 and the generated proxy knows 7.
	GYRO_CHECK(session.Client.Globals().Compositor.IsValid());
	GYRO_CHECK_EQ(session.Client.Globals().Compositor.Version(), std::uint32_t{ 6 });
	GYRO_CHECK(session.Client.Globals().Shell.IsValid());
	GYRO_CHECK(session.Client.Globals().Dmabuf.IsValid());
	GYRO_CHECK(session.Client.Globals().Presentation.IsValid());
	GYRO_CHECK(session.Client.Globals().Syncobj.IsValid());

	// The feedback arrived whole, which is what nothing can be allocated without.
	GYRO_CHECK(!session.Client.Support().IsEmpty());
	GYRO_CHECK_EQ(session.Client.Support().MainDevice, std::uint64_t{ 0xe280 });

	// `wp_presentation.clock_id` said CLOCK_MONOTONIC, so decision 57's conversion has something to
	// convert from and a timestamp may be called precise.
	GYRO_CHECK(session.Client.IsMonotonic());

	// And the peer met nothing it did not model, which is what keeps its silence from being agreement.
	GYRO_CHECK_EQ(session.Host.Unhandled, std::uint64_t{ 0 });
}

GYRO_TEST(NestedHost, AMissingGlobalIsASentenceRatherThanAStall)
{
	// The failure somebody actually meets: a host with no `wp_presentation`. Without it there is no
	// clock, and a nested output would present frames it could never learn the timing of — so it is a
	// refusal at startup rather than a session that runs blind.
	Session session;

	session.Host.Globals = { Nested::PeerGlobal{ WlCompositor::WireName, 6 },
		                     Nested::PeerGlobal{ XdgWmBase::WireName, 6 },
		                     Nested::PeerGlobal{ ZwpLinuxDmabufV1::WireName, 5 } };

	const Result<void> opened = session.Open();

	GYRO_REQUIRE(!opened.has_value());
	GYRO_CHECK_EQ(opened.error().Code(), ENOPROTOOPT);
}

GYRO_TEST(NestedHost, ADmabufTooOldForFeedbackIsRefusedByVersionRatherThanByUse)
{
	// `get_default_feedback` arrived in version 4. Below it there are only the flat `format` and
	// `modifier` events, which say what the host *can* import and not what it prefers or which device
	// it wants the import on — and decision 120 rests on being told the constraint rather than
	// inferring it. Caught at the bind rather than three requests later, so the message names the
	// version.
	Session session;

	session.Host.Globals = { Nested::PeerGlobal{ WlCompositor::WireName, 6 },
		                     Nested::PeerGlobal{ XdgWmBase::WireName, 6 },
		                     Nested::PeerGlobal{ ZwpLinuxDmabufV1::WireName, 3 },
		                     Nested::PeerGlobal{ WpPresentation::WireName, 1 } };

	const Result<void> opened = session.Open();

	GYRO_REQUIRE(!opened.has_value());
	GYRO_CHECK_EQ(opened.error().Code(), ENOPROTOOPT);
}

GYRO_TEST(NestedHost, AHostThatOffersNothingUsableIsRefused)
{
	// Every global present, the feedback conversation complete, and nothing in it. The distinction
	// matters because it is not a protocol failure: the host answered correctly and has nothing gyro
	// can import, which is a different sentence from a missing interface — and it is where a session
	// stops rather than a place it limps on from, since there is no format to fall back to.
	//
	// A host with an offer gyro cannot *use* — one that takes only `NV12` — is a different case again,
	// and it is refused one layer up where a target set is built. Nested/Output.Test.cpp has it.
	Session session;
	session.Host.OfferNothing = true;

	const Result<void> opened = session.Open();

	GYRO_REQUIRE(!opened.has_value());
	GYRO_CHECK_EQ(opened.error().Code(), ENOPROTOOPT);
}

GYRO_TEST(NestedHost, AHostWithNoSyncobjProtocolStillComesUp)
{
	// The fallback path's precondition. A host without `wp_linux_drm_syncobj_v1` is perfectly usable —
	// most were until recently — and refusing one would make gyro unrunnable on the desktop somebody
	// has. What it costs is stated in the log and measured in Output.Test.cpp.
	Session session;

	session.Host.Globals = { Nested::PeerGlobal{ WlCompositor::WireName, 6 },
		                     Nested::PeerGlobal{ XdgWmBase::WireName, 6 },
		                     Nested::PeerGlobal{ ZwpLinuxDmabufV1::WireName, 5 },
		                     Nested::PeerGlobal{ WpPresentation::WireName, 1 } };

	GYRO_REQUIRE(session.Open().has_value());

	GYRO_CHECK(!session.Client.Globals().Syncobj.IsValid());
	GYRO_CHECK(!session.Client.HasExplicitSync());
}

GYRO_TEST(NestedHost, AHostThatHangsUpEndsTheDrainRatherThanSpinning)
{
	// The loop drains every source every iteration and does not act on the result — decision 80 —
	// so a socket at end of file would otherwise be read forever, once per wakeup, for the life of the
	// process. `EPIPE` is what the composition root reads to know the session is over.
	Session session;

	GYRO_REQUIRE(session.Open().has_value());

	session.Host.HangUp();

	const Result<void> drained = session.Client.Drain();

	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPIPE);

	// And it is latched, so every verb afterwards says the same thing rather than trying again.
	GYRO_CHECK(session.Client.Connection().Failed().has_value());
}
