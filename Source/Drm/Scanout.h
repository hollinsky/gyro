#pragma once

#include <array>
#include <cstdint>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Seam/Importer.h"
#include "Seam/Scanout.h"

namespace Drm
{

// SPEC: how many client buffers one card holds a framebuffer for at once.
//
// It is a working set rather than a window count: a promoted window cycles two or three buffers and
// each is one entry, so this is roughly eighty windows' worth of buffers all promotable at the same
// moment on one card. Past it, `Adopt` refuses and the surface is sampled instead of promoted, which
// is a frame that composites rather than anything a person sees. Fixed because the table is read on
// the frame thread and decision 36 forbids growing one there.
inline constexpr std::uint32_t MaxScanoutImages = 256;

// What a display device needs of an output in order to know whether it has stopped scanning a
// framebuffer out.
//
// **The mirror of Render/Textures.h's `ITextureFence`, and it exists for the same reason**: the
// importer is per card and the reader is per output, so one table has to ask several panels. What
// differs is the completion — a renderer answers *has my queue drained past this value*, and a panel
// answers *is this image one of the two commits I could still be showing* — because there is no
// timeline here to compare against.
//
// A separate interface rather than a `DrmOutput*` because the dependency would otherwise run both
// ways: an output resolves an id through this table, and this table would name the output.
class IScanoutHold
{
public:
	IScanoutHold() = default;

	virtual ~IScanoutHold() = default;

	IScanoutHold(const IScanoutHold&) = delete;
	IScanoutHold& operator=(const IScanoutHold&) = delete;
	IScanoutHold(IScanoutHold&&) = delete;
	IScanoutHold& operator=(IScanoutHold&&) = delete;

	// Whether this output could still be reading that framebuffer. Written by the frame thread at a
	// flip and read here on the dispatch thread, so the implementation carries the ordering.
	//
	// **False where the output cannot know is not an option**, unlike a texture fence's `Reached`: an
	// output that answers *no* about an image it is still scanning out has the framebuffer removed
	// underneath it, which disables the plane. Where an output does not know, it says *yes*, and the
	// cost is a framebuffer held one frame longer.
	[[nodiscard]] virtual bool Holds(std::uint32_t framebuffer) const noexcept = 0;
};

// Seam/Scanout.h on a KMS card: a client's dmabuf becoming something the display engine can read.
//
// **The GEM handles underneath are refcounted here and the reason is a bug this file exists to
// prevent.** `drmPrimeFDToHandle` returns the *same* handle for two imports of one dmabuf on one
// device — it is the kernel's own per-file handle for that buffer object, not a fresh name — so a
// table that closed its handle when one entry was done would invalidate every other entry over the
// same buffer. What that looks like is a window going black when an unrelated window closes, on the
// machine where two clients share a buffer or one buffer is promoted on two outputs of one card. So a
// handle is held once with a count, and `GEM_CLOSE` happens when the last framebuffer over it goes.
//
// It is also why nothing here relies on `drmModeRmFB` to release handles. It does not: removing a
// framebuffer drops the framebuffer's own reference to the buffer object and leaves the handle in
// this file's handle table, which for a per-window churn of buffers is an unbounded leak rather than
// the bounded one an output's four targets are.
class DrmScanout final : public IScanoutImporter
{
public:
	// The device's own descriptor, by reference rather than by value: a `DrmDevice` is constructed
	// before it opens anything, so a borrow taken at construction would be the invalid one. It is
	// declared before this member and outlives it.
	explicit DrmScanout(const Fd& device) noexcept : m_Device{ device } {}

	~DrmScanout() override;

	DrmScanout(const DrmScanout&) = delete;
	DrmScanout& operator=(const DrmScanout&) = delete;
	DrmScanout(DrmScanout&&) = delete;
	DrmScanout& operator=(DrmScanout&&) = delete;

	// Seam/Scanout.h's two verbs, on the dispatch thread.
	//
	// `EINVAL` for a null id, a source that does not describe the image it claims, a mapped source, or
	// a descriptor the card will not import; `ENOMEM` where the table is full.
	[[nodiscard]] Result<void> Adopt(TextureId id, const TextureSource& source) override;

	void Forget(TextureId id) noexcept override;

	// An output that could be scanning these framebuffers out. Registered by the composition root,
	// which is the only party that sees both. Registering none is legal and means a forgotten
	// framebuffer is removed immediately, which is correct for a card driving nothing.
	void Attach(IScanoutHold& hold) noexcept;

	void Detach(IScanoutHold& hold) noexcept;

	// What an id names, on the frame thread. Zero for an id this card has no framebuffer for, which is
	// the answer `DrmOutput::Expressible` turns into a refusal of the whole partition.
	//
	// **Total, allocating nothing, and lock-free by the watermark**, which is Render/Textures.h's
	// argument unchanged: the dispatch thread only ever writes an entry no published snapshot names,
	// and the frame thread only ever reads ones the snapshot it is composing from does.
	[[nodiscard]] std::uint32_t Find(TextureId id) const noexcept;

	// How that id's pixels are laid out, as the fourcc and modifier the framebuffer was created under.
	// An invalid format for an id this card has no framebuffer for, which is the same miss `Find`
	// reports as zero.
	//
	// **Kept rather than asked of the kernel, because `drmModeGetFB2` is an ioctl and the caller is
	// `DrmOutput::Expressible` on the `SCHED_FIFO` frame thread.** What wants it is the pre-filter that
	// decides whether a plane advertises this layout at all — which is the cheap, deterministic half of
	// not proposing a partition the atomic test would refuse, and the expensive half is the test.
	[[nodiscard]] PixelFormat Layout(TextureId id) const noexcept;

	// Remove every framebuffer no output is reading any more. Called at the top of both dispatch-side
	// verbs, so a card on which nothing is ever promoted walks an empty list.
	void Sweep() noexcept;

	// How many ids name a framebuffer, and how many are waiting for a panel to stop reading them. For
	// a test, which is the only party that can assert on either.
	[[nodiscard]] std::uint32_t Held() const noexcept { return m_Count; }

	[[nodiscard]] std::uint32_t Retiring() const noexcept { return m_DoomedCount; }

private:
	// One framebuffer over one buffer object. The id is kept whole rather than reduced to an index,
	// for Core/Texture.h's reason: a stale generation compares unequal and the lookup misses, where an
	// index alone would resolve to whatever took the slot.
	struct Image
	{
		TextureId Id;
		std::uint32_t Framebuffer = 0;
		std::array<std::uint32_t, MaxImagePlanes> Handles{};
		std::uint32_t PlaneCount = 0;

		// What `AddFB2` was told these pixels are. Kept because the only other way to ask is an ioctl,
		// and the party that asks is inside a frame section — see `Layout`.
		PixelFormat Format{};
	};

	// One GEM handle and how many framebuffers name it. See the class comment: this is the count that
	// keeps two entries over one client buffer from closing each other's handle.
	struct Named
	{
		std::uint32_t Handle = 0;
		std::uint32_t Count = 0;
	};

	// A framebuffer no id names any more, still possibly on a panel.
	struct Doomed
	{
		std::uint32_t Framebuffer = 0;
		std::array<std::uint32_t, MaxImagePlanes> Handles{};
		std::uint32_t PlaneCount = 0;
	};

	// The handle for a descriptor, imported or found. Raises the count either way, so every successful
	// call is matched by exactly one `Release`.
	[[nodiscard]] Result<std::uint32_t> Acquire(RawFd descriptor) noexcept;

	// Drop one reference, closing the buffer object where it was the last.
	void Release(std::uint32_t handle) noexcept;

	void Destroy(std::uint32_t framebuffer, std::span<const std::uint32_t> handles) noexcept;

	[[nodiscard]] bool StillReading(std::uint32_t framebuffer) const noexcept;

	const Fd& m_Device;

	// Dense, order not meaningful. A linear scan per promoted layer per frame is what a lookup costs,
	// which is at most `MaxLayers` scans of a table that is empty on a machine promoting nothing.
	std::array<Image, MaxScanoutImages> m_Images{};
	std::uint32_t m_Count = 0;

	// Dense and at most one entry per plane of every image, which is the worst case where no two
	// images share a buffer object.
	std::array<Named, MaxScanoutImages * MaxImagePlanes> m_Named{};
	std::uint32_t m_NamedCount = 0;

	std::array<Doomed, MaxScanoutImages> m_Doomed{};
	std::uint32_t m_DoomedCount = 0;

	static constexpr std::uint32_t MaxHolds = 8;

	std::array<IScanoutHold*, MaxHolds> m_Holds{};
	std::uint32_t m_HoldCount = 0;
};

} // namespace Drm
