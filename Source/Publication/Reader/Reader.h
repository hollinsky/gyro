#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "Core/Wake.h"
#include "Publication/Snapshot.h"

// The frame side's view of a published snapshot: wait-free, const, and non-allocating.
//
// Docs/Structure.md#threads-are-a-second-partition names this the frame half of Publication. It is
// the only path the frame thread has into the world, and Docs/Decisions.md decision 45 makes three
// of its properties requirements rather than optimisations: it takes no lock the dispatch thread can
// hold, it never allocates, and it bounds-checks everything it resolves. The first two are why it is
// a view over bytes the publisher already laid out rather than a structure it builds; the third is
// why a malformed run resolves to an *empty span* and never to a read past the end or an abort. The
// frame thread must not take a data-driven crash mid-composite, and a snapshot it cannot make sense
// of is a frame it skips, not a process it fells.
//
// **It reconstitutes, it does not own.** A resolved run is a `std::span<const T>` *into the snapshot
// bytes*, so the frame thread reads coefficients in place and evaluates them there — the zero-copy
// half of publishing coefficients rather than values. The reader holds the byte span it was given
// and a copy of the header it validated; it never copies the runs, and it never writes.
//
// **Offsets, so a rebased mapping still reads.** Every run is resolved by adding a byte offset to the
// span's base, so a snapshot copied to — or mapped at — a different address resolves identically.
// That is decision 49's process option kept alive, and it is pinned by a test that reads the same
// bytes at two addresses.
class SnapshotReader
{
public:
	// The empty reader: no bytes, not valid, every run empty. What the frame thread holds before it
	// has acquired its first snapshot, and what a default-constructed member resolves to without a
	// special case at every call site.
	constexpr SnapshotReader() noexcept = default;

	// Adopt a published snapshot. The bytes are validated once here — base alignment, a header that
	// fits, the magic, the version, and a self-consistent size — and the verdict is remembered, so
	// that the hot accessors below are arithmetic against an already-trusted header rather than a
	// re-validation each call.
	//
	// The header is copied out by value rather than read in place, so that the accessors never
	// reinterpret the head of a span that may not hold a header at all: only after validation passes
	// does anything treat the run bytes as the types they claim to be.
	explicit SnapshotReader(std::span<const std::byte> bytes) noexcept : m_Bytes{ bytes }
	{
		if (bytes.size() < sizeof(SnapshotHeader) || !BaseAligned(bytes))
		{
			return;
		}

		std::memcpy(&m_Header, bytes.data(), sizeof(SnapshotHeader));

		m_Valid =
			m_Header.Magic == SnapshotMagic && m_Header.Version == SnapshotVersion && m_Header.ByteSize == bytes.size();
	}

	// Whether the bytes were a snapshot this reader understands. False is not an error to be handled
	// so much as a frame to be skipped: an unrecognised or truncated mapping yields a valid, empty
	// reader, and every accessor below already answers emptily for it.
	[[nodiscard]] constexpr bool IsValid() const noexcept { return m_Valid; }

	// The snapshot's identity, for the reclamation watermark that will ride the return channel. Zero
	// on an invalid reader, which is the same answer a snapshot that named no sequence would give.
	[[nodiscard]] constexpr std::uint64_t Sequence() const noexcept { return m_Valid ? m_Header.Sequence : 0; }

	// The coefficients of one run, as the type the caller knows they are. Empty unless the reader is
	// valid, the writer's element size and alignment match `T` exactly, and the run lies wholly within
	// the snapshot — the mismatch cases decision 45's bounds-check exists to turn into nothing rather
	// than into a wrong reading.
	//
	// The size and alignment equality is the boundary's type check: the publisher wrote `sizeof(T)`
	// and `alignof(T)` from its own `T`, and a divergence here means the two sides disagree about the
	// record's shape, which must not be resolved as if they agreed.
	template<typename T>
	[[nodiscard]] std::span<const T> Run(SnapshotRun which) const noexcept
	{
		static_assert(
			std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>,
			"A run element is reconstituted from bytes at an offset, so it must be one"
		);

		return m_Valid ? Resolve<T>(m_Header.Runs[RunIndex(which)]) : std::span<const T>{};
	}

	// The per-output wake schedule: one reduced fold per output, in output order. The frame thread
	// reads its schedule from here rather than folding the scene, per Docs/Animation.md#storage.
	[[nodiscard]] std::span<const Wake> Wakes() const noexcept
	{
		return m_Valid ? Resolve<Wake>(m_Header.Wakes) : std::span<const Wake>{};
	}

	// The scene: decision 86's preorder run, one record per node, each naming the length of its own
	// subtree. Empty on the same terms every run above is — an invalid reader, a size or alignment the
	// writer disagrees about, or a run that does not lie wholly inside the snapshot.
	//
	// **Templated for the reason `Run` is, and here it is what keeps the waist below `World`.** The
	// record is `World/Node.h`'s, because `Scene` writes it and `Frame` walks it and neither may name
	// the other (decision 91). This module never names it, so it stays on `Core` and `Geometry` and a
	// field added to a node is not a change to the boundary.
	//
	// The bounds check is worth more here than anywhere else in this file. Every other run is a flat
	// array whose worst misreading is a wrong number; this one is walked as a tree, and decision 90
	// puts a depth counter and a subtree-length check on that walk for the same reason this span is
	// clamped — an unbounded traversal inside the frame section is the most expensive failure the
	// system has.
	template<typename T>
	[[nodiscard]] std::span<const T> Nodes() const noexcept
	{
		static_assert(
			std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>,
			"A node record is reconstituted from bytes at an offset, so it must be one"
		);

		return m_Valid ? Resolve<T>(m_Header.Nodes) : std::span<const T>{};
	}

private:
	// Whether the span's base meets the alignment every element depends on. Checked against the actual
	// address rather than assumed, so that a reader handed an under-aligned mapping refuses it rather
	// than resolving spans the hardware may fault on.
	[[nodiscard]] static bool BaseAligned(std::span<const std::byte> bytes) noexcept
	{
		return reinterpret_cast<std::uintptr_t>(bytes.data()) % SnapshotBaseAlignment == 0;
	}

	// Turn a directory entry into a typed span into the snapshot bytes, or nothing. The element size,
	// the bounds, and the resulting address's alignment are all checked; the reinterpret is
	// well-defined because the publisher created these trivially-copyable objects here by copying their
	// bytes in, which begins their lifetime for an implicit-lifetime type.
	template<typename T>
	[[nodiscard]] std::span<const T> Resolve(const RunEntry& run) const noexcept
	{
		if (run.Count == 0 || run.ElementSize != sizeof(T) || run.ElementAlign != alignof(T) ||
		    !Detail::RunWithinBounds(run, m_Bytes.size()))
		{
			return {};
		}

		const std::byte* const at = m_Bytes.data() + run.Offset;

		if (reinterpret_cast<std::uintptr_t>(at) % alignof(T) != 0)
		{
			return {};
		}

		return std::span<const T>{ reinterpret_cast<const T*>(at), run.Count };
	}

	std::span<const std::byte> m_Bytes{};
	SnapshotHeader m_Header{};
	bool m_Valid = false;
};
