#include "Drm/Refusal.h"

#include "Testing/Test.h"

// What two refusals have to differ in before a person is told twice.
//
// The whole of this is one question — *is this the same refusal I already reported* — and it was asked
// on a panel and nowhere else until the answer was wrong in the field: a pointer parked in the last few
// columns of a screen wrote a warning every frame, because the arrangement the display engine refused
// carried the composite's framebuffer id and gyro flips through a ring of them.

namespace
{
// A pointer-sized layer over a composite, which is the partition every one of these is about.
[[nodiscard]] RefusedProposal Pointer(std::uint32_t composite, std::int64_t x, int code = 22)
{
	RefusedProposal proposal{};
	proposal.Count = 2;
	proposal.Code = code;

	proposal.Layers[0] = RefusedLayer{ .Plane = 35,
		                               .Framebuffer = composite,
		                               .SrcW = 1920ULL << 16,
		                               .SrcH = 1080ULL << 16,
		                               .CrtcW = 1920,
		                               .CrtcH = 1080 };

	proposal.Layers[1] = RefusedLayer{ .Plane = 65,
		                               .Framebuffer = 648,
		                               .SrcW = 64ULL << 16,
		                               .SrcH = 64ULL << 16,
		                               .CrtcX = x,
		                               .CrtcY = 137,
		                               .CrtcW = 64,
		                               .CrtcH = 64 };

	return proposal;
}
} // namespace

// **The standing refusal, which is the case the report exists for.** Nothing moved and nothing
// resized; only the image gyro drew into changed, because it draws into a different one every frame.
GYRO_TEST(DrmRefusal, AStandingRefusalIsTheSameRefusalThroughTheFlipChain)
{
	GYRO_CHECK(Pointer(645, 1918).SameAs(Pointer(646, 1918)));
	GYRO_CHECK(Pointer(646, 1918).SameAs(Pointer(647, 1918)));
	GYRO_CHECK(Pointer(647, 1918).SameAs(Pointer(645, 1918)));
}

// **And the pair that is the diagnosis.** A refusal that moves, that lands on a different plane, or
// that comes back with a different errno is a second story and earns a second line.
GYRO_TEST(DrmRefusal, AProposalThatMovedIsADifferentRefusal)
{
	GYRO_CHECK(!Pointer(645, 1918).SameAs(Pointer(645, 1915)));
	GYRO_CHECK(!Pointer(645, 1918).SameAs(Pointer(645, 1918, 16)));

	RefusedProposal elsewhere = Pointer(645, 1918);
	elsewhere.Layers[1].Plane = 95;

	GYRO_CHECK(!Pointer(645, 1918).SameAs(elsewhere));
}

// **A narrower partition is a different one**, which is what the loop's narrowing walks through: the
// same layers with one given back to the composite is a proposal the driver has not answered yet.
GYRO_TEST(DrmRefusal, GivingALayerBackIsADifferentRefusal)
{
	RefusedProposal alone = Pointer(645, 1918);
	alone.Count = 1;

	GYRO_CHECK(!Pointer(645, 1918).SameAs(alone));
	GYRO_CHECK(!alone.SameAs(Pointer(645, 1918)));
}

// Nothing has been refused yet, on both sides — the state a fresh output is in, where *the same*
// has to mean *there is nothing to say* rather than a line about two empty proposals.
GYRO_TEST(DrmRefusal, TwoEmptyProposalsAreTheSame)
{
	GYRO_CHECK(RefusedProposal{}.SameAs(RefusedProposal{}));
}
