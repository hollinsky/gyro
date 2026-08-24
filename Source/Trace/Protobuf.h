#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

// Protocol buffer encoding, by hand, because the only thing gyro wants from protobuf is to be read by
// somebody else's tool.
//
// **What is being written is a Perfetto trace, and the alternative was to link Perfetto.** That is a
// large dependency whose in-process tracing path allocates, takes locks, and starts threads of its
// own, none of which the `SCHED_FIFO` frame thread can host — so the emitter would have been fed from
// a ring like Core/Trace.h's anyway, and the library would have been carrying nothing but this file.
// See decision 139.
//
// **Nothing here is general.** There is no schema, no reflection, no field presence, and no decoder:
// the wire format is seven bit groups with a continuation bit and a tag that is a field number and a
// wire type, and Trace/Perfetto.cpp names the field numbers it writes beside the message they belong
// to. A submessage is built into its own writer and appended with its length, which costs a copy per
// nesting level and buys not having to reserve, patch, and re-measure. This runs on the snapshot
// thread against a buffer already in memory, so the copy is free in the only sense that matters.

class ProtoWriter
{
public:
	// proto3 varint, which is also how every enum and every bool travels.
	void Varint(std::uint32_t field, std::uint64_t value)
	{
		Tag(field, 0);
		Raw(value);
	}

	// A signed field is the same varint over the two's complement bits — ten bytes for a negative
	// number, which is protobuf's own inefficiency rather than one introduced here. Nothing zigzags
	// because no field written by this trace is `sint64`.
	void Signed(std::uint32_t field, std::int64_t value) { Varint(field, static_cast<std::uint64_t>(value)); }

	// `fixed64`, which Perfetto uses for flow ids specifically so that a pointer-sized identifier does
	// not cost ten bytes.
	void Fixed64(std::uint32_t field, std::uint64_t value)
	{
		Tag(field, 1);

		for (int shift = 0; shift < 64; shift += 8)
		{
			m_Bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
		}
	}

	void Text(std::uint32_t field, std::string_view value)
	{
		Tag(field, 2);
		Raw(value.size());

		const auto* const bytes = reinterpret_cast<const std::byte*>(value.data());
		m_Bytes.insert(m_Bytes.end(), bytes, bytes + value.size());
	}

	void Nested(std::uint32_t field, const ProtoWriter& message)
	{
		Tag(field, 2);
		Raw(message.m_Bytes.size());
		m_Bytes.insert(m_Bytes.end(), message.m_Bytes.begin(), message.m_Bytes.end());
	}

	void Clear() noexcept { m_Bytes.clear(); }

	[[nodiscard]] std::size_t Size() const noexcept { return m_Bytes.size(); }

	[[nodiscard]] std::span<const std::byte> View() const noexcept { return m_Bytes; }

private:
	void Tag(std::uint32_t field, std::uint32_t wire) { Raw((static_cast<std::uint64_t>(field) << 3) | wire); }

	void Raw(std::uint64_t value)
	{
		while (value >= 0x80U)
		{
			m_Bytes.push_back(static_cast<std::byte>((value & 0x7FU) | 0x80U));
			value >>= 7;
		}

		m_Bytes.push_back(static_cast<std::byte>(value));
	}

	std::vector<std::byte> m_Bytes;
};
