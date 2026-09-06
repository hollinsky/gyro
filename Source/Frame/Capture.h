#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Frame/Admission.h"
#include "Frame/Evaluator.h"
#include "Seam/Renderer.h"

// The memory that keeps a closing window owing its picture only once.
//
// Docs/Decisions.md decision 20 gives a window leaving the screen its own copy of its last frame
// rather than a hold on the client's buffer, and decision 46 keeps that copy in a rectangle of an
// atlas reserved when the output was configured. `Scene/Atlas.h` takes the rectangle the moment a
// retirement is observed, `World/Exit.h` carries it across the waist, and `Frame/Evaluator.h` says
// which items are the window. This is the one question left: whether it has been drawn yet.
//
// **The whole of the once-only rule lives here, on the frame thread, and nothing on the far side is
// asked.** A window's picture is its whole subtree drawn a second time into the atlas, so a snapshot
// that repeated every frame would spend an exit's entire cost on every frame of that exit — the
// animation a person is watching would stutter for exactly as long as they were watching it. Asking
// dispatch instead would mean a report out, a publication back, and the picture arriving two frames
// after the window needed it. What the far side sends is a reservation count that never repeats, and
// remembering the ones already filled is a comparison against a handful of integers.
//
// **A rectangle is not an identity and that is why the count exists.** A shelf hands the same texels
// to the next occupant as soon as the one before it is finished, so remembering *places* would let a
// window that closed a frame later inherit the picture of the one before it — on screen, for a whole
// fade, and only on a machine busy enough to have skipped a publication. See
// `ExitSnapshot::Reservation`.

class ExitCaptures
{
public:
	// What this output should draw into its atlas before its next composite: the walk's proposals
	// minus the ones already drawn, lowered to what a renderer is told.
	//
	// The span is this object's and lives until the next call, which is the same contract the draw
	// list travels under and for the same reason: decision 36 forbids allocating on this thread, so
	// the storage is a member sized once.
	[[nodiscard]] std::span<const SnapshotCapture> Propose(std::span<const ExitCapture> found, std::size_t output)
	{
		m_Count = 0;
		m_Output = output;

		if (output >= m_Filled.size())
		{
			return {};
		}

		Forget(found, output);

		for (const ExitCapture& capture : found)
		{
			if (m_Filled[output].Holds(capture.Reservation) || m_Count == m_Captures.size())
			{
				continue;
			}

			m_Captures[m_Count] = SnapshotCapture{ .Into = capture.Into,
				                                   .Slot = capture.Slot,
				                                   .Source = capture.Source,
				                                   .First = capture.First,
				                                   .Count = capture.Count };
			m_Proposed[m_Count] = capture.Reservation;
			++m_Count;
		}

		return { m_Captures.data(), m_Count };
	}

	// The first `taken` of the snapshots the last proposal offered are in a submission.
	//
	// **Called on a submission rather than on a presentation, and the difference is one the picture
	// cannot see.** What the next frame needs is that the drawing is *ordered before* whatever samples
	// it, which submission on the same queue is; whether the panel showed that frame is a question
	// about the panel. A frame that was recorded and then never presented has still filled the
	// rectangle, and a window fading over the frames after it draws from pixels that are there.
	//
	// **The count is the renderer's and not this object's**, per `Submission::Captured`: a renderer
	// with no snapshot path takes none, and remembering pictures nobody drew would put uninitialised
	// device memory on the glass for the length of a fade. A record the renderer refused, or one it
	// declined, is simply not confirmed and is offered again next frame — which is the deferral
	// decision 46 bounds by reserving the rectangle in advance.
	void Landed(std::uint32_t taken) noexcept
	{
		if (m_Output >= m_Filled.size())
		{
			return;
		}

		for (std::size_t index = 0; index < m_Count && index < taken; ++index)
		{
			m_Filled[m_Output].Remember(m_Proposed[index]);
		}
	}

	// Whether this output has the pixels for that reservation. `Frame/Evaluator.h`'s question once a
	// closing window draws from the atlas instead of from the surface it no longer has.
	[[nodiscard]] bool Holds(std::size_t output, std::uint32_t reservation) const noexcept
	{
		return output < m_Filled.size() && m_Filled[output].Holds(reservation);
	}

	// Everything this output has pixels for, handed to the walk so that a closing window whose picture
	// exists is drawn from it instead of from the surface it no longer has.
	//
	// **A span into this object rather than a question asked per node**, because the walk is inside a
	// frame section and the answer cannot change while it runs: the proposal that fills this is made
	// from the list the walk produces, so nothing is added to it until the walk is over.
	[[nodiscard]] std::span<const std::uint32_t> Held(std::size_t output) const noexcept
	{
		if (output >= m_Filled.size())
		{
			return {};
		}

		return { m_Filled[output].Reservations.data(), m_Filled[output].Count };
	}

private:
	// What one output has already drawn. A fixed run scanned linearly, which is shorter than an index
	// would be: the retiring set is a handful by construction (46), and this is asked once per exit
	// per frame.
	struct Filled
	{
		std::array<std::uint32_t, MaxExitCaptures> Reservations{};
		std::size_t Count = 0;

		[[nodiscard]] bool Holds(std::uint32_t reservation) const noexcept
		{
			for (std::size_t index = 0; index < Count; ++index)
			{
				if (Reservations[index] == reservation)
				{
					return true;
				}
			}

			return false;
		}

		void Remember(std::uint32_t reservation) noexcept
		{
			if (!Holds(reservation) && Count != Reservations.size())
			{
				Reservations[Count] = reservation;
				++Count;
			}
		}
	};

	// Drop what this output no longer owes, so that the memory is the size of the exits in flight
	// rather than of every window ever closed on this screen.
	//
	// **A reservation the walk no longer finds is one whose exit finished**, and its rectangle is back
	// on the shelf for somebody else. Forgetting it is what keeps the run short; forgetting it is
	// *safe* because the next occupant of those texels arrives under a count of its own, so nothing
	// that comes back can be mistaken for what left.
	//
	// **It is the walk's whole proposal that is scanned, before anything is filtered out of it**, which
	// is why `Frame/Evaluator.h` reports a closing window every frame rather than stopping once its
	// picture has been drawn. A list that had already been filtered would say a window was gone on the
	// frame after its snapshot was taken, and the reservation would be forgotten and then taken again
	// — the copy repeating for the length of the exit, which is the one thing this file exists to stop.
	void Forget(std::span<const ExitCapture> found, std::size_t output) noexcept
	{
		Filled& filled = m_Filled[output];
		std::size_t kept = 0;

		for (std::size_t index = 0; index < filled.Count; ++index)
		{
			const std::uint32_t reservation = filled.Reservations[index];
			bool present = false;

			for (const ExitCapture& capture : found)
			{
				present = present || capture.Reservation == reservation;
			}

			if (present)
			{
				filled.Reservations[kept] = reservation;
				++kept;
			}
		}

		filled.Count = kept;
	}

	std::array<SnapshotCapture, MaxExitCaptures> m_Captures{};
	std::array<std::uint32_t, MaxExitCaptures> m_Proposed{};
	std::size_t m_Count = 0;

	// Which output the last proposal spoke for, so that a confirmation cannot be filed against another
	// one. Past the end until the first proposal, which makes an unconfirmed call do nothing.
	std::size_t m_Output = MaxOutputs;

	std::array<Filled, MaxOutputs> m_Filled{};
};
