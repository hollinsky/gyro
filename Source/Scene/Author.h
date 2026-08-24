#pragma once

#include <string_view>

#include "Core/Result.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"

// What the dispatch loop steps: the producer that authors the published scene, opened once and
// advanced on every wake.
//
// **A gym is one implementor and it is deliberately not the only one the name allows.** A gym is gyro
// authoring for itself with no client behind it; the client host that turns a `wl_surface.commit`
// into a node is the other, and the two meet at the store rather than at this interface — so the
// contract is `ISceneAuthor`, not `IGym`.
//
// **It lives in `Scene` because it authors a scene.** A `SceneStore`, a `SceneCommit`, a
// `SceneSerializer` — everything an author touches to make a frame is this module's, and the interface
// names its own module the way `SceneStore` and `SceneReturn` do. That now includes the texture space:
// `ITextures` began in `Gym` and moved here when the second author turned out to be its heaviest caller
// rather than a party that could ignore it, since a client's `wl_shm` pool is pixels arriving exactly
// the way a gym's card arrives. `Scene/Textures.h` carries that argument. An author that draws no
// images — three of the five gyms — still never calls it.
//
// **An author that drives from a descriptor owns that descriptor, drains it inside `Advance`, and
// hands the composition root only the fd to sleep on.** The host reads its clients over a socket; a gym
// reads nothing. Draining is not a verb here because a gym would have to answer for one it does not
// have, and because a drain hoisted out of `Advance` would need the store to still be reachable when it
// ran — which is the reference `Open` and `Advance` are shaped to avoid handing out. Reading inside
// `Advance` puts the store and the texture space on the stack at the moment a `wl_surface.commit`
// arrives, so a host retains neither.
//
// **What the root does own is the flush**, because the root owns the wait. Everything gyro owes its
// clients back — a frame callback, a `wl_buffer.release` — is queued during the step and has to leave
// before the thread parks, and only the root knows when that is. A host flushing at the top of its own
// next `Advance` would hold a frame callback until something else woke dispatch, and with a settled
// world and a client waiting on exactly that callback, nothing would.
//
// The two verbs are separate because they run at different rates and answer different questions:
// `Open` builds a tree and can fail, `Advance` mutates one and cannot. An `Advance` that returned a
// `Result` would be a failure on the path a loop takes every wake, which is a failure nothing is in a
// position to do anything about — so the ways an author can be wrong are all in `Open`, and what
// `Advance` answers is only when to call it again.
class ISceneAuthor
{
public:
	virtual ~ISceneAuthor() = default;

	ISceneAuthor(const ISceneAuthor&) = delete;
	ISceneAuthor& operator=(const ISceneAuthor&) = delete;
	ISceneAuthor(ISceneAuthor&&) = delete;
	ISceneAuthor& operator=(ISceneAuthor&&) = delete;

	[[nodiscard]] virtual std::string_view Name() const noexcept = 0;

	// Author the tree, once, into a store that already carries the output set. Called before the first
	// `Advance` and never again.
	//
	// **The texture space arrives as an argument rather than being held**, on `SceneStore`'s terms and
	// for the same reason: what an author owns is a scene, and both of the things it authors that scene
	// *into* belong to the loop around it. An author that draws no images ignores it.
	[[nodiscard]] virtual Result<void> Open(SceneStore& scene, ITextures& textures) = 0;

	// Retarget whatever is due at `now`, and answer when the next thing falls due.
	//
	// **The origin a retarget is stamped with is the instant it fell due, not `now`.** A wake served
	// late is decision 89's ordinary case — the motion renders already in progress by exactly the
	// elapsed amount rather than starting from zero — so lateness costs the first frame or two of an
	// animation and never its shape, and an author stamping `now` would hide precisely the lateness it
	// exists to expose.
	[[nodiscard]] virtual Wake Advance(SceneStore& scene, ITextures& textures, Instant now) = 0;

protected:
	ISceneAuthor() = default;
};
