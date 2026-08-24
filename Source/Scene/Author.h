#pragma once

#include <string_view>

#include "Core/Result.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Scene/Store.h"

// What an author is handed onto the texture id space. Declared in `Gym` and implemented in
// `Dispatch` — decision 136's placement, which this interface does not disturb — and named here only
// by reference, because a scene author is a *caller* of the texture verbs rather than a definer of
// them. Forward-declared rather than included so that `Scene` gains no edge to `Gym`, which depends on
// it the other way.
class ITextures;

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
// names its own module the way `SceneStore` and `SceneReturn` do. The one thing an author is handed
// that is *not* `Scene`'s is the texture space, and that is `ITextures` above: forward-declared, so
// the author's home carries no dependency the author itself does not. An author that draws no images —
// three of the five gyms, and most of what a shell authors — never names it at all.
//
// **An author that drives from a descriptor has that event source wired in beside it by the
// composition root, which is why draining one is not a verb here.** The host reads its clients over a
// socket; a gym reads nothing; the loop drains whatever source the root handed it before it calls
// `Advance`. Putting the descriptor on this interface would make every gym answer for one it does not
// have, and would drag `Seam`'s `IEventSource` into a contract decision 136 keeps clients' peers away
// from.
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
