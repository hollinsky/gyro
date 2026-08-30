#include "Protocol/Subcompositor.h"

#include <cstddef>

#include "Protocol/Context.h"
#include "Protocol/Surface.h"
#include "Testing/Test.h"

// The commit behaviour, which is the whole of what `wl_subsurface` adds to a `wl_surface`.
//
// **A surface here has no `wl_resource` behind it**, which is Protocol/Surface.Test.cpp's arrangement
// and the same trade: the caching is observable directly — `Current()` before and after somebody
// else's commit — where a client can only ever see the applied side. What it costs is everything that
// needs an object to name: `place_above` resolves a `wl_surface` the client sent, and a subsurface
// does not reach the world until a buffer does. Both are Integration/ProtocolRoundTrip.Test.cpp's,
// over a real socket, where a real toolkit's sequence is what is being checked.

namespace
{
// A parent, a child, and the role that joins them. Declared in this order deliberately: the role goes
// away before either surface does, so it takes itself out of the parent's runs rather than the parent
// reaching a dead one.
struct Pair
{
	HostContext Context;
	ClientSurface Parent{ Context };
	ClientSurface Child{ Context };
	ClientSubsurface Role{ Context, &Child, &Parent };

	Pair()
	{
		static_cast<void>(Child.AdoptRole(Role));

		Parent.AddChild(Role);
	}
};
} // namespace

GYRO_TEST(Subcompositor, ASubsurfaceIsSynchronizedUntilItSaysOtherwise)
{
	Pair pair;

	// The protocol's default, and the one a toolkit almost always leaves alone: what a client is buying
	// is that its parts arrive together rather than a frame apart.
	GYRO_CHECK(pair.Role.IsSynchronized());

	pair.Role.OnSetDesync();

	GYRO_CHECK(!pair.Role.IsSynchronized());
}

GYRO_TEST(Subcompositor, ASynchronizedCommitChangesNothingUntilTheParentCommits)
{
	Pair pair;

	pair.Child.OnSetBufferScale(2);
	pair.Child.OnCommit();

	// The child has committed and the world has not moved. This is the case a video player and the
	// controls over it depend on: the two are stated separately and land in one frame.
	GYRO_CHECK_EQ(pair.Child.Current().BufferScale, 1);

	pair.Parent.OnCommit();

	GYRO_CHECK_EQ(pair.Child.Current().BufferScale, 2);
}

GYRO_TEST(Subcompositor, ADesynchronizedCommitLandsOnItsOwn)
{
	Pair pair;

	pair.Role.OnSetDesync();

	pair.Child.OnSetBufferScale(3);
	pair.Child.OnCommit();

	// A decoder that wants its own cadence gets it, which is the whole reason the mode exists — the
	// alternative is a video that only advances when the toolkit around it repaints.
	GYRO_CHECK_EQ(pair.Child.Current().BufferScale, 3);
}

GYRO_TEST(Subcompositor, GoingDesynchronizedAppliesWhatWasAlreadyCached)
{
	Pair pair;

	pair.Child.OnSetBufferScale(4);
	pair.Child.OnCommit();

	GYRO_CHECK_EQ(pair.Child.Current().BufferScale, 1);

	// A client that stops batching wants what it has already stated to be on screen. Holding it until
	// the parent's next commit would leave a player that switched modes on a still window showing the
	// frame before the one it just decoded, for as long as nothing else moved.
	pair.Role.OnSetDesync();

	GYRO_CHECK_EQ(pair.Child.Current().BufferScale, 4);
}

GYRO_TEST(Subcompositor, TheParentsModeWinsOverTheChildsOwn)
{
	Pair pair;

	ClientSurface grandchild{ pair.Context };
	ClientSubsurface nested{ pair.Context, &grandchild, &pair.Child };

	static_cast<void>(grandchild.AdoptRole(nested));

	pair.Child.AddChild(nested);

	nested.OnSetDesync();

	// Desynchronized on its own account and synchronized because everything above it is, which is what
	// keeps a nested part from arriving a frame ahead of the window it belongs to.
	GYRO_CHECK(nested.IsSynchronized());

	grandchild.OnSetBufferScale(2);
	grandchild.OnCommit();

	pair.Child.OnCommit();

	// **Two levels of caching, and one commit does not discharge both.** The grandchild's state lands
	// when its own parent's does, and its own parent's is still waiting on the window — so a toolkit
	// that stated every part of a window and has not committed the window shows none of it, rather than
	// showing whichever parts it happened to nest shallowly.
	GYRO_CHECK_EQ(grandchild.Current().BufferScale, 1);

	pair.Parent.OnCommit();

	GYRO_CHECK_EQ(grandchild.Current().BufferScale, 2);
}

GYRO_TEST(Subcompositor, ASurfaceThatDidNotCommitPassesNothingDownToItsChildren)
{
	Pair pair;

	ClientSurface grandchild{ pair.Context };
	ClientSubsurface nested{ pair.Context, &grandchild, &pair.Child };

	static_cast<void>(grandchild.AdoptRole(nested));

	pair.Child.AddChild(nested);

	grandchild.OnSetBufferScale(2);
	grandchild.OnCommit();

	// The window commits and the surface in between never did. A cache is released by the surface
	// directly above it, so nothing here is owed an application yet — a compositor that walked the whole
	// tree on every window commit would show an arrangement the client is still halfway through stating.
	pair.Parent.OnCommit();

	GYRO_CHECK_EQ(grandchild.Current().BufferScale, 1);
}

GYRO_TEST(Subcompositor, ACommitTheParentNeverAppliedStillOwesItsDamage)
{
	Pair pair;

	pair.Child.OnDamage(0, 0, 10, 10);
	pair.Child.OnCommit();

	pair.Child.OnDamage(20, 20, 5, 5);
	pair.Child.OnCommit();

	pair.Parent.OnCommit();

	// **Two cached commits are one arrangement and two sets of changed pixels.** Replacing the cache
	// wholesale would drop the first rectangle, which is a patch of a video frame nobody repaints —
	// and it stays wrong until something else happens to damage it.
	GYRO_CHECK_EQ(pair.Child.Current().SurfaceDamage.size(), std::size_t{ 2 });

	// And the cache is spent rather than sticky: the next arrangement the parent applies carries the
	// damage stated after this one and not these rectangles again.
	pair.Child.OnCommit();
	pair.Parent.OnCommit();

	GYRO_CHECK(pair.Child.Current().SurfaceDamage.empty());
}

GYRO_TEST(Subcompositor, ASubsurfaceLeavesTheParentsRunsWhenItGoes)
{
	HostContext context;
	ClientSurface parent{ context };
	ClientSurface child{ context };

	{
		ClientSubsurface role{ context, &child, &parent };

		static_cast<void>(child.AdoptRole(role));

		parent.AddChild(role);

		GYRO_CHECK_EQ(parent.Pending().Above.size(), std::size_t{ 1 });
	}

	// A `wl_subsurface` destroyed leaves nothing behind in the parent, which is what keeps the next
	// commit from asking a freed object where its node is.
	GYRO_CHECK(parent.Pending().Above.empty());
	GYRO_CHECK(parent.Stack().Above.empty());

	// And the surface underneath has its role back, which is the protocol's own rule: the surface
	// returns to having none and may be given another.
	GYRO_CHECK(!child.HasRole());
}
