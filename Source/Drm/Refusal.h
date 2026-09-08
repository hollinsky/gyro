#pragma once

#include <array>
#include <cstdint>

#include "Seam/RenderTarget.h"

// What the display engine last refused, kept so that somebody outside the frame section can say what
// was in it.
//
// **Its own file because it is arithmetic over numbers rather than anything the kernel has to answer**,
// which is Drm/Catalog.h's reason: `IPresenter::TestLayers` runs on the `SCHED_FIFO` frame thread and
// needs a device to be asked at all, so a comparison living inside `DrmOutput` is a comparison no test
// can reach. The one that was there compared a field that changes every frame by construction, and a
// panel was the only place that would ever have shown it.

// One plane's worth of a refused proposal: the property values as they went to the kernel.
//
// **16.16 fixed point for the source rectangle, exactly as it was sent.** Reporting the numbers that
// were sent rather than the floats they came from is the point, since a conversion is one of the things
// that can be wrong.
struct RefusedLayer
{
	std::uint32_t Plane = 0;

	// **Printed rather than compared** — see `RefusedProposal::SameAs`. The format and the modifier are
	// deliberately absent: they are the kernel's, and `drmModeGetFB2` answers them from this id at
	// report time, off the frame path, where an ioctl is free.
	std::uint32_t Framebuffer = 0;

	std::uint64_t SrcX = 0;
	std::uint64_t SrcY = 0;
	std::uint64_t SrcW = 0;
	std::uint64_t SrcH = 0;

	std::int64_t CrtcX = 0;
	std::int64_t CrtcY = 0;
	std::uint64_t CrtcW = 0;
	std::uint64_t CrtcH = 0;
};

// **Compared rather than counted, which is what *once per distinct refusal* means.** A standing refusal
// is one line for the session; a refusal that changes when a window resizes is a new line, and the pair
// of them together is the diagnosis.
struct RefusedProposal
{
	std::array<RefusedLayer, MaxLayers> Layers{};
	std::uint32_t Count = 0;
	int Code = 0;

	// Every field of every layer except which framebuffer it named.
	//
	// **The composite lands on a different target every frame, and that is not a different refusal.**
	// gyro draws into a ring of images and flips them in turn, so the same arrangement of the same
	// planes carries a framebuffer id that never repeats — and comparing it turned *once per distinct
	// refusal* into *once per frame*, which is sixty warning lines a second in the journal of a machine
	// whose pointer is parked where the display engine will not take it.
	//
	// **What that gives up is a refusal that differs only in a buffer's format**, which no longer
	// discriminates here. A client that changes format without moving or resizing is the one case that
	// now goes unreported, and it is worth less than the flood.
	[[nodiscard]] constexpr bool SameAs(const RefusedProposal& other) const noexcept
	{
		if (Count != other.Count || Code != other.Code)
		{
			return false;
		}

		for (std::uint32_t index = 0; index < Count; ++index)
		{
			const RefusedLayer& left = Layers[index];
			const RefusedLayer& right = other.Layers[index];

			if (left.Plane != right.Plane || left.SrcX != right.SrcX || left.SrcY != right.SrcY ||
			    left.SrcW != right.SrcW || left.SrcH != right.SrcH || left.CrtcX != right.CrtcX ||
			    left.CrtcY != right.CrtcY || left.CrtcW != right.CrtcW || left.CrtcH != right.CrtcH)
			{
				return false;
			}
		}

		return true;
	}
};
