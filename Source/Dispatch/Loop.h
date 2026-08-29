#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Core/Trace.h"
#include "Core/Wake.h"
#include "Dispatch/Textures.h"
#include "Publication/Publisher/Outbox.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Scene/Author.h"
#include "Scene/Cursor.h"
#include "Scene/Output.h"
#include "Scene/Return.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Seam/Importer.h"
#include "Seam/Input.h"

// The dispatch thread's iteration, with the wait left to whoever owns the thread.
//
// **It is a step for the reason
// [decision 80](../../Docs/Decisions.md#80-the-frame-loop-is-a-step-the-composition-root-owns-the-wait)
// makes the frame loop one**, and the symmetry is the point rather than a nicety: the composition root
// is the only thing in the process allowed to name a ring, a descriptor, or a thread, so both halves of
// the boundary hand it a `Wake` and let it do the sleeping. That keeps this file portable, which is
// what lets the producer side of the publication boundary be exercised on a machine with no GPU, no
// seat, and no compositor — the same tier the gyms are held to and for the same reason.
//
// **What it is not is the dispatch loop [Architecture.md](../../Docs/Architecture.md#the-dispatch-loop)
// describes.** That one drains input first, demarshals client traffic under a per-connection budget,
// and imports buffers. None of those have a producer yet. What is here is the part of that loop which
// exists below all of it and does not change when they arrive: author, serialise, publish, reclaim.
// The gym is standing where the clients will stand, and `ISceneAuthor`'s two verbs are the shape a shell has
// anyway — author once, retarget what is due, say when to come back.
//
// **Nothing here blocks and nothing here waits on the frame thread**, which is
// [decision
// 61](../../Docs/Decisions.md#61-the-frame-thread-is-sched_fifo-the-earliest-deadline-first-schedule-is-gyros-not-the-kernels)'s
// priority order surviving contact: a full ring costs a retained buffer rather than a lost snapshot,
// and the watermark arrives on a queue the frame thread never waits to write to.

// SPEC: how long dispatch waits before retrying a publish the ring had no room for.
//
// **There is nothing to wake it, and that is the whole reason for a number here.** A refused publish is
// unblocked by the frame thread posting a `FrameReport`, and the return channel carries no descriptor
// — [decision 83](../../Docs/Decisions.md#83-dispatchs-publication-is-an-event-source) gave the
// *forward* channel a doorbell precisely because the frame thread had nothing to wake it, and the
// return direction has the same hole with the threads reversed. So dispatch polls, and this is the
// cadence.
//
// **What sizes it is the event it is waiting for, which is a frame completing.** A retry can only
// succeed once the frame thread has posted a report that moves the watermark past the slot the
// deferred sequence wants, and it posts one per frame — so an interval below a panel period spends
// whole scene walks that cannot possibly go out, and spends every one of them at the moment the
// machine is already behind. The world holds no refresh rate to derive this from: decision 97 gives
// the mode's extent to the frame side, and `Scene/Output.h` carries where an output *is* rather than
// when it scans, so this is a constant here rather than a function of the output set.
//
// **Four milliseconds is under one period for any panel to 250 Hz and a quarter of a frame at 60.**
// Below it there is nothing to win: a refused publish means the frame thread is four publishes behind,
// so a retry that succeeded instantly would still not be *read* for four frames. Above it there is
// something to lose, but in one corner only — a world that settles with its last publish refused,
// where the retry's own publication is what rings
// [decision 83](../../Docs/Decisions.md#83-dispatchs-publication-is-an-event-source)'s doorbell and
// wakes an idle frame thread. That delay lands at the tail of a spring that has already stopped
// moving. Carried in [Open.md](../../Docs/Open.md) as a doorbell the return channel does not have.
inline constexpr Duration PublishRetryInterval = std::chrono::milliseconds{ 4 };

// One dispatch iteration: everything the world's author owes the frame thread, and when to come back.
class DispatchLoop
{
public:
	// The importers are the renderers a texture has to exist on, borrowed from the composition root —
	// which is the only party that has them, since a renderer is per output and the seam is where the
	// two halves of one meet. An empty set is legal and makes every adopt `ENODEV`, which is the shape
	// of a run with no renderer that can sample: `--gym=card` refuses to open rather than authoring a
	// scene of nodes that would draw nothing and say nothing.
	DispatchLoop(
		const IClock& clock,
		SnapshotRing& ring,
		ReturnChannel& returns,
		std::span<ITextureImporter* const> importers = {},
		std::span<const TextureFormat> formats = {},
		IScanoutImporter* scanout = nullptr,
		IDmabufAllocator* allocator = nullptr
	)
		: m_Store{ clock }, m_Outbox{ ring, returns }, m_Textures{ importers, formats, scanout, allocator }
	{}

	// Neither copied nor moved, for `SnapshotOutbox`'s reason rather than a weaker one: the outbox is
	// the ring's writer *and* the bookkeeping beside it, and a second loop on one ring would keep a
	// second answer to what is still out.
	DispatchLoop(const DispatchLoop&) = delete;
	DispatchLoop& operator=(const DispatchLoop&) = delete;
	DispatchLoop(DispatchLoop&&) = delete;
	DispatchLoop& operator=(DispatchLoop&&) = delete;

	// Take the author and the output set, and let the author build its tree. Called once, before the
	// first `Step`.
	//
	// **The outputs are the composition root's and arrive here rather than being discovered**, because
	// where an output sits in global space is a fact only the root holds — it is the one thing that has
	// both the mode the backend agreed to and the layout the world is arranged in, which is
	// [decision
	// 87](../../Docs/Decisions.md#87-a-type-both-halves-of-the-world-name-lives-below-both-waists-not-in-seam)'s
	// division. Their *order* is load-bearing: the snapshot's per-output wake and placement runs are
	// positional, so index `i` here has to be the frame loop's output `i`.
	[[nodiscard]] Result<void> Open(std::unique_ptr<ISceneAuthor> author, std::span<const SceneOutput> outputs)
	{
		if (m_Author)
		{
			return Failure(EEXIST, "the dispatch loop already has an author");
		}

		if (!author)
		{
			return Failure(EINVAL, "the dispatch loop needs an author");
		}

		// **An empty output set is refused rather than published**, and the failure it prevents is the
		// quiet one. Decision 84 makes a run whose length is not the output set's *no* information
		// rather than partial information, so a scene published against no outputs reaches the frame
		// thread as a wake schedule of length zero — which reads as nothing owed. The compositor would
		// then fold to idle, correctly, with a scene nobody ever sees, and every counter in the process
		// would say it was working.
		if (outputs.empty())
		{
			return Failure(EINVAL, "a scene with no outputs publishes a wake schedule nothing can read");
		}

		m_Store.SetOutputs(outputs);

		// The drain reads a positional run, so it has to be told how long this world's is — the same
		// number and the same order the store just took, which is what makes index `i` here the frame
		// loop's output `i` on the way back as well as on the way out.
		m_Return.SetOutputs(outputs.size());

		if (const Result<void> opened = author->Open(m_Store, m_Textures); !opened)
		{
			return opened;
		}

		m_Author = std::move(author);

		return {};
	}

	// One iteration. Returns the wake the next one is owed at; `Wake::Never()` arms nothing, which on
	// this side of the boundary means *the world has stopped changing* rather than *nothing is being
	// drawn*.
	//
	// The order is `SnapshotOutbox`'s own: collect first so the watermark is fresh and everything below
	// it is back in the pool before anything asks for a buffer, then author, then publish what the scene
	// resolved to.
	[[nodiscard]] Wake Step()
	{
		// Nothing has been authored, so there is nothing to say and nothing to come back for. A loop
		// stepping an unopened dispatch is a wiring mistake in the root rather than a state to serve,
		// and `Open` is where it is caught.
		if (!m_Author)
		{
			return Wake::Never();
		}

		// The author's whole iteration, and the row it lands on is this thread's — so the trace reads as
		// two rows with arrows between them rather than one interleaved column.
		const TraceSpan step{ "step" };

		{
			const TraceSpan collect{ "collect" };

			Collect();
		}

		// **Beside the outbox's own reclamation and under the same number.** Everything strictly below
		// the watermark is the dispatch side's to take back — snapshot buffers there, the pixels behind
		// a retired texture here — and doing both at the top of the step is what makes *retirement rides
		// on the watermark* one rule rather than two implementations of it.
		m_Textures.Reclaim(m_Outbox.Watermark());

		const Instant now = m_Store.Now();

		TraceSpan advance{ "author" };

		const Wake authored = m_Author->Advance(m_Store, m_Textures, now);

		advance.Close();

		// **The cursor is stepped here rather than authored**, and it is between the author and the
		// serialisation because both sides are its business: the author may have changed the world the
		// pointer is over, and the scene about to cross has to carry the glyph where the last input
		// event put it. `Scene/Cursor.h` carries why it is not an author's job — the splash, the
		// recovery console, a gym and the client host all want the same pointer, and one that each of
		// them had to remember to draw is one the console is without.
		//
		// **Before the seal and not after**, for the same reason the seal is where it is: a pointer
		// that just went away retires its image in here, and the scene about to be published is the
		// first that does not name it.
		m_Cursor.Step(m_Store, m_Textures);

		// **Before the publish and not after**, because the number has to be the sequence this step's
		// snapshot will carry: the author gave those textures up during the `Advance` above, so the
		// scene about to go out is the first that does not name them.
		m_Textures.Seal(m_Outbox.NextSequence());

		// **`Serialize` takes the store by mutable reference and that is the point of calling it here
		// rather than caching**: the walk that decides whether a coefficient crosses is the walk that
		// retires the channels which have settled, so a scene that is finishing gets smaller only
		// because this ran.
		// Read before the publish because the ring's answer is idempotent until one is consumed, and
		// after the seal for the same reason the seal is where it is: this is the number the scene about
		// to cross will carry, and it is the number every row downstream of here is named for — the
		// `acquired` mark it draws an arrow to, and the slice on the glass row that says which scene a
		// person was looking at.
		const std::uint64_t sequence = m_Outbox.NextSequence();

		TraceSpan serialize{ "serialize", TraceThread, TraceTag(sequence) };

		const bool published = m_Outbox.Publish(m_Serializer.Serialize(m_Store));

		serialize.Close();

		// **Beside the texture seal and under the same number**, and the two are mirror images: that one
		// stamps what the scene stopped naming, this one stamps what it started showing. A commit during
		// the `Advance` above said an entity is owed a frame, and the scene that just crossed is the
		// first that carries it — so a report at or past this sequence is that entity reaching the glass,
		// which is what `Protocol` turns into a `wl_surface.frame` callback.
		//
		// **After the serialisation rather than before it**, because the walk sweeps: an entity whose
		// author went away and whose exit has finished is destroyed in there, and sealing first would put
		// a handle in the ledger that the same step then freed.
		m_Return.Seal(sequence, m_Store);

		if (published)
		{
			++m_Publications;

			TraceMark("published", TraceThread, TraceFlow(TraceDomain::Scene, sequence));
		}
		else
		{
			++m_Deferrals;

			// The frame thread being four publishes behind, which is the one thing on this row that is
			// about the *other* row. Counted already; marked here because a run of these beside a gap in
			// the frame thread's iterations is the pair that names which side is late.
			TraceMark("deferred", TraceThread, TraceFlow(TraceDomain::Scene, sequence));
		}

		TraceCount("watermark", static_cast<std::int64_t>(m_Outbox.Watermark()));

		// **`Flush` is deliberately not called beside `Publish`**, though `SnapshotOutbox`'s header
		// sketches a loop that does. Flushing first would deliver the refused snapshot *and* then the
		// one just built, spending two of the four ring slots at the one moment the frame thread has
		// already shown it cannot keep up, to hand it a scene the world has moved past. `Publish`
		// supersedes the pending slot in place under the same unconsumed sequence, which is the
		// behaviour its own documentation argues for. `Flush` earns its place the day a step can decide
		// it has nothing new to serialise; today every step does.
		//
		// **Superseding is also what pays for `PublishRetryInterval` being a panel period rather than a
		// millisecond**, and the two choices only work together. A retry that merely flushed would hand
		// over a scene one interval stale, so lengthening the interval would be lengthening how far
		// behind the delivered scene is; a retry that re-serialises hands over the current one whenever
		// it lands, so the interval buys nothing back for being short. Shortening it and flushing is the
		// combination to avoid: it is the most work for the stalest result.

		// Two folds, from opposite sides of the same settling. `Advance` says when the author next wants
		// to write; `Republish` says when a channel it already wrote comes to rest, which is when this
		// scene can be re-serialised smaller. Both must be armed or one of the two halves of
		// Docs/Architecture.md#doing-nothing-must-cost-nothing goes unserved.
		const Wake wake = Sooner(authored, m_Serializer.Republish());

		return published ? wake : Sooner(wake, Wake::At(Advanced(now, PublishRetryInterval)));
	}

	// The world this loop authors into, so that a test can ask what is in the scene rather than infer it
	// from the bytes that crossed.
	[[nodiscard]] const SceneStore& Store() const noexcept { return m_Store; }

	// The images the world holds, for the same reason: a test asserts that a swap retired what it
	// replaced rather than that nothing crashed, and the root's report line says how many are live.
	[[nodiscard]] const TextureRegistry& Textures() const noexcept { return m_Textures; }

	[[nodiscard]] TextureRegistry& Textures() noexcept { return m_Textures; }

	// What is still out and what has come back. Nothing in the design reads these; they are the
	// composition root's report line and a test's way of asserting that reclamation happened rather than
	// that nothing crashed.
	[[nodiscard]] const SnapshotOutbox& Outbox() const noexcept { return m_Outbox; }

	[[nodiscard]] std::uint64_t Publications() const noexcept { return m_Publications; }

	// Publishes the ring had no room for. Retained rather than lost, and retried on the wake above — so
	// a number that climbs is the frame thread falling behind, not a scene going missing.
	[[nodiscard]] std::uint64_t Deferrals() const noexcept { return m_Deferrals; }

	// Frame reports taken off the return channel. The releases in them belong to whoever imported the
	// buffers, and nothing imports one yet.
	[[nodiscard]] std::uint64_t Reports() const noexcept { return m_Reports; }

	// What the reports meant, as signals. `Protocol` owns the links to these — decision 115's shape, and
	// the reason the drain is `Scene`'s rather than the protocol layer's is that the boot splash and the
	// recovery console present frames with nothing on the far end of them.
	// Whether a client is waiting on a frame that has not reached the glass yet, which is the composition
	// root's condition for waking this thread when one does. `Scene/Return.h` carries the argument.
	[[nodiscard]] bool Owing() const noexcept { return m_Return.Owing(); }

	// Let the devices drive where the pointer is. The root's to call once, before the first step, for
	// `ClientHost::Observe`'s reason: it is the only party that holds both the device set and this loop.
	//
	// **The store is written from the signal rather than through a buffered displacement**, and the
	// connection is legal because both ends are on this thread — `IInput` is drained by the composition
	// root's dispatch iteration, immediately before `Step`, which is
	// [Seam/Input.h](../Seam/Input.h)'s whole reason for being an `IEventSource` that dispatch pumps. A
	// delta parked in the root and applied here would be a second encoding of one displacement, which is
	// the shape decision 152 rejects one level up when it refuses a frame-side copy of the position.
	//
	// **Every device drives the one cursor**, so this connects the signal and not a device: two mice on
	// a desk are one pointer that two hands can push, which [Scene/Pointer.h](../Scene/Pointer.h) states
	// and `Move` is written to accumulate.
	//
	// **`IInput::Position` is deliberately not connected**, and the absence is a missing fact rather than
	// an omission: it carries a fraction of a *device's* active area, and turning that into a place on a
	// screen needs a tablet-to-output binding that nothing in the tree holds yet. Connecting it against
	// the first output would put a stylus somewhere arbitrary on a two-monitor desk and look like a warp
	// bug rather than like absent configuration.
	void Observe(IInput& input)
	{
		m_Motion.ConnectTo<&DispatchLoop::OnMotion>(input.Motion, *this);
		m_Touch.ConnectTo<&DispatchLoop::OnTouch>(input.Touch, *this);
	}

	// The pointer as something on screen, so a test can ask whether there is one rather than count
	// nodes. Nothing in the design reads it.
	[[nodiscard]] const SceneCursor& Cursor() const noexcept { return m_Cursor; }

	[[nodiscard]] SceneReturn& Return() noexcept { return m_Return; }

	[[nodiscard]] const SceneReturn& Return() const noexcept { return m_Return; }

private:
	// Drain the return channel to empty, which is what advances the watermark and puts every buffer
	// below it back in the pool. Taking one report and stopping would leave reclamation a frame behind
	// forever, since the frame thread posts one per frame whatever else happened.
	//
	// **Each report is handed on rather than counted and dropped.** `SnapshotOutbox::Collect` takes the
	// watermark and performs the reclamation it authorises, which is the half that is a memory-safety
	// property; `SceneReturn` takes what is left — decision 115's derivation, which is where a presented
	// sequence becomes a frame callback and a hold becomes a `wl_buffer.release`. Both halves see every
	// report, and neither can be advanced without the other running.
	// A mouse, a touchpad or a trackpoint moved the seat's cursor.
	//
	// **The displacement has already been through the acceleration curve**, which is `Core/Input.h`'s
	// rule and the reason nothing is summed before it arrives: the curve is nonlinear in velocity, so a
	// coalesced displacement accelerated once is a pointer that feels mushy when it is eased. What is
	// accumulated here is the accelerated result, which is what `ScenePointer` is for.
	void OnMotion(const PointerMotion& motion)
	{
		static_cast<void>(m_Store.Pointer().Move({ motion.DeltaX, motion.DeltaY }, m_Store.Outputs()));
	}

	// A finger landed, so there is no cursor to draw.
	//
	// **A touchscreen has a position and no pointer**, and a cursor left parked in the middle of a kiosk
	// panel is the visible form of getting that wrong. The position itself is left where the mouse put
	// it rather than moved to the contact: `Scene/Pointer.h` forbids driving the seat's cursor from a
	// touch, because a pointer that jumps to wherever a finger landed is the behaviour every
	// touchscreen laptop that gets this wrong exhibits.
	void OnTouch(const TouchEvent&) { m_Store.Pointer().Hide(); }

	void Collect()
	{
		FrameReport report{};

		while (m_Outbox.Collect(report))
		{
			m_Return.Drain(report);
			++m_Reports;
		}
	}

	SceneStore m_Store;
	SceneSerializer m_Serializer{};
	SnapshotOutbox m_Outbox;

	// The return leg's reader, beside the outbox that is its other half. One report feeds both.
	SceneReturn m_Return{};

	// The texture id space, which is the world's rather than the loop's — it outlives any one snapshot
	// and is what a device migration re-adopts against. Held here because this is the object that knows
	// both the watermark and the moment an author has finished giving things up.
	TextureRegistry m_Textures;

	std::unique_ptr<ISceneAuthor> m_Author;

	// The pointer as something on screen, kept on `ScenePointer` by the step above. Held here rather
	// than in the store beside the position it follows, because drawing it needs the texture space and
	// the store has none — and because an author handed the world would then be handed a cursor it
	// could retire.
	SceneCursor m_Cursor{};

	// The devices' link to where the pointer is. Held rather than fired and forgotten, because a device
	// set that goes away while this loop is alive has to be able to drop the observer.
	Connection<const PointerMotion&> m_Motion;
	Connection<const TouchEvent&> m_Touch;

	std::uint64_t m_Publications = 0;
	std::uint64_t m_Deferrals = 0;
	std::uint64_t m_Reports = 0;
};
