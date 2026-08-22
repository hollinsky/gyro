#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "Core/Wake.h"
#include "Publication/Snapshot.h"

// The dispatch side's builder: it serialises a snapshot and owns the memory until it crosses.
//
// Docs/Structure.md#threads-are-a-second-partition names this the dispatch half of Publication — the
// side that allocates, which Docs/Decisions.md decision 45 requires it to and forbids the reader
// from. Docs/Decisions.md decision 50 makes the active-spring array *the serialisation the publisher
// emits* rather than a mirror maintained beside the nodes, and this is where that serialisation
// happens: coefficients go in, a contiguous offset-addressed snapshot comes out.
//
// **Naive by construction, and Docs/Open.md says to keep it that way for now.** "Publication pacing"
// leaves the arena strategy — eager per commit, paced to an output period, copy-on-write — to be
// settled by measurement, and asks that the first cut be the naive one. So this builder stages each
// run into its own owned buffer and assembles a region at Build. The eventual publisher will
// serialise straight from the live scene into a reused arena; this one is the naive baseline that
// makes the boundary testable before that arena exists.
//
// **Who owns the assembled bytes, and until when, is Publication/Publisher/Outbox.h's.** This file
// only lays them out. The distinction matters because the answer is not "until Build returns": a
// published snapshot belongs to the dispatch side until the frame thread's watermark passes it, which
// is decision 45's deferred reclamation and is the reason the buffer below can be handed back and
// filled again rather than being a fresh allocation each time.

// A published snapshot's backing storage, owned until reclamation.
//
// It carries its bytes over-aligned to SnapshotBaseAlignment so that every run placed at an
// element-aligned offset yields an address the reader can hand back as a typed span. Every byte a
// snapshot occupies is zeroed on reset, which is what makes the header's reserved bytes and any
// inter-run padding zero rather than whatever the allocator or the last snapshot left there — the
// value-initialisation obligation Core/Wake.h and Animation/Solve/Spring.h record for their tail
// padding, discharged for the whole region at once, and discharged again each time the region is
// reused. Retained capacity past the current snapshot is not part of the region and is not cleared
// until a later snapshot reaches it.
class SnapshotBuffer
{
public:
	SnapshotBuffer() = default;

	explicit SnapshotBuffer(std::size_t byteSize) { Reset(byteSize); }

	// **Moved from means empty, and the size is the half that has to be said out loud.** The implicit
	// move would take the vector and *copy* the byte size beside it, leaving a buffer that reports a
	// snapshot's worth of bytes over storage it no longer owns. That is the precise shape of the defect
	// the two-thread soak found in an earlier Outbox — a correctly-sized span over a freed pointer — so
	// the invariant is enforced by the type rather than by every caller remembering not to ask.
	//
	// The self-assignment guard is there for the same reason and not for tidiness: a std::vector
	// self-move is valid-but-unspecified, and unspecified here means the storage goes and the size
	// stays.
	SnapshotBuffer(SnapshotBuffer&& other) noexcept
		: m_Store{ std::move(other.m_Store) }, m_ByteSize{ std::exchange(other.m_ByteSize, 0) }
	{}

	SnapshotBuffer& operator=(SnapshotBuffer&& other) noexcept
	{
		if (this != &other)
		{
			m_Store = std::move(other.m_Store);
			m_ByteSize = std::exchange(other.m_ByteSize, 0);
		}

		return *this;
	}

	// Deep-copying a published snapshot is never what is wanted: the dispatch side hands ownership
	// along, and the frame side reads bytes it does not own at all.
	SnapshotBuffer(const SnapshotBuffer&) = delete;
	SnapshotBuffer& operator=(const SnapshotBuffer&) = delete;

	~SnapshotBuffer() = default;

	// Make the buffer hold a snapshot of the given size, reusing the allocation whenever it is already
	// large enough. Never shrinks, because the whole point of handing a reclaimed buffer back to the
	// publisher is that the next snapshot of about the same size costs no allocator traffic at all.
	//
	// The zeroing is not an optimisation to skip once the publisher writes every byte it declares,
	// because it does not: inter-run alignment padding belongs to no run, and decision 49's shared
	// mapping would make an uninitialised gap somebody else's business.
	//
	// **It zeroes the snapshot, not the allocation.** Only the units this snapshot occupies are cleared,
	// because those are the only bytes Bytes() ever hands out — the retained tail beyond them is
	// unreachable until a later Reset grows the snapshot back over it, and that Reset clears it then. A
	// buffer that once held a burst's worth of runs would otherwise pay for that peak on every publish
	// forever after, on the dispatch thread's serialise-every-commit path, which is exactly the cost the
	// pooling above exists to avoid.
	//
	// **It also never half-succeeds, and that is relied upon.** The growth is the only step here that
	// can fail, and a std::vector resize that throws leaves the vector as it was — so a buffer whose
	// Reset ran out of memory still holds the snapshot it held before, at the size it reported before.
	// Publication/Publisher/Outbox.h's Publish builds a superseding snapshot straight into the deferred
	// one for exactly that reason: out of memory has to mean the older snapshot is still there, not that
	// both are gone. Anything added below the resize must not be able to throw.
	void Reset(std::size_t byteSize)
	{
		const std::size_t units = (byteSize + sizeof(Unit) - 1) / sizeof(Unit);

		if (m_Store.size() < units)
		{
			m_Store.resize(units);
		}

		// Guarded rather than left to memset: a zero-length snapshot over a vector that never allocated
		// would pass data()'s null pointer, which the standard makes undefined for any length at all.
		if (units != 0)
		{
			std::memset(m_Store.data(), 0, units * sizeof(Unit));
		}

		m_ByteSize = byteSize;
	}

	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept
	{
		return { reinterpret_cast<const std::byte*>(m_Store.data()), m_ByteSize };
	}

	[[nodiscard]] std::span<std::byte> Bytes() noexcept
	{
		return { reinterpret_cast<std::byte*>(m_Store.data()), m_ByteSize };
	}

	[[nodiscard]] std::size_t Size() const noexcept { return m_ByteSize; }

	// What the allocation could hold without touching the allocator. Nothing in the design reads this
	// — it is here so a test can prove a reclaimed buffer was reused rather than infer it.
	[[nodiscard]] std::size_t Capacity() const noexcept { return m_Store.size() * sizeof(Unit); }

private:
	// The widest fundamental alignment, so std::vector's allocation meets SnapshotBaseAlignment
	// without an over-aligned allocator.
	using Unit = std::max_align_t;

	std::vector<Unit> m_Store;
	std::size_t m_ByteSize = 0;
};

class SnapshotPublisher
{
public:
	// Stage one run's coefficients. The bytes are copied now, so the caller's storage need not outlive
	// Build — a safety the eventual serialise-in-place publisher will not want, but the right default
	// for a builder assembled from scattered sources in a test or a first cut.
	template<typename T>
	SnapshotPublisher& Put(SnapshotRun which, std::span<const T> elements)
	{
		Stage(m_Runs[RunIndex(which)], elements);
		return *this;
	}

	// Stage the per-output wake schedule, one Wake per output in output order.
	SnapshotPublisher& PutWakes(std::span<const Wake> wakes)
	{
		Stage(m_Wakes, wakes);
		return *this;
	}

	// Stage the scene: decision 86's preorder run, one record per node, each naming the length of its
	// own subtree.
	//
	// **A template, exactly as `Put` is, and that is what keeps `World` out of this module.** The node
	// record is `World/Node.h`'s, because `Scene` writes it and `Frame` walks it and neither may name
	// the other (decision 91). The waist does not need to know that: it carries the run as bytes, a
	// count, and the writer's size and alignment, so a field added to the record touches this file not
	// at all — the same property Publication/Snapshot.h claims for spring coefficients, obtained the
	// same way.
	template<typename T>
	SnapshotPublisher& PutNodes(std::span<const T> nodes)
	{
		Stage(m_Nodes, nodes);
		return *this;
	}

	// Stage the per-output placement, one adapter per output in output order. A template for
	// `PutNodes`' reason: the adapter is `Geometry`'s, the frame half of this module may not name a
	// coordinate, and the waist carries it as bytes either way.
	template<typename T>
	SnapshotPublisher& PutViews(std::span<const T> views)
	{
		Stage(m_Views, views);
		return *this;
	}

	// Stage what the image nodes draw, indexed by a node's `Content`.
	template<typename T>
	SnapshotPublisher& PutImages(std::span<const T> images)
	{
		Stage(m_Images, images);
		return *this;
	}

	// Stage what the solid nodes draw, indexed by a node's `Content`.
	template<typename T>
	SnapshotPublisher& PutSolids(std::span<const T> solids)
	{
		Stage(m_Solids, solids);
		return *this;
	}

	// Assemble the staged runs into one contiguous offset-addressed snapshot, in a buffer the caller
	// owns. The header goes first, then each non-empty run at an offset aligned for its element, then
	// the wake schedule; the directory records where each landed. The result is self-describing: its
	// own ByteSize, the magic, and the version are what the reader validates before trusting a byte of
	// it.
	//
	// **The sequence is an argument rather than a staged field, and that is the point.** It is both the
	// snapshot's identity and the forward ring's ordering, so those must be one number and it must come
	// from the ring — see Publication/Ring.h's NextSequence. A builder that carried its own would make
	// monotonicity a caller obligation, which is the kind of invariant nobody notices breaking until
	// reclamation stops.
	//
	// **`into` is either rebuilt or untouched, never partly either.** Reset's growth is this function's
	// only failure point and it leaves the buffer as it was; everything after it is memcpy into storage
	// that already exists. Outbox's Publish builds into the buffer holding a deferred snapshot, so a
	// weaker guarantee here would lose that snapshot on an allocation failure rather than defer it.
	void Build(SnapshotBuffer& into, std::uint64_t sequence) const
	{
		std::array<std::uint32_t, SnapshotRunCount> offsets{};

		std::size_t cursor = sizeof(SnapshotHeader);
		for (std::size_t index = 0; index < SnapshotRunCount; ++index)
		{
			cursor = Place(m_Runs[index], cursor, offsets[index]);
		}

		std::uint32_t wakeOffset = 0;
		cursor = Place(m_Wakes, cursor, wakeOffset);

		std::uint32_t nodeOffset = 0;
		cursor = Place(m_Nodes, cursor, nodeOffset);

		std::uint32_t viewOffset = 0;
		cursor = Place(m_Views, cursor, viewOffset);

		std::uint32_t imageOffset = 0;
		cursor = Place(m_Images, cursor, imageOffset);

		std::uint32_t solidOffset = 0;
		cursor = Place(m_Solids, cursor, solidOffset);

		const std::size_t byteSize = cursor;

		SnapshotHeader header{};
		header.Sequence = sequence;
		header.ByteSize = static_cast<std::uint32_t>(byteSize);
		for (std::size_t index = 0; index < SnapshotRunCount; ++index)
		{
			header.Runs[index] = Entry(m_Runs[index], offsets[index]);
		}
		header.Wakes = Entry(m_Wakes, wakeOffset);
		header.Nodes = Entry(m_Nodes, nodeOffset);
		header.Views = Entry(m_Views, viewOffset);
		header.Images = Entry(m_Images, imageOffset);
		header.Solids = Entry(m_Solids, solidOffset);

		into.Reset(byteSize);
		const std::span<std::byte> bytes = into.Bytes();

		std::memcpy(bytes.data(), &header, sizeof(SnapshotHeader));
		for (std::size_t index = 0; index < SnapshotRunCount; ++index)
		{
			CopyInto(bytes, offsets[index], m_Runs[index]);
		}
		CopyInto(bytes, wakeOffset, m_Wakes);
		CopyInto(bytes, nodeOffset, m_Nodes);
		CopyInto(bytes, viewOffset, m_Views);
		CopyInto(bytes, imageOffset, m_Images);
		CopyInto(bytes, solidOffset, m_Solids);
	}

	// The same assembly into a buffer nobody had yet. The outbox never takes this path — it always has
	// a reclaimed buffer or a pending one to fill — so it exists for call sites that want one snapshot
	// and no ownership question, which in practice means tests.
	[[nodiscard]] SnapshotBuffer Build(std::uint64_t sequence) const
	{
		SnapshotBuffer buffer;
		Build(buffer, sequence);

		return buffer;
	}

private:
	// One staged run: the element bytes, and the size and alignment the reader will check against the
	// type it resolves them as.
	struct Staged
	{
		std::vector<std::byte> Bytes;
		std::uint32_t Count = 0;
		std::uint32_t ElementSize = 0;
		std::uint32_t ElementAlign = 0;
	};

	template<typename T>
	static void Stage(Staged& run, std::span<const T> elements)
	{
		static_assert(
			std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>,
			"A run element crosses as bytes at an offset, so it must be one"
		);

		run.Count = static_cast<std::uint32_t>(elements.size());
		run.ElementSize = sizeof(T);
		run.ElementAlign = alignof(T);
		run.Bytes.resize(elements.size_bytes());

		if (!elements.empty())
		{
			std::memcpy(run.Bytes.data(), elements.data(), elements.size_bytes());
		}
	}

	// Reserve a run's place at an element-aligned offset and advance the cursor past it. An empty run
	// takes no space and its offset is left at zero — the reader reads its zero count first and never
	// consults the offset.
	static std::size_t Place(const Staged& run, std::size_t cursor, std::uint32_t& offset) noexcept
	{
		if (run.Count == 0)
		{
			offset = 0;
			return cursor;
		}

		const std::size_t at = Detail::AlignUp(cursor, run.ElementAlign);
		offset = static_cast<std::uint32_t>(at);

		return at + run.Bytes.size();
	}

	static RunEntry Entry(const Staged& run, std::uint32_t offset) noexcept
	{
		return { offset, run.Count, run.ElementSize, run.ElementAlign };
	}

	static void CopyInto(std::span<std::byte> bytes, std::uint32_t offset, const Staged& run) noexcept
	{
		if (run.Count != 0)
		{
			std::memcpy(bytes.data() + offset, run.Bytes.data(), run.Bytes.size());
		}
	}

	std::array<Staged, SnapshotRunCount> m_Runs;
	Staged m_Wakes;
	Staged m_Nodes;
	Staged m_Views;
	Staged m_Images;
	Staged m_Solids;
};
