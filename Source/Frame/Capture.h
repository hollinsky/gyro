#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Frame/Admission.h"
#include "Publication/Reader/Reader.h"
#include "Seam/Renderer.h"
#include "World/Content.h"
#include "World/Exit.h"
#include "World/Node.h"

// What a closing window owes this output, and the memory that keeps it owing it only once.
//
// Docs/Decisions.md decision 20 gives a window leaving the screen its own copy of its last frame
// rather than a hold on the client's buffer, and decision 46 keeps that copy in a rectangle of an
// atlas reserved when the output was configured. `Scene/Atlas.h` takes the rectangle the moment a
// retirement is observed and `World/Exit.h` carries it across the waist; this is the frame thread
// reading it and saying what to copy.
//
// **The whole of the once-only rule lives here, on the frame thread, and nothing on the far side is
// asked.** The copy is thirty-three megabytes read and written for a full-screen window against a
// budget of two or three milliseconds, so a capture that repeated every frame would spend an exit's
// entire cost on every frame of that exit — the animation a person is watching would stutter for
// exactly as long as they were watching it. Asking dispatch instead would mean a report out, a
// publication back, and the picture arriving two frames after the window needed it. What the far side
// sends is a reservation count that never repeats, and remembering the ones already filled is a
// comparison against a handful of integers.
//
// **A rectangle is not an identity and that is why the count exists.** A shelf hands the same texels
// to the next occupant as soon as the one before it is finished, so remembering *places* would let a
// window that closed a frame later inherit the picture of the one before it — on screen, for a whole
// fade, and only on a machine busy enough to have skipped a publication. See `ExitSnapshot::
// Reservation`.
//
// **What is copied is found by walking rather than published.** The retiring root is a container —
// decision 111 makes a toplevel one, and decision 46 reserves against that container's own quad — so
// the pixels are on a node below it. Reading the run for them is decision 90's rule that the frame
// thread checks what it is given: an exit whose subtree holds no image is a window with nothing to
// copy, which is an absence rather than a refusal.

// SPEC: how many exit snapshots one output may take in one frame.
//
// It is `Publication/Return.h`'s `ReleasesPerReport` over again and for the same population — the
// windows that can start leaving one output within a single frame — because the buffer a hold keeps
// alive is the buffer a capture reads. Sixteen is well past what a person closes at once and past
// what an application's teardown lands in any one frame; beyond it the rest wait for the next frame,
// which is the deferral decision 46 says the reservation is there to bound.
inline constexpr std::size_t MaxExitCaptures = 16;

class ExitCaptures
{
public:
	// What this output should copy before its next composite.
	//
	// The span is this object's and lives until the next call, which is the same contract the draw
	// list travels under and for the same reason: decision 36 forbids allocating on this thread, so
	// the storage is a member sized once.
	[[nodiscard]] std::span<const SnapshotCapture> Gather(const SnapshotReader& snapshot, std::size_t output)
	{
		m_Count = 0;
		m_Output = output;

		if (output >= m_Filled.size())
		{
			return {};
		}

		const std::span<const ExitSnapshot> exits = snapshot.Exits<ExitSnapshot>();
		const std::span<const Node> nodes = snapshot.Nodes<Node>();
		const std::span<const ImageContent> images = snapshot.Images<ImageContent>();

		Forget(exits, output);

		for (const ExitSnapshot& exit : exits)
		{
			if (exit.Output != output || exit.Reservation == 0 || exit.Texture.IsNull() || exit.Slot.IsEmpty())
			{
				continue;
			}

			if (m_Filled[output].Holds(exit.Reservation) || m_Count == m_Captures.size())
			{
				continue;
			}

			const ImageContent* const content = Pixels(exit.Node, nodes, images);

			// A window whose subtree carries no image is one there is nothing to photograph: a
			// container that never had pixels, or one whose client took its buffer away on the way out.
			// Decision 46's answer to having no snapshot is the same either way — the window finishes
			// its exit at once rather than fading from an empty rectangle — so this is left unfilled and
			// unremembered, and becomes a capture the moment pixels appear under it.
			if (content == nullptr)
			{
				continue;
			}

			m_Captures[m_Count] = SnapshotCapture{
				.Into = exit.Texture, .Slot = exit.Slot, .From = content->Texture, .Source = content->Source
			};
			m_Proposed[m_Count] = exit.Reservation;
			++m_Count;
		}

		return { m_Captures.data(), m_Count };
	}

	// The first `taken` of the copies the last gather proposed are in a submission.
	//
	// **Called on a submission rather than on a presentation, and the difference is one the picture
	// cannot see.** What the next frame needs is that the copy is *ordered before* whatever samples
	// it, which submission on the same queue is; whether the panel showed that frame is a question
	// about the panel. A frame that was recorded and then never presented has still filled the
	// rectangle, and a window fading over the frames after it draws from pixels that are there.
	//
	// **The count is the renderer's and not this object's**, per `Submission::Captured`: a renderer
	// with no snapshot path takes none, and remembering copies nobody made would put uninitialised
	// device memory on the glass for the length of a fade. A record the renderer refused, or a copy it
	// declined, is simply not confirmed and is proposed again next frame — which is the deferral
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

private:
	// What one output has already copied. A fixed run scanned linearly, which is shorter than an index
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
	// **A reservation that has left the run is one whose exit finished**, and its rectangle is back on
	// the shelf for somebody else. Forgetting it is what keeps the run short; forgetting it is *safe*
	// because the next occupant of those texels arrives under a count of its own, so nothing that
	// comes back can be mistaken for what left.
	void Forget(std::span<const ExitSnapshot> exits, std::size_t output) noexcept
	{
		Filled& filled = m_Filled[output];
		std::size_t kept = 0;

		for (std::size_t index = 0; index < filled.Count; ++index)
		{
			const std::uint32_t reservation = filled.Reservations[index];
			bool present = false;

			for (const ExitSnapshot& exit : exits)
			{
				present = present || (exit.Output == output && exit.Reservation == reservation);
			}

			if (present)
			{
				filled.Reservations[kept] = reservation;
				++kept;
			}
		}

		filled.Count = kept;
	}

	// The image a closing window's snapshot is taken from: the first one in its subtree, in the
	// preorder the run is already written in, or nothing.
	//
	// **Every bound is checked rather than trusted**, per decision 90 — a subtree claiming more nodes
	// than the run holds, a content index past the image run — because a walk that trusted either
	// would read whatever bytes followed and copy them onto a screen.
	[[nodiscard]] static const ImageContent*
	Pixels(std::uint32_t node, std::span<const Node> nodes, std::span<const ImageContent> images) noexcept
	{
		if (node >= nodes.size())
		{
			return nullptr;
		}

		const std::size_t past = nodes[node].Past(node);

		if (past > nodes.size())
		{
			return nullptr;
		}

		for (std::size_t index = node; index < past; ++index)
		{
			const Node& record = nodes[index];

			if (record.IsHidden())
			{
				index = record.Past(index) - 1;

				continue;
			}

			if (record.Kind == NodeKind::Image && record.Content < images.size())
			{
				return &images[record.Content];
			}
		}

		return nullptr;
	}

	std::array<SnapshotCapture, MaxExitCaptures> m_Captures{};
	std::array<std::uint32_t, MaxExitCaptures> m_Proposed{};
	std::size_t m_Count = 0;

	// Which output the last gather spoke for, so that a confirmation cannot be filed against another
	// one. Past the end until the first gather, which makes an unconfirmed call do nothing.
	std::size_t m_Output = MaxOutputs;

	std::array<Filled, MaxOutputs> m_Filled{};
};
