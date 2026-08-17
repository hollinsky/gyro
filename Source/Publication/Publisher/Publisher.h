#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
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
// run into its own owned buffer and assembles a fresh region at Build. The eventual publisher will
// serialise straight from the live scene into a reused arena; this one is the naive baseline that
// makes the boundary testable before that arena exists.

// A published snapshot's backing storage, owned until reclamation.
//
// It carries its bytes over-aligned to SnapshotBaseAlignment so that every run placed at an
// element-aligned offset yields an address the reader can hand back as a typed span. The storage is
// value-initialised, which is what makes the header's reserved bytes and any inter-run padding zero
// rather than whatever the allocator last held — the value-initialisation obligation Core/Wake.h and
// Animation/Solve/Spring.h record for their tail padding, discharged for the whole region at once.
class SnapshotBuffer
{
public:
	SnapshotBuffer() = default;

	explicit SnapshotBuffer(std::size_t byteSize)
		: m_Store((byteSize + sizeof(Unit) - 1) / sizeof(Unit)), m_ByteSize{ byteSize }
	{}

	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept
	{
		return { reinterpret_cast<const std::byte*>(m_Store.data()), m_ByteSize };
	}

	[[nodiscard]] std::span<std::byte> Bytes() noexcept
	{
		return { reinterpret_cast<std::byte*>(m_Store.data()), m_ByteSize };
	}

	[[nodiscard]] std::size_t Size() const noexcept { return m_ByteSize; }

private:
	// The widest fundamental alignment, so std::vector's allocation meets SnapshotBaseAlignment
	// without an over-aligned allocator. A value-initialised vector zeroes its elements, so the region
	// starts clean.
	using Unit = std::max_align_t;

	std::vector<Unit> m_Store;
	std::size_t m_ByteSize = 0;
};

class SnapshotPublisher
{
public:
	// The sequence number the snapshot carries, for the reclamation watermark this module will grow.
	// It is the caller's monotonic publish counter; the builder only records it.
	SnapshotPublisher& Sequence(std::uint64_t sequence) noexcept
	{
		m_Sequence = sequence;
		return *this;
	}

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

	// Assemble the staged runs into one contiguous offset-addressed snapshot. The header goes first,
	// then each non-empty run at an offset aligned for its element, then the wake schedule; the
	// directory records where each landed. The result is self-describing: its own ByteSize, the magic,
	// and the version are what the reader validates before trusting a byte of it.
	[[nodiscard]] SnapshotBuffer Build() const
	{
		std::array<std::uint32_t, SnapshotRunCount> offsets{};

		std::size_t cursor = sizeof(SnapshotHeader);
		for (std::size_t index = 0; index < SnapshotRunCount; ++index)
		{
			cursor = Place(m_Runs[index], cursor, offsets[index]);
		}

		std::uint32_t wakeOffset = 0;
		cursor = Place(m_Wakes, cursor, wakeOffset);

		const std::size_t byteSize = cursor;

		SnapshotHeader header{};
		header.Sequence = m_Sequence;
		header.ByteSize = static_cast<std::uint32_t>(byteSize);
		for (std::size_t index = 0; index < SnapshotRunCount; ++index)
		{
			header.Runs[index] = Entry(m_Runs[index], offsets[index]);
		}
		header.Wakes = Entry(m_Wakes, wakeOffset);

		SnapshotBuffer buffer{ byteSize };
		const std::span<std::byte> bytes = buffer.Bytes();

		std::memcpy(bytes.data(), &header, sizeof(SnapshotHeader));
		for (std::size_t index = 0; index < SnapshotRunCount; ++index)
		{
			CopyInto(bytes, offsets[index], m_Runs[index]);
		}
		CopyInto(bytes, wakeOffset, m_Wakes);

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
	std::uint64_t m_Sequence = 0;
};
