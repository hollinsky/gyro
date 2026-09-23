#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Buffer.h"
#include "Core/Handle.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Publication/Return.h"
#include "Scene/Reach.h"
#include "Scene/Store.h"

// The return leg's dispatch side: what a presented frame means to the world that authored it.
//
// [Decision 115](../../Docs/Decisions.md#115-scene-drains-the-return-channel-and-protocol-observes-what-it-derives)
// puts the drain here rather than in `Protocol`, and the argument that decides it is the boot path.
// gyro publishes a scene and presents it before any client exists — the splash continues the firmware
// logo, the recovery console has no protocol behind it — and those snapshots still have to be reclaimed
// against decision 74's watermark. A drain that lived in `Protocol` would stop the ring the moment
// there was nothing to talk to, and the symptom is a compositor running out of snapshot slots on the
// screen it exists to keep showing.
//
// **Three quarters of what a report carries never leaves this module**, which is the sharpest form of
// the same argument. The watermark is the publisher's and frees everything below it. The exit blit's
// hold is the atlas's. Client damage cleared by a presented sequence is decision 113's and is the
// entity's. Only the frame callbacks, `wp_presentation_feedback` and `wl_buffer.release` cross to
// `Protocol` at all, and they cross as signals it owns the links to — decision 77's shape, legal
// because both modules are the dispatch thread and a signal is intra-thread by rule.
//
// **The edge is why it is a signal rather than a call.** `Protocol` depends on `Scene` and `Scene` may
// not say `Protocol`, so this cannot hand anybody a frame callback; it declares what happened and lets
// the observer decide what that is worth. `Session` connects to the same signal rather than a second
// one.
//
// **The derivation is the ledger below**, which is decision 115's accepted cost built rather than
// deferred: the entities a commit said were owed a frame, stamped with the sequence about to carry
// them and with the outputs their pixels land on, resolved as reports come back and dropped when every
// output has shown them. It is one entry per window with a frame in flight — superseded by that
// window's next commit, so a client committing faster than the panel scans does not accumulate — and
// the scan on the return path is over that list rather than over the world, which is the axis decision
// 115 rejects a pull on.
//
// **Three consumers want it and two of them exist today.** The frame callback was the first, and
// `wp_presentation_feedback` is the second — the same signal read for more of what it already carried,
// which is why serving that protocol added an argument here rather than a channel. Decision 113's
// client damage clears against *every* output having shown the sequence rather than the first, which
// is why `Owed` goes on clearing bits after the signal has gone out. `wl_buffer.release` is the third and needs no
// ledger yet, because `Protocol/Shm.h` copies at commit and releases in the same step — it lands with the dmabuf path,
// where the hold is real.
//
// **What is still per output rather than per surface is `Presented` itself**, and it stays that way:
// the boot splash and the recovery console present frames with nothing on the far end of them, so the
// channel has to be exercised with no author in the ledger at all.

// The presented state one output is in, as the world holds it.
//
// Separate from `PresentedFrame` because the two answer different questions. That one is a *report* —
// what happened since the last one crossed, zero where nothing did — and this is *state*, which is what
// decision 113's damage clear and a late-connecting observer both need. Folding them would make a
// quiet output indistinguishable from one that has never presented.
struct OutputPresentation
{
	// The highest published sequence this output has shown. Zero until it shows one, since
	// Publication/Ring.h begins at one.
	std::uint64_t Sequence = 0;

	// When that frame reached the glass.
	Instant At{};

	// What the panel said about that flip, forwarded from `PresentedFrame` and read by exactly one
	// party: [Protocol/Presentation.h](../Protocol/Presentation.h), which owes a client a retrace
	// counter, a refresh figure and a statement about how much of the timestamp was hardware. Nothing
	// in `Scene` derives anything from them, which is why they sit here as a group rather than beside
	// the sequence the ledger is resolved against.
	std::uint64_t Vblank = 0;
	Duration Period{};
	bool Vsync = false;
	bool HardwareClock = false;
	bool HardwareCompletion = false;
	bool ZeroCopy = false;

	[[nodiscard]] bool HasPresented() const noexcept { return Sequence != 0; }

	friend constexpr bool operator==(OutputPresentation, OutputPresentation) noexcept = default;
};

class SceneReturn
{
public:
	SceneReturn() = default;

	// Neither copied nor moved, because it owns signals and a signal's observers are links the observer
	// holds — a copy would give two objects the same observer list and a move would leave connections
	// pointing at the corpse. Core/Signal.h refuses both from its side; this says so from here.
	SceneReturn(const SceneReturn&) = delete;
	SceneReturn& operator=(const SceneReturn&) = delete;
	SceneReturn(SceneReturn&&) = delete;
	SceneReturn& operator=(SceneReturn&&) = delete;

	// A frame reached the glass on the output at this index, drawn from this published sequence, at this
	// instant.
	//
	// **The index is the output's position in the set both halves agreed on**, not an `OutputId`, and
	// that is deliberate: the report's run is positional for the same reason the snapshot's per-output
	// runs are, and translating to an identity here would put a second answer beside the ordering that
	// already has one. An observer that wants the identity asks `SceneStore` for output `index`.
	//
	// Fires once per output that actually presented, and not at all on a report where none did — which
	// is most of them, since a report crosses every frame whether or not any output flipped.
	Signal<std::size_t, std::uint64_t, Instant> Presented;

	// The compositor has finished with a client buffer. `wl_buffer.release` is `Protocol`'s to send;
	// what crosses here is only the identity, because a release that named the wrong buffer is a client
	// drawing into pixels gyro is still sampling.
	Signal<BufferId> Released;

	// What this entity committed has been shown, on this output, in the state that output is now in.
	// `wl_surface.frame` and `wp_presentation_feedback.presented` are `Protocol`'s to answer and this is
	// the fact both of them answer on — decision 115's derivation, arriving as an entity rather than as
	// a sequence because a sequence is a number no client has ever heard of.
	//
	// **The output index is the *fold's* answer rather than a second question**, which is why it is an
	// argument here and not something the observer looks up. Decision 32 gives a surface the cadence of
	// the fastest panel it touches, so *which* panel resolved the entry is already decided by the walk
	// below — and `wp_presentation_feedback.sync_output` is a client asking exactly that. An observer
	// re-deriving it from a window's geometry would answer *the outputs it is on* and pick one, which is
	// a different question with a different answer whenever a window straddles two.
	//
	// **It fires on the *first* output to show the frame, which is decision 32's cadence.** A surface on
	// two panels has one buffer and one callback queue, so per-output pacing is not expressible in the
	// protocol at all; the fastest output it touches is the rate it gets, and the earliest report at or
	// past its sequence is that rate arriving. The opposite fold — *every* output — is decision 113's
	// damage clear, which is why the ledger below goes on clearing bits after the signal has gone out
	// rather than dropping the entry on the first one.
	//
	// **The instant is when the frame reached the glass**, not when it was drawn and not now. A client
	// paces itself off this number, so handing it the moment gyro happened to drain the channel would
	// put dispatch's own jitter into every animation a toolkit runs. The rest of the record is the
	// panel's own account of that flip, forwarded rather than derived — a reference because it is the
	// state this object already holds, and reading it is legal only for the duration of the emit.
	Signal<EntityId, std::size_t, const OutputPresentation&> Reached;

	// Take one report. Called at the top of the dispatch iteration, once per report, until the channel
	// is empty.
	//
	// **Before input rather than after**, which is decision 115's addition to decision 45's ordering. A
	// `wl_surface.frame` callback delivered at the top of the iteration gives the client the whole of
	// that iteration to render into; the same callback at the bottom gives it the next one. Nothing
	// about `t₀` moves, since an input event carries its own timestamp and this is not the moment of
	// handling.
	//
	// **The watermark is deliberately not read here.** `SnapshotOutbox::Collect` has already taken it
	// and performed the reclamation it authorises, which is the arrangement that keeps a memory-safety
	// property from depending on anybody remembering to call two things.
	void Drain(const FrameReport& report)
	{
		// Decision 84's rule, on the return leg and in the direction that costs the least to be wrong
		// about: a run whose length is not this world's output set is no information rather than partial
		// information. It happens across a hotplug, where a report staged against the old set arrives
		// after the new one is bound — and attributing that flip by index would report one panel's frame
		// against another's.
		const std::span<const PresentedFrame> presented = report.Presented();

		if (presented.size() == m_Outputs)
		{
			for (std::size_t index = 0; index < presented.size(); ++index)
			{
				Observe(index, presented[index]);
			}
		}

		for (const BufferId id : report.Released())
		{
			if (!id.IsNull())
			{
				Released.Emit(id);
			}
		}
	}

	// Take what the store's commits declared owed, against the sequence about to carry them.
	//
	// **The sequence is the one the snapshot will be published under, read before the publish**, which
	// `SnapshotOutbox::NextSequence` makes knowable and idempotent: a publish the ring refuses supersedes
	// the pending slot under the same number, so the stamp stays the sequence that will eventually
	// carry this scene. `Dispatch/Textures.h` seals a retirement against the same number for the same
	// reason, and the two rules are one rule seen from either end of a snapshot's life.
	//
	// **An entry whose reach is empty is re-armed rather than resolved, and that is the invisible-window
	// case.** A window on no output — minimised behind a hidden container, placed off every screen, or
	// mapped before anything shows it — is owed nothing, because nothing is going to present it; the
	// protocol says as much, and a compositor that answered anyway would have a client repainting into a
	// screen it is not on. What it must not become is permanent: the entry stays, and every later
	// publication asks the geometry again, so the frame the window becomes visible on is the frame its
	// callback goes out on.
	void Seal(std::uint64_t sequence, SceneStore& store)
	{
		// An entity that has gone takes its entry with it. A client disconnecting mid-flight is the
		// ordinary case, and `Protocol` would ignore the id anyway — dropping it here is what keeps the
		// ledger the length of the live windows rather than of the session.
		std::erase_if(m_Owed, [&store](const Owed& entry) noexcept { return !store.IsLive(entry.Entity); });

		for (const EntityId id : store.Awaiting())
		{
			// A window whose client went away between the commit and the publish. There is nobody left to
			// tell, and the entity may already have been swept.
			if (!store.IsLive(id))
			{
				continue;
			}

			Stamp(id, sequence, Reach(store, id));
		}

		store.ClearAwaiting();

		for (Owed& entry : m_Owed)
		{
			if (entry.Outputs == 0 && !entry.Reported)
			{
				entry.Sequence = sequence;
				entry.Outputs = Reach(store, entry.Entity);
			}
		}
	}

	// The output set the reports are read against, replaced on a hotplug rather than mutated — the same
	// statement decision 84 makes about the snapshot's runs from the other side, and the reason a report
	// carries its own count.
	//
	// Everything already presented is dropped, because an index into the old set names a different panel
	// in the new one.
	void SetOutputs(std::size_t outputs) noexcept
	{
		m_Outputs = std::min(outputs, OutputsPerReport);
		m_Presentations = {};

		// A mask is a set of positions in the old set, so on the new one it names different panels — the
		// same statement decision 84 makes about a run whose length no longer matches. What has already
		// been reported is finished with and goes; what has not is re-armed at the next publication,
		// against the outputs that exist now.
		std::erase_if(m_Owed, [](const Owed& entry) noexcept { return entry.Reported; });

		for (Owed& entry : m_Owed)
		{
			entry.Outputs = 0;
		}
	}

	[[nodiscard]] std::size_t Outputs() const noexcept { return m_Outputs; }

	// Whether anything is still waiting on a frame reaching the glass.
	//
	// **It is the condition the composition root rings the return leg's doorbell on**, and the reason
	// the question is asked here rather than counted there: the root cannot see a ledger and the frame
	// thread must not know one exists. False on a run with no clients — a splash, a console, a gym —
	// which is what keeps a presented frame from costing a wakeup nobody needed.
	//
	// **A window nothing is going to present keeps this true for as long as it stays off screen**, and
	// what that costs is nothing on a still machine and one re-armed reach per publication on a busy
	// one. It is true rather than costly because the doorbell is rung by a *presented frame*: a world
	// where nothing moves presents nothing, so a minimised window waiting forever wakes nobody. Where
	// something else is animating, dispatch was being woken by that client's commits anyway.
	[[nodiscard]] bool Owing() const noexcept { return !m_Owed.empty(); }

	// What this output has last shown, and when. Decision 113's damage clear reads it, and so does an
	// observer that connected after the frame it cares about — a signal is an event and this is the
	// state behind it.
	[[nodiscard]] const OutputPresentation& Presentation(std::size_t output) const noexcept
	{
		return output < m_Outputs ? m_Presentations[output] : m_Nothing;
	}

private:
	// One output's entry, folded and then announced.
	//
	// **A sequence that did not advance is silence rather than an event**, and both halves of that
	// matter. Zero is the report's *no news*, which is the ordinary case on every frame an output did
	// not flip. A value that is not greater than what is already held is a report the merge in
	// `ReturnChannel::Stage` has already superseded — re-announcing it would hand a client a second
	// frame callback for one frame, which is a client that draws twice as fast as the panel and then
	// waits.
	void Observe(std::size_t index, const PresentedFrame& frame)
	{
		if (frame.Sequence <= m_Presentations[index].Sequence)
		{
			return;
		}

		m_Presentations[index] = { .Sequence = frame.Sequence,
			                       .At = frame.At,
			                       .Vblank = frame.Vblank,
			                       .Period = frame.Period,
			                       .Vsync = frame.Vsync,
			                       .HardwareClock = frame.HardwareClock,
			                       .HardwareCompletion = frame.HardwareCompletion,
			                       .ZeroCopy = frame.ZeroCopy };

		Presented.Emit(index, frame.Sequence, frame.At);

		Resolve(index, frame.Sequence);
	}

	// One entity's entry, superseded rather than appended to.
	//
	// **A second commit before the first was shown replaces the first**, which is not a loss: a frame
	// callback answers *draw again*, and answering it twice for two commits a client made inside one
	// refresh would ask for a frame the panel has nowhere to put. `ClientSurface::Apply` folds its
	// pending callbacks into one due list on exactly the same reasoning, so the two sides agree by
	// construction rather than by a rule somebody has to maintain across the module boundary.
	struct Owed
	{
		EntityId Entity{};

		// The published sequence that first carries what was committed. A report at or past it has shown
		// it, because the scene crossing the boundary is complete state and not a delta (74).
		std::uint64_t Sequence = 0;

		// The outputs that have not yet shown it, one bit each. Cleared as reports arrive.
		OutputReach Outputs = 0;

		// Whether the callback has gone out, which is the first bit clearing. Kept beside the mask
		// because the two answer decision 32's fold and decision 113's, and those are opposite
		// directions over one set.
		bool Reported = false;
	};

	void Stamp(EntityId id, std::uint64_t sequence, OutputReach outputs)
	{
		const auto matches = [id](const Owed& entry) noexcept { return entry.Entity == id; };
		const auto at = std::find_if(m_Owed.begin(), m_Owed.end(), matches);

		if (at != m_Owed.end())
		{
			*at = Owed{ .Entity = id, .Sequence = sequence, .Outputs = outputs, .Reported = false };

			return;
		}

		m_Owed.push_back(Owed{ .Entity = id, .Sequence = sequence, .Outputs = outputs, .Reported = false });
	}

	// One output's report, against everything still owed.
	//
	// The scan is over the windows with a frame in flight, which is what committed since the last
	// presentation — a handful on a busy desktop and none at all on a still one. Decision 115 rejects a
	// derivation whose cost is the size of the world, and this is that rule kept on the return path.
	void Resolve(std::size_t index, std::uint64_t sequence)
	{
		const OutputReach bit = index < MaxReachableOutputs ? OutputReach{ 1 } << index : 0;

		if (bit == 0)
		{
			return;
		}

		for (Owed& entry : m_Owed)
		{
			if ((entry.Outputs & bit) == 0 || sequence < entry.Sequence)
			{
				continue;
			}

			entry.Outputs &= ~bit;

			if (!entry.Reported)
			{
				entry.Reported = true;

				Reached.Emit(entry.Entity, index, m_Presentations[index]);
			}
		}

		// Erased after the walk rather than inside it, because an observer of `Reached` above may commit
		// — a shell reacting to a frame having landed is an ordinary thing — and a commit does not touch
		// this list, but a future one might. Doing the removal here costs nothing and takes the question
		// away.
		std::erase_if(m_Owed, [](const Owed& entry) noexcept { return entry.Reported && entry.Outputs == 0; });
	}

	// Decision 115's run of handles retained beside the snapshots in flight, as one entry per window
	// waiting on a frame. Bounded by the live entities and in practice by the windows a person is
	// looking at, because an entry is superseded by the next commit on the same entity and erased when
	// every output has shown it.
	std::vector<Owed> m_Owed;

	std::size_t m_Outputs = 0;
	std::array<OutputPresentation, OutputsPerReport> m_Presentations{};

	// What `Presentation` answers for an output outside the set. A reference has to name something, and
	// an output that has never presented is exactly what an output that does not exist has in common
	// with one that has not drawn yet.
	static constexpr OutputPresentation m_Nothing{};
};
