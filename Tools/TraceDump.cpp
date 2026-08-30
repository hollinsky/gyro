// What is actually in a `.pftrace`, printed, and whether it is well formed.
//
// **The reason this exists is that a malformed trace and an empty one look identical in Perfetto.** A
// row nothing recorded on, a track whose descriptor never went out, a slice that never closed, an
// arrow whose far end is not there: every one of them opens without complaint and draws nothing, and
// the only signal is a person staring at a gap. `--check` is that difference made into an exit code.
//
// **`--json` is the other half, and it is there because gyro writes this format by hand.** Reading a
// trace back to answer a question about the frame loop meant writing a throwaway protobuf walker each
// time, and a throwaway walker that misreads a field number does not fail — it reports a stall that is
// not there. This one shares Trace/Protobuf.h's reader and Trace/Schema.h's field numbers with the
// encoder, so what it prints is what gyro wrote or the build does not compile.
//
// Perfetto's own `trace_processor_shell` is the better instrument for open-ended digging and is not
// what this replaces: it offers SQL over the same file and merges a system trace alongside. What it
// will not do is object. A trace whose packets are malformed is a query that returns no rows, which is
// the answer this tool exists to distinguish from a run where nothing happened.
//
// Standalone and outside the module graph, like the other two probes: it takes a file on the command
// line and knows nothing about a compositor.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Trace/Protobuf.h"
#include "Trace/Schema.h"

namespace
{

using namespace Perfetto;

// A track as the file describes it, which is deliberately not what Trace/Perfetto.cpp meant by one.
// The uuid bases are in the schema and reading them here would be the tool agreeing with the encoder
// about a convention rather than reporting the descriptors it found — so a row with no descriptor is
// reported as a row with no descriptor, which is the defect worth catching.
struct Track
{
	std::uint64_t Uuid = 0;
	std::uint64_t Parent = 0;
	std::string Name;
	bool Counter = false;
	bool Described = false;
};

struct Annotated
{
	std::string Name;
	std::uint64_t Value = 0;
};

struct Record
{
	std::int64_t Stamp = 0;
	std::uint64_t Track = 0;
	std::uint64_t Type = 0;
	std::uint64_t Sequence = 0;
	std::string Name;
	double Counter = 0.0;
	bool IsCounter = false;
	std::vector<std::uint64_t> Flows;
	std::vector<Annotated> Annotations;
};

struct Trace
{
	std::map<std::uint64_t, Track> Tracks;
	std::vector<Record> Records;

	// Interning is per packet sequence and is cleared by `StateCleared`, so a name read against the
	// wrong sequence is a name belonging to somebody else rather than a name that is missing.
	std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, std::string>> Names;

	std::vector<std::string> Complaints;
	std::size_t Packets = 0;
	std::size_t Snapshots = 0;
	bool Truncated = false;
};

[[nodiscard]] std::optional<std::vector<std::byte>> ReadFile(const char* path)
{
	std::FILE* file = std::fopen(path, "rb");

	if (file == nullptr)
	{
		return std::nullopt;
	}

	std::vector<std::byte> bytes;
	std::byte chunk[64 * 1024];

	while (const std::size_t read = std::fread(chunk, 1, sizeof(chunk), file))
	{
		bytes.insert(bytes.end(), chunk, chunk + read);
	}

	std::fclose(file);

	return bytes;
}

// The full path of a row, which is how a person names one: a lane belongs to an output belongs to the
// process, and printing only the leaf makes two outputs' `frame` rows indistinguishable.
[[nodiscard]] std::string Path(const Trace& trace, std::uint64_t uuid)
{
	std::string path;

	for (std::uint64_t at = uuid, guard = 0; at != 0 && guard < 16; ++guard)
	{
		const auto found = trace.Tracks.find(at);

		if (found == trace.Tracks.end())
		{
			path = path.empty() ? "?" : "?/" + path;

			break;
		}

		const std::string& name = found->second.Name;

		path = path.empty() ? name : name + "/" + path;
		at = found->second.Parent;
	}

	return path.empty() ? std::string{ "?" } : path;
}

void ReadDescriptor(Trace& trace, std::span<const std::byte> descriptor)
{
	const std::optional<ProtoField> uuid = ProtoFind(descriptor, Descriptor::Uuid);

	if (!uuid)
	{
		trace.Complaints.emplace_back("a track descriptor carries no uuid");

		return;
	}

	Track track;
	track.Uuid = uuid->Value;
	track.Described = true;

	if (const std::optional<ProtoField> parent = ProtoFind(descriptor, Descriptor::ParentUuid); parent)
	{
		track.Parent = parent->Value;
	}

	if (const std::optional<ProtoField> name = ProtoFind(descriptor, Descriptor::Name); name)
	{
		track.Name = ProtoText(*name);
	}

	// A process or thread descriptor carries the name instead, and it is the name that makes the row
	// land beside that thread's scheduling in a merged system trace.
	if (const std::optional<ProtoField> process = ProtoFind(descriptor, Descriptor::Process); process)
	{
		if (const std::optional<ProtoField> name = ProtoFind(process->Bytes, ProcessDescriptor::Name); name)
		{
			track.Name = ProtoText(*name);
		}
	}

	if (const std::optional<ProtoField> thread = ProtoFind(descriptor, Descriptor::Thread); thread)
	{
		if (const std::optional<ProtoField> name = ProtoFind(thread->Bytes, ThreadDescriptor::Name); name)
		{
			track.Name = ProtoText(*name);
		}
	}

	track.Counter = ProtoFind(descriptor, Descriptor::Counter).has_value();

	const auto [at, fresh] = trace.Tracks.insert({ track.Uuid, track });

	// Perfetto keeps the first and drops the second silently, so two descriptors for one uuid is a row
	// that quietly stopped being what it says it is.
	if (!fresh && at->second.Described)
	{
		trace.Complaints.emplace_back(
			"track " + std::to_string(track.Uuid) + " is described twice, as '" + at->second.Name + "' and '" +
			track.Name + "'"
		);
	}
	else if (!fresh)
	{
		at->second = track;
	}
}

void ReadInterned(Trace& trace, std::uint64_t sequence, std::span<const std::byte> interned)
{
	for (const ProtoField& entry : ProtoFindAll(interned, Interned::EventNames))
	{
		const std::optional<ProtoField> iid = ProtoFind(entry.Bytes, Interned::Iid);
		const std::optional<ProtoField> name = ProtoFind(entry.Bytes, Interned::Name);

		if (iid && name)
		{
			trace.Names[sequence][iid->Value] = ProtoText(*name);
		}
	}
}

void ReadEvent(Trace& trace, std::uint64_t sequence, std::int64_t stamp, std::span<const std::byte> event)
{
	Record record;
	record.Stamp = stamp;
	record.Sequence = sequence;

	if (const std::optional<ProtoField> track = ProtoFind(event, Event::TrackUuid); track)
	{
		record.Track = track->Value;
	}

	if (const std::optional<ProtoField> type = ProtoFind(event, Event::Type); type)
	{
		record.Type = type->Value;
	}

	if (const std::optional<ProtoField> name = ProtoFind(event, Event::Name); name)
	{
		record.Name = ProtoText(*name);
	}
	else if (const std::optional<ProtoField> id = ProtoFind(event, Event::NameId); id)
	{
		const auto table = trace.Names.find(sequence);

		if (table == trace.Names.end() || !table->second.contains(id->Value))
		{
			trace.Complaints.emplace_back(
				"name " + std::to_string(id->Value) + " on sequence " + std::to_string(sequence) + " was never interned"
			);
		}
		else
		{
			record.Name = table->second.at(id->Value);
		}
	}

	if (const std::optional<ProtoField> value = ProtoFind(event, Event::CounterValue); value)
	{
		record.IsCounter = true;
		record.Counter = static_cast<double>(static_cast<std::int64_t>(value->Value));
	}

	for (const ProtoField& flow : ProtoFindAll(event, Event::FlowIds))
	{
		record.Flows.push_back(flow.Value);
	}

	for (const ProtoField& annotation : ProtoFindAll(event, Event::DebugAnnotations))
	{
		Annotated held;

		if (const std::optional<ProtoField> name = ProtoFind(annotation.Bytes, Annotation::Name); name)
		{
			held.Name = ProtoText(*name);
		}

		if (const std::optional<ProtoField> value = ProtoFind(annotation.Bytes, Annotation::UintValue); value)
		{
			held.Value = value->Value;
		}

		record.Annotations.push_back(std::move(held));
	}

	// A track a descriptor never covered still gets an entry, so that the walk below has somewhere to
	// hang the event and `--check` has something to name.
	Track placeholder;
	placeholder.Uuid = record.Track;

	trace.Tracks.try_emplace(record.Track, placeholder);
	trace.Records.push_back(std::move(record));
}

[[nodiscard]] Trace Read(std::span<const std::byte> bytes)
{
	Trace trace;
	ProtoReader file{ bytes };

	while (!file.Done())
	{
		const std::optional<ProtoField> packet = file.Next();

		if (!packet)
		{
			// The tail of a file that was cut short, which is worth saying rather than ignoring: what
			// is printed above it is real and what would have followed is gone.
			trace.Truncated = true;

			break;
		}

		if (packet->Number != TracePacket)
		{
			continue;
		}

		++trace.Packets;

		std::uint64_t sequence = 0;

		if (const std::optional<ProtoField> id = ProtoFind(packet->Bytes, Packet::TrustedSequence); id)
		{
			sequence = id->Value;
		}

		if (const std::optional<ProtoField> flags = ProtoFind(packet->Bytes, Packet::SequenceFlags); flags)
		{
			if ((flags->Value & StateCleared) != 0)
			{
				trace.Names[sequence].clear();
			}
		}

		if (ProtoFind(packet->Bytes, Packet::ClockSnapshot))
		{
			++trace.Snapshots;
		}

		if (const std::optional<ProtoField> interned = ProtoFind(packet->Bytes, Packet::InternedData); interned)
		{
			ReadInterned(trace, sequence, interned->Bytes);
		}

		if (const std::optional<ProtoField> descriptor = ProtoFind(packet->Bytes, Packet::TrackDescriptor); descriptor)
		{
			ReadDescriptor(trace, descriptor->Bytes);
		}

		if (const std::optional<ProtoField> event = ProtoFind(packet->Bytes, Packet::TrackEvent); event)
		{
			std::int64_t stamp = 0;

			if (const std::optional<ProtoField> at = ProtoFind(packet->Bytes, Packet::Timestamp); at)
			{
				stamp = static_cast<std::int64_t>(at->Value);
			}

			ReadEvent(trace, sequence, stamp, event->Bytes);
		}
	}

	return trace;
}

// The structural questions, all of which have the same symptom when the answer is wrong — a row that
// draws nothing — and none of which any reader of the file will raise.
[[nodiscard]] std::vector<std::string> Check(const Trace& trace, std::vector<std::string>& notes)
{
	std::vector<std::string> complaints = trace.Complaints;

	if (trace.Truncated)
	{
		complaints.emplace_back("the file ends mid-packet");
	}

	if (trace.Packets == 0)
	{
		complaints.emplace_back("no trace packets at all");
	}

	if (trace.Snapshots == 0)
	{
		// Without one, a reader cannot place these timestamps against a system trace's and drops every
		// packet — the trace opens empty rather than misaligned. See Trace/Perfetto.h.
		complaints.emplace_back("no clock snapshot, so nothing here merges with a system trace");
	}

	for (const auto& [uuid, track] : trace.Tracks)
	{
		if (!track.Described)
		{
			complaints.emplace_back("track " + std::to_string(uuid) + " is used by an event and never described");
		}

		if (track.Parent != 0 && !trace.Tracks.contains(track.Parent))
		{
			complaints.emplace_back(
				"track " + std::to_string(uuid) + " hangs off " + std::to_string(track.Parent) +
				", which is not described"
			);
		}
	}

	// Slice balance per row, walked in the order the file gives, which is the order Perfetto reads it.
	// An end with nothing open is the one that loses the whole row rather than one slice.
	std::unordered_map<std::uint64_t, std::size_t> depth;
	std::unordered_map<std::uint64_t, std::size_t> unmatched;

	for (const Record& record : trace.Records)
	{
		if (record.Type == SliceBegin)
		{
			++depth[record.Track];
		}
		else if (record.Type == SliceEnd)
		{
			if (depth[record.Track] == 0)
			{
				++unmatched[record.Track];
			}
			else
			{
				--depth[record.Track];
			}
		}

		if (record.IsCounter != (record.Type == CounterEvent))
		{
			complaints.emplace_back(
				"a record on " + Path(trace, record.Track) + " is a counter on one field and not the other"
			);
		}

		if (record.Type == SliceBegin && record.Name.empty())
		{
			complaints.emplace_back("an unnamed slice begins on " + Path(trace, record.Track));
		}
	}

	for (const auto& [track, count] : unmatched)
	{
		complaints.emplace_back(std::to_string(count) + " slice end(s) with nothing open on " + Path(trace, track));
	}

	for (const auto& [track, count] : depth)
	{
		if (count != 0)
		{
			complaints.emplace_back(std::to_string(count) + " slice(s) left open on " + Path(trace, track));
		}
	}

	// **An arrow needs two ends, and a one-ended arrow is the ordinary case at the edges of the
	// window.** Each thread's ring lapped independently, so a publication recorded near the start or
	// the end has a partner that fell off somebody else's ring — Trace/Perfetto.h says the same about
	// slices. What separates the two needs no threshold: inside the span where *every* sequence was
	// recording, a missing partner is a missing record. Outside it, it is a ring that had lapped.
	std::int64_t overlapFrom = std::numeric_limits<std::int64_t>::min();
	std::int64_t overlapTo = std::numeric_limits<std::int64_t>::max();

	{
		std::unordered_map<std::uint64_t, std::int64_t> oldest;
		std::unordered_map<std::uint64_t, std::int64_t> newest;

		for (const Record& record : trace.Records)
		{
			const auto [at, fresh] = oldest.try_emplace(record.Sequence, record.Stamp);

			at->second = std::min(at->second, record.Stamp);
			newest[record.Sequence] = std::max(newest[record.Sequence], record.Stamp);
		}

		for (const auto& [sequence, stamp] : oldest)
		{
			overlapFrom = std::max(overlapFrom, stamp);
			overlapTo = std::min(overlapTo, newest[sequence]);
		}
	}

	// A flow as the file describes it: how many records named it, when it was first and last named, and
	// which row it left from — the earliest end being the producing one, which is what a supersession
	// has to be judged against.
	struct Flow
	{
		std::size_t Count = 0;
		std::int64_t First = 0;
		std::int64_t Last = 0;
		std::uint64_t FromTrack = 0;
	};

	std::unordered_map<std::uint64_t, Flow> flows;

	for (const Record& record : trace.Records)
	{
		for (const std::uint64_t id : record.Flows)
		{
			const auto [at, fresh] = flows.try_emplace(id);
			Flow& flow = at->second;

			if (fresh || record.Stamp < flow.First)
			{
				flow.First = record.Stamp;
				flow.FromTrack = record.Track;
			}

			if (fresh)
			{
				flow.Last = record.Stamp;
			}

			flow.Last = std::max(flow.Last, record.Stamp);

			++flow.Count;
		}
	}

	// **The other ordinary one-ended arrow, and it is not a defect either: a producer that counts every
	// attempt feeding a consumer that only ever takes the newest.** `Publication/Ring.h` is exactly
	// that — the dispatch thread numbers and publishes a scene every iteration, and the frame thread
	// acquires whatever is in the slot when it looks, so a scene the next publish overwrote before
	// anybody looked is dropped by design and its `published` mark never gets an `acquired` to point
	// at. Nothing was lost when that happens; the frame thread drew a *newer* picture.
	//
	// What proves it from the file alone, with no name matched and no threshold picked, is that the
	// same chain later carried a *higher* number all the way across. Ids within one chain come from one
	// counter, so a completed arrow numbered above this one, drawn after it, is the consumer saying it
	// had moved past. A chain that simply stops — nothing higher ever completes — is the defect this
	// check is for, and is still reported.
	//
	// A chain is identified by the two rows its completed arrows run between rather than by decoding
	// anything out of the id, for the reason the track walk above gives: the tool reports what the file
	// says and does not agree with the encoder about a convention. That also keeps two producers apart,
	// since only an arrow leaving the *same* row can supersede this one — a one-ended arrow on a
	// consuming row is an arrival with no departure, which nothing supersedes and which stays a
	// complaint.
	std::unordered_map<std::uint64_t, std::vector<std::pair<std::int64_t, std::uint64_t>>> completed;

	for (const auto& [id, flow] : flows)
	{
		if (flow.Count >= 2)
		{
			completed[flow.FromTrack].emplace_back(flow.Last, id);
		}
	}

	// Sorted by when the arrow landed, and carrying the highest id that lands at or after each point,
	// so the question *did this chain get further than here* is one lookup.
	for (auto& [track, arrows] : completed)
	{
		std::sort(arrows.begin(), arrows.end());

		for (std::size_t at = arrows.size(); at-- > 1;)
		{
			arrows[at - 1].second = std::max(arrows[at - 1].second, arrows[at].second);
		}
	}

	const auto superseded = [&completed](const Flow& flow, std::uint64_t id) {
		const auto arrows = completed.find(flow.FromTrack);

		if (arrows == completed.end())
		{
			return false;
		}

		// By landing time alone: the suffix maximum above leaves the ids unsorted, and what is being
		// asked is whether any arrow landed strictly later than this mark.
		const auto after = std::upper_bound(
			arrows->second.begin(),
			arrows->second.end(),
			flow.Last,
			[](std::int64_t stamp, const std::pair<std::int64_t, std::uint64_t>& arrow) { return stamp < arrow.first; }
		);

		return after != arrows->second.end() && after->second > id;
	};

	std::size_t lapped = 0;
	std::size_t dropped = 0;

	for (const auto& [id, flow] : flows)
	{
		if (flow.Count >= 2)
		{
			continue;
		}

		if (flow.First < overlapFrom || flow.First > overlapTo)
		{
			++lapped;

			continue;
		}

		if (superseded(flow, id))
		{
			++dropped;

			continue;
		}

		complaints.emplace_back(
			"flow " + std::to_string(id) + " at " + std::to_string(flow.First) +
			" is named by one slice inside the window every ring covers, and the chain it belongs to got"
			" no further, so no arrow is drawn"
		);
	}

	if (lapped != 0)
	{
		// Said rather than hidden, because a large number of these is a ring that is too short for the
		// thing being looked at rather than a trace that is fine.
		notes.emplace_back(std::to_string(lapped) + " flow(s) lost a partner to a lapped ring at the window edges");
	}

	if (dropped != 0)
	{
		// Same reason, and this one is a figure worth reading on its own: it is how many scenes the
		// dispatch thread built that nobody ever drew.
		notes.emplace_back(std::to_string(dropped) + " flow(s) were superseded before the far side looked");
	}

	// Timestamps must not go backwards within a sequence: Perfetto orders by them and a record out of
	// order is one it discards.
	std::unordered_map<std::uint64_t, std::int64_t> newest;

	for (const Record& record : trace.Records)
	{
		std::int64_t& at = newest[record.Sequence];

		if (record.Stamp < at)
		{
			complaints.emplace_back(
				"sequence " + std::to_string(record.Sequence) + " goes backwards in time at " +
				std::to_string(record.Stamp)
			);

			break;
		}

		at = record.Stamp;
	}

	return complaints;
}

void PrintSummary(const Trace& trace)
{
	std::printf(
		"%zu packets, %zu records, %zu tracks, %zu clock snapshot(s)\n",
		trace.Packets,
		trace.Records.size(),
		trace.Tracks.size(),
		trace.Snapshots
	);

	if (trace.Records.empty())
	{
		return;
	}

	const std::int64_t first = trace.Records.front().Stamp;
	std::int64_t last = first;

	for (const Record& record : trace.Records)
	{
		last = std::max(last, record.Stamp);
	}

	std::printf(
		"window %.3f ms, %lld to %lld\n",
		static_cast<double>(last - first) / 1e6,
		static_cast<long long>(first),
		static_cast<long long>(last)
	);

	// Per row, which is the shape of the question actually being asked: *did this lane record
	// anything, and how much*. A row at zero is the whole reason to run this.
	std::printf("\n%-40s %8s %8s %8s\n", "track", "begin", "instant", "counter");

	for (const auto& [uuid, track] : trace.Tracks)
	{
		std::size_t begins = 0;
		std::size_t instants = 0;
		std::size_t counters = 0;

		for (const Record& record : trace.Records)
		{
			if (record.Track != uuid)
			{
				continue;
			}

			begins += record.Type == SliceBegin ? 1 : 0;
			instants += record.Type == InstantEvent ? 1 : 0;
			counters += record.Type == CounterEvent ? 1 : 0;
		}

		if (begins + instants + counters == 0 && track.Described && !track.Counter)
		{
			std::printf("%-40s %8s %8s %8s   (silent)\n", Path(trace, uuid).c_str(), "-", "-", "-");

			continue;
		}

		std::printf("%-40s %8zu %8zu %8zu\n", Path(trace, uuid).c_str(), begins, instants, counters);
	}
}

// JSON, because the thing done with a dump is a question the dump does not answer — a percentile, a
// gap between two rows, a histogram — and every language has a JSON reader and none of them has one
// for this. One record per line so that the ordinary tools work on it unparsed.
void PrintJson(const Trace& trace)
{
	const auto quote = [](std::string_view text) {
		std::string out = "\"";

		for (const char character : text)
		{
			if (character == '"' || character == '\\')
			{
				out += '\\';
				out += character;
			}
			else if (static_cast<unsigned char>(character) < 0x20)
			{
				char escape[8];
				std::snprintf(escape, sizeof(escape), "\\u%04x", character);
				out += escape;
			}
			else
			{
				out += character;
			}
		}

		return out + "\"";
	};

	for (const Record& record : trace.Records)
	{
		std::string line = "{\"ts\":" + std::to_string(record.Stamp);
		line += ",\"track\":" + quote(Path(trace, record.Track));
		line += ",\"type\":" + std::to_string(record.Type);
		line += ",\"name\":" + quote(record.Name);

		if (record.IsCounter)
		{
			line += ",\"value\":" + std::to_string(static_cast<long long>(record.Counter));
		}

		if (!record.Flows.empty())
		{
			line += ",\"flows\":[";

			for (std::size_t at = 0; at < record.Flows.size(); ++at)
			{
				line += (at == 0 ? "" : ",") + std::to_string(record.Flows[at]);
			}

			line += "]";
		}

		if (!record.Annotations.empty())
		{
			line += ",\"args\":{";

			for (std::size_t at = 0; at < record.Annotations.size(); ++at)
			{
				line += (at == 0 ? "" : ",") + quote(record.Annotations[at].Name) + ":" +
				        std::to_string(record.Annotations[at].Value);
			}

			line += "}";
		}

		line += "}";

		std::printf("%s\n", line.c_str());
	}
}

void Usage()
{
	std::fprintf(
		stderr,
		"TraceDump <trace.pftrace> [--check | --json]\n"
		"\n"
		"  (default)  a summary, and one line per track saying how much landed on it\n"
		"  --check    structural defects only; exit 1 if any. Nothing else reports these\n"
		"  --json     one JSON object per record, oldest first, for whatever asks next\n"
	);
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2 || argc > 3)
	{
		Usage();

		return 2;
	}

	const std::string_view mode = argc == 3 ? std::string_view{ argv[2] } : std::string_view{};

	if (!mode.empty() && mode != "--check" && mode != "--json")
	{
		Usage();

		return 2;
	}

	const std::optional<std::vector<std::byte>> bytes = ReadFile(argv[1]);

	if (!bytes)
	{
		std::fprintf(stderr, "TraceDump: cannot read %s\n", argv[1]);

		return 2;
	}

	const Trace trace = Read(*bytes);

	if (mode == "--json")
	{
		PrintJson(trace);

		return 0;
	}

	if (mode == "--check")
	{
		std::vector<std::string> notes;
		const std::vector<std::string> complaints = Check(trace, notes);

		for (const std::string& note : notes)
		{
			std::printf("note: %s\n", note.c_str());
		}

		// One defect upstream produces one complaint per record it touched — a truncated file loses
		// every arrow in flight — so the list is capped and the remainder is counted rather than
		// dropped: what a reader needs is the *kinds*, and a silent cut would read as a shorter list.
		constexpr std::size_t Shown = 20;

		for (std::size_t at = 0; at < complaints.size() && at < Shown; ++at)
		{
			std::printf("%s\n", complaints[at].c_str());
		}

		if (complaints.size() > Shown)
		{
			std::printf("... and %zu more\n", complaints.size() - Shown);
		}

		if (complaints.empty())
		{
			std::printf(
				"%s: well formed, %zu records on %zu tracks\n", argv[1], trace.Records.size(), trace.Tracks.size()
			);

			return 0;
		}

		return 1;
	}

	PrintSummary(trace);

	return 0;
}
