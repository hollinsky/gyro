#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Core/Buffer.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Publication/Return.h"

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
// **What is not here yet is the derivation itself.** Decision 115's accepted cost is a run of handles
// retained beside each in-flight snapshot and freed at the watermark, and that is phase two's — it
// wants the matching and lifetime work `Scene/Commit.h` still has no code for. Until then a presented
// sequence crosses as a sequence, which is the number every one of those derivations is a function of,
// and the signal fires per output rather than per surface. What that costs is that `Protocol` cannot
// yet be written against it; what it buys is that the channel is exercised end to end before a client
// exists, which is the only way the boot path above is ever tested.

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
	}

	[[nodiscard]] std::size_t Outputs() const noexcept { return m_Outputs; }

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

		m_Presentations[index] = { .Sequence = frame.Sequence, .At = frame.At };

		Presented.Emit(index, frame.Sequence, frame.At);
	}

	std::size_t m_Outputs = 0;
	std::array<OutputPresentation, OutputsPerReport> m_Presentations{};

	// What `Presentation` answers for an output outside the set. A reference has to name something, and
	// an output that has never presented is exactly what an output that does not exist has in common
	// with one that has not drawn yet.
	static constexpr OutputPresentation m_Nothing{};
};
