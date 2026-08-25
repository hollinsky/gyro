#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
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
// **Nothing here is general.** There is no schema, no reflection, and no field presence: the wire
// format is seven bit groups with a continuation bit and a tag that is a field number and a wire type,
// and Trace/Schema.h names the field numbers beside the message they belong to. A submessage is built
// into its own writer and appended with its length, which costs a copy per nesting level and buys not
// having to reserve, patch, and re-measure. This runs on the snapshot thread against a buffer already
// in memory, so the copy is free in the only sense that matters.
//
// **There is a reader, and it is a walker rather than a decoder.** It knows wire types and nothing
// about the schema, so every question is phrased as *find this field number inside that message*. It
// is here rather than in a test because gyro writes this format by hand and nothing else checks it:
// the same walk answers the module's own tests and backs `Tools/TraceDump.cpp`, which is what keeps
// the file gyro emits and the file something reads back from being two descriptions that drift.

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

// One field as it lies in the buffer, with its payload left where it is: a submessage is a span into
// the same bytes rather than a copy, so walking a nested message costs nothing but another reader.
struct ProtoField
{
	std::uint32_t Number = 0;
	std::uint32_t Wire = 0;

	// Wire types 0, 1 and 5 — a varint, and the two fixed widths read little endian into the same
	// place, because a caller that knows the field knows which of the three it asked for.
	std::uint64_t Value = 0;

	// Wire type 2, which is every string and every submessage.
	std::span<const std::byte> Bytes;
};

// **A truncated or malformed message stops the walk rather than throwing.** The one caller that is not
// a test is reading a file somebody handed it, and half a trace is still worth printing — so `Next`
// answering nothing means *this message ends here*, and the caller decides whether that is the end of
// the buffer or a defect worth reporting.
class ProtoReader
{
public:
	explicit ProtoReader(std::span<const std::byte> bytes) noexcept : m_Bytes{ bytes } {}

	[[nodiscard]] bool Done() const noexcept { return m_At >= m_Bytes.size(); }

	// How far in the walk has got, which is what lets a caller tell a message that ended from one that
	// stopped: a reader that is not `Done` and answered nothing has bytes it could not read.
	[[nodiscard]] std::size_t Offset() const noexcept { return m_At; }

	[[nodiscard]] std::optional<ProtoField> Next() noexcept
	{
		const std::optional<std::uint64_t> tag = Varint();

		if (!tag)
		{
			return std::nullopt;
		}

		ProtoField field{ .Number = static_cast<std::uint32_t>(*tag >> 3),
			              .Wire = static_cast<std::uint32_t>(*tag & 7U),
			              .Value = 0,
			              .Bytes = {} };

		// Field zero is not a legal field number, and it is the shape a run of zero padding takes.
		// Accepting it turns a buffer that was zero filled into an endless sequence of empty varints.
		if (field.Number == 0)
		{
			return std::nullopt;
		}

		if (field.Wire == 0)
		{
			const std::optional<std::uint64_t> value = Varint();

			if (!value)
			{
				return std::nullopt;
			}

			field.Value = *value;

			return field;
		}

		if (field.Wire == 1 || field.Wire == 5)
		{
			// Five is `fixed32`, which nothing here writes and a system trace concatenated in front of
			// this one does. Skipping it by the wrong width would not lose that field, it would lose
			// every field after it.
			const std::size_t width = field.Wire == 1 ? 8U : 4U;

			if (m_At + width > m_Bytes.size())
			{
				return std::nullopt;
			}

			for (std::size_t byte = 0; byte < width; ++byte)
			{
				field.Value |= static_cast<std::uint64_t>(m_Bytes[m_At++]) << (byte * 8);
			}

			return field;
		}

		if (field.Wire != 2)
		{
			// Three and four are proto2's groups, which have no length and can only be skipped by
			// understanding them, and six and seven were never assigned. Any of them means the walk has
			// lost the framing rather than met a field it can ignore.
			return std::nullopt;
		}

		const std::optional<std::uint64_t> length = Varint();

		if (!length || *length > m_Bytes.size() - m_At)
		{
			return std::nullopt;
		}

		field.Bytes = m_Bytes.subspan(m_At, static_cast<std::size_t>(*length));
		m_At += static_cast<std::size_t>(*length);

		return field;
	}

private:
	[[nodiscard]] std::optional<std::uint64_t> Varint() noexcept
	{
		std::uint64_t value = 0;

		for (int shift = 0; shift < 64; shift += 7)
		{
			if (m_At >= m_Bytes.size())
			{
				return std::nullopt;
			}

			const std::uint64_t byte = static_cast<std::uint64_t>(m_Bytes[m_At++]);

			value |= (byte & 0x7FU) << shift;

			if ((byte & 0x80U) == 0)
			{
				return value;
			}
		}

		return std::nullopt;
	}

	std::span<const std::byte> m_Bytes;
	std::size_t m_At = 0;
};

// The first field with this number, which is the whole of what a schema-free walker offers. A repeated
// field is read with a `ProtoReader` directly, because the caller is the only party that knows one
// field number can arrive more than once.
[[nodiscard]] inline std::optional<ProtoField> ProtoFind(std::span<const std::byte> message, std::uint32_t number)
{
	ProtoReader reader{ message };

	while (!reader.Done())
	{
		const std::optional<ProtoField> field = reader.Next();

		if (!field)
		{
			return std::nullopt;
		}

		if (field->Number == number)
		{
			return field;
		}
	}

	return std::nullopt;
}

// Every field with this number, in the order they were written, which is the order a repeated field
// means.
[[nodiscard]] inline std::vector<ProtoField> ProtoFindAll(std::span<const std::byte> message, std::uint32_t number)
{
	std::vector<ProtoField> found;
	ProtoReader reader{ message };

	while (!reader.Done())
	{
		const std::optional<ProtoField> field = reader.Next();

		if (!field)
		{
			break;
		}

		if (field->Number == number)
		{
			found.push_back(*field);
		}
	}

	return found;
}

// A length-delimited field's bytes as the string they are. Not `string_view` onto a temporary: every
// caller here is building a record that outlives the walk.
[[nodiscard]] inline std::string_view ProtoText(const ProtoField& field) noexcept
{
	return std::string_view{ reinterpret_cast<const char*>(field.Bytes.data()), field.Bytes.size() };
}
