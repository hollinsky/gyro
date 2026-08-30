#include "Protocol/Presentation.h"

#include "Protocol/Context.h"
#include "Protocol/Surface.h"
#include "Testing/Test.h"

// Where a feedback waits, and what moves it.
//
// **A feedback here has no `wl_resource` behind it**, which is `Surface.Test.cpp`'s arrangement and
// buys the same thing: what these cases are about is the staging, and the staging is a fact about the
// two lists rather than about any event going out. What it costs is the half that *sends* — a stale
// resource swallows `presented` and `discarded` alike — so the events themselves are asserted in
// Integration/ProtocolRoundTrip.Test.cpp, where a real client receives them and can be asked what it
// got.
//
// That split is not incidental to this protocol. A feedback's whole observable behaviour is one event
// at one moment, so a test that could not read the event would be testing nothing; and the moment is
// decided here, so a test that could only read the event would not say *why* it arrived. Both halves
// exist because the two failures — a feedback answered for the wrong frame, and a feedback answered
// with the wrong numbers — look identical from a client and have nothing to do with each other.

GYRO_TEST(Presentation, TheVersionIsTheOneWithNoVariableRefreshPromiseInIt)
{
	// 2 would let gyro pick a representative rate for an output with no constant refresh, and there is
	// no such output in `Scene/Output.h` to pick one for. The number goes up in the commit that builds
	// the thing it names.
	GYRO_CHECK_EQ(PresentationVersion, std::uint32_t{ 1 });
}

GYRO_TEST(Presentation, AFeedbackWaitsForTheCommitItIsAbout)
{
	HostContext context;
	ClientSurface surface{ context };

	auto* const feedback = new ClientPresentationFeedback{ context, &surface };

	surface.AdoptFeedback(*feedback);

	// **Asked for before the content update it describes**, exactly as `wl_surface.frame` is: a client
	// requests the feedback and then attaches the pixels it is about. Nothing is owed until the commit
	// that carries them.
	GYRO_CHECK_EQ(surface.DueFeedbackCount(), std::size_t{ 0 });

	surface.OnCommit();

	GYRO_CHECK_EQ(surface.DueFeedbackCount(), std::size_t{ 1 });

	// The client disconnecting is what runs this, and the surface must not be left holding a resource
	// it means to send an event to. There is no way to observe that from the outside other than the
	// count, which is the point of the object removing itself rather than being swept.
	delete feedback;

	GYRO_CHECK_EQ(surface.DueFeedbackCount(), std::size_t{ 0 });
}

GYRO_TEST(Presentation, ASecondCommitDoesNotCarryTheFirstCommitsFeedback)
{
	HostContext context;
	ClientSurface surface{ context };

	auto* const first = new ClientPresentationFeedback{ context, &surface };
	auto* const second = new ClientPresentationFeedback{ context, &surface };

	surface.AdoptFeedback(*first);
	surface.OnCommit();
	surface.AdoptFeedback(*second);
	surface.OnCommit();

	// **One, not two, and this is where `wp_presentation_feedback` parts from `wl_surface.frame`.**
	// Callbacks accumulate, because *you may draw again* stays true however many commits a client made
	// inside one refresh. A feedback is about one content update, and the second commit is what
	// superseded the first — so the first is discarded rather than answered with a timestamp belonging
	// to pixels nobody ever saw.
	GYRO_CHECK_EQ(surface.DueFeedbackCount(), std::size_t{ 1 });

	delete second;
}
