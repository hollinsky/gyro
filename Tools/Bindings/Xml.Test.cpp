#include "Xml.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Protocol.h"
#include "Testing/Test.h"

// Two halves, and they are testing different things.
//
// The first half parses the protocols this machine actually has, against counts taken independently
// from the same files, and then reaches into the two shapes that are invisible unless you look for
// them: `wl_registry.bind`'s interface-less new_id, and an `enum=` pointing at another interface.
// Both are cases where a plausible parser produces a plausible model and the binding built from it
// is wrong on the wire, which is the failure this whole tool exists to make impossible.
//
// The second half is a set of documents small enough to read in one line each, and every one of
// them asserts *which* refusal came back rather than that something did. A parser that rejects
// everything passes a test that only checks for failure, and the reason to be strict here is that
// the message is what somebody reads at 2am when a protocol update breaks the build — so the tests
// have to hold the message to naming the right thing at the right line.

namespace
{
constexpr std::string_view WaylandXml = GYRO_WAYLAND_DATA_DIR "/wayland.xml";
constexpr std::string_view XdgShellXml = GYRO_WAYLAND_PROTOCOLS_DIR "/stable/xdg-shell/xdg-shell.xml";
constexpr std::string_view PresentationTimeXml =
	GYRO_WAYLAND_PROTOCOLS_DIR "/stable/presentation-time/presentation-time.xml";
constexpr std::string_view LinuxDmabufXml = GYRO_WAYLAND_PROTOCOLS_DIR "/stable/linux-dmabuf/linux-dmabuf-v1.xml";
constexpr std::string_view LinuxDrmSyncobjXml =
	GYRO_WAYLAND_PROTOCOLS_DIR "/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml";

std::optional<std::string> ReadFile(std::string_view path)
{
	std::ifstream file{ std::string{ path }, std::ios::binary };
	if (!file)
	{
		return std::nullopt;
	}
	return std::string{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
}

// Everything a count can be taken of, so a protocol that lost an element somewhere in the middle
// fails on the number rather than on a spot check that happened to look elsewhere.
struct Counts
{
	std::size_t Interfaces = 0;
	std::size_t Requests = 0;
	std::size_t Events = 0;
	std::size_t Enumerations = 0;
	std::size_t Entries = 0;
	std::size_t Arguments = 0;
};

Counts Count(const Protocol& protocol)
{
	Counts counts;
	counts.Interfaces = protocol.Interfaces.size();
	for (const Interface& interface : protocol.Interfaces)
	{
		counts.Requests += interface.Requests.size();
		counts.Events += interface.Events.size();
		counts.Enumerations += interface.Enumerations.size();
		for (const Enumeration& enumeration : interface.Enumerations)
		{
			counts.Entries += enumeration.Entries.size();
		}
		for (const std::vector<Message>* messages : { &interface.Requests, &interface.Events })
		{
			for (const Message& message : *messages)
			{
				counts.Arguments += message.Arguments.size();
			}
		}
	}
	return counts;
}

const Interface* Find(const Protocol& protocol, std::string_view name)
{
	const auto found = std::ranges::find(protocol.Interfaces, name, &Interface::Name);
	return (found == protocol.Interfaces.end()) ? nullptr : &*found;
}

const Message* Find(const std::vector<Message>& messages, std::string_view name)
{
	const auto found = std::ranges::find(messages, name, &Message::Name);
	return (found == messages.end()) ? nullptr : &*found;
}

const Enumeration* Find(const std::vector<Enumeration>& enumerations, std::string_view name)
{
	const auto found = std::ranges::find(enumerations, name, &Enumeration::Name);
	return (found == enumerations.end()) ? nullptr : &*found;
}

const EnumerationEntry* Find(const std::vector<EnumerationEntry>& entries, std::string_view name)
{
	const auto found = std::ranges::find(entries, name, &EnumerationEntry::Name);
	return (found == entries.end()) ? nullptr : &*found;
}

// Parses a corpus file, or leaves the diagnostic where a failing test can print it. A missing file
// is a failure rather than a skip: CMake resolved the directory through pkg-config, so a file that
// is not there means the protocol suite moved and the generator is about to be pointed at nothing.
struct Corpus
{
	std::optional<Protocol> Model;
	Diagnostic Detail;
	std::string Note;
};

Corpus Load(std::string_view path)
{
	Corpus corpus;

	const std::optional<std::string> text = ReadFile(path);
	if (!text)
	{
		corpus.Note = std::format("cannot read {}", path);
		return corpus;
	}

	const Result<Protocol> parsed = ParseProtocol(*text, &corpus.Detail);
	if (!parsed)
	{
		corpus.Note = std::format("{}:{}", path, corpus.Detail);
		return corpus;
	}

	corpus.Model = *parsed;
	return corpus;
}

// A document around a fragment, so a fragment test is one line and its line numbers are still
// predictable: the declaration is line 1, `<protocol>` is line 2, and the fragment starts at line 3.
std::string Document(std::string_view body)
{
	return std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<protocol name=\"test\">\n{}\n</protocol>\n", body);
}

// The same, one level deeper: `<interface>` is line 3 and the fragment starts at line 4.
std::string WithinInterface(std::string_view body)
{
	return Document(std::format("<interface name=\"test_thing\" version=\"1\">\n{}\n</interface>", body));
}

struct Rejection
{
	bool Refused = false;
	Diagnostic Detail;
	std::string_view Context;
};

Rejection Reject(std::string_view xml)
{
	Rejection rejection;
	const Result<Protocol> parsed = ParseProtocol(xml, &rejection.Detail);
	rejection.Refused = !parsed.has_value();
	if (!parsed)
	{
		rejection.Context = parsed.error().Context();
	}
	return rejection;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// The corpus.

GYRO_TEST(Xml, WaylandParsesWhole)
{
	const Corpus corpus = Load(WaylandXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	GYRO_CHECK_EQ(corpus.Model->Name, std::string{ "wayland" });

	const Counts counts = Count(*corpus.Model);
	GYRO_CHECK_EQ(counts.Interfaces, std::size_t{ 23 });
	GYRO_CHECK_EQ(counts.Requests, std::size_t{ 71 });
	GYRO_CHECK_EQ(counts.Events, std::size_t{ 61 });
	GYRO_CHECK_EQ(counts.Enumerations, std::size_t{ 26 });
	GYRO_CHECK_EQ(counts.Entries, std::size_t{ 222 });
	GYRO_CHECK_EQ(counts.Arguments, std::size_t{ 213 });

	// The licence notice a generated header has to reproduce. Checked for having survived the parse
	// rather than for its text, which is upstream's to change.
	GYRO_CHECK(corpus.Model->Copyright.starts_with("Copyright"));
	GYRO_CHECK(corpus.Model->Copyright.find("Permission is hereby granted") != std::string::npos);
}

GYRO_TEST(Xml, XdgShellParsesWhole)
{
	const Corpus corpus = Load(XdgShellXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	GYRO_CHECK_EQ(corpus.Model->Name, std::string{ "xdg_shell" });

	const Counts counts = Count(*corpus.Model);
	GYRO_CHECK_EQ(counts.Interfaces, std::size_t{ 5 });
	GYRO_CHECK_EQ(counts.Requests, std::size_t{ 36 });
	GYRO_CHECK_EQ(counts.Events, std::size_t{ 9 });
	GYRO_CHECK_EQ(counts.Enumerations, std::size_t{ 11 });
	GYRO_CHECK_EQ(counts.Entries, std::size_t{ 69 });
	GYRO_CHECK_EQ(counts.Arguments, std::size_t{ 61 });

	// Every interface in xdg-shell moves together, and a version read off the wrong attribute is
	// the defect that silently withholds requests from clients entitled to them.
	for (const Interface& interface : corpus.Model->Interfaces)
	{
		GYRO_CHECK_EQ(interface.Version, 7);
	}
}

GYRO_TEST(Xml, PresentationTimeParsesWhole)
{
	const Corpus corpus = Load(PresentationTimeXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	GYRO_CHECK_EQ(corpus.Model->Name, std::string{ "presentation_time" });

	const Counts counts = Count(*corpus.Model);
	GYRO_CHECK_EQ(counts.Interfaces, std::size_t{ 2 });
	GYRO_CHECK_EQ(counts.Requests, std::size_t{ 2 });
	GYRO_CHECK_EQ(counts.Events, std::size_t{ 4 });
	GYRO_CHECK_EQ(counts.Enumerations, std::size_t{ 2 });
	GYRO_CHECK_EQ(counts.Entries, std::size_t{ 6 });
	GYRO_CHECK_EQ(counts.Arguments, std::size_t{ 11 });

	// The presented event's clock reading is a 64-bit nanosecond count split across three wire
	// words, which is the one place in this protocol an argument list read short would be silent.
	const Interface* feedback = Find(*corpus.Model, "wp_presentation_feedback");
	GYRO_REQUIRE(feedback != nullptr);

	const Message* presented = Find(feedback->Events, "presented");
	GYRO_REQUIRE(presented != nullptr);
	GYRO_CHECK_EQ(presented->Arguments.size(), std::size_t{ 7 });
	GYRO_CHECK(presented->Arguments[0].Kind == ArgumentKind::Uint);
	GYRO_CHECK_EQ(presented->Arguments.back().Enumeration, std::string{ "kind" });
}

GYRO_TEST(Xml, LinuxDmabufParsesWhole)
{
	const Corpus corpus = Load(LinuxDmabufXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	GYRO_CHECK_EQ(corpus.Model->Name, std::string{ "linux_dmabuf_v1" });

	const Counts counts = Count(*corpus.Model);
	GYRO_CHECK_EQ(counts.Interfaces, std::size_t{ 3 });
	GYRO_CHECK_EQ(counts.Requests, std::size_t{ 10 });
	GYRO_CHECK_EQ(counts.Events, std::size_t{ 11 });
	GYRO_CHECK_EQ(counts.Enumerations, std::size_t{ 3 });
	GYRO_CHECK_EQ(counts.Entries, std::size_t{ 14 });
	GYRO_CHECK_EQ(counts.Arguments, std::size_t{ 31 });

	// The file descriptor is the argument this protocol exists for, and it is the one kind that
	// travels out of band rather than in the message body.
	const Interface* params = Find(*corpus.Model, "zwp_linux_buffer_params_v1");
	GYRO_REQUIRE(params != nullptr);

	const Message* add = Find(params->Requests, "add");
	GYRO_REQUIRE(add != nullptr);
	GYRO_REQUIRE(!add->Arguments.empty());
	GYRO_CHECK(add->Arguments[0].Kind == ArgumentKind::Fd);

	// A tranche flag added in a later revision than the enum containing it.
	const Interface* feedback = Find(*corpus.Model, "zwp_linux_dmabuf_feedback_v1");
	GYRO_REQUIRE(feedback != nullptr);
	const Enumeration* flags = Find(feedback->Enumerations, "tranche_flags");
	GYRO_REQUIRE(flags != nullptr);
	GYRO_CHECK(flags->Bitfield);
	const EnumerationEntry* scanout = Find(flags->Entries, "scanout");
	GYRO_REQUIRE(scanout != nullptr);
	GYRO_CHECK_EQ(scanout->Since, 4);
}

GYRO_TEST(Xml, LinuxDrmSyncobjParsesWhole)
{
	const Corpus corpus = Load(LinuxDrmSyncobjXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	GYRO_CHECK_EQ(corpus.Model->Name, std::string{ "linux_drm_syncobj_v1" });

	const Counts counts = Count(*corpus.Model);
	GYRO_CHECK_EQ(counts.Interfaces, std::size_t{ 3 });
	GYRO_CHECK_EQ(counts.Requests, std::size_t{ 7 });
	GYRO_CHECK_EQ(counts.Events, std::size_t{ 0 });
	GYRO_CHECK_EQ(counts.Enumerations, std::size_t{ 2 });
	GYRO_CHECK_EQ(counts.Entries, std::size_t{ 8 });
	GYRO_CHECK_EQ(counts.Arguments, std::size_t{ 10 });

	// An interface with no events at all, which is worth having in the corpus: an emitter that
	// assumes both vectors are non-empty has nowhere else to trip over it.
	const Interface* timeline = Find(*corpus.Model, "wp_linux_drm_syncobj_timeline_v1");
	GYRO_REQUIRE(timeline != nullptr);
	GYRO_CHECK(timeline->Events.empty());

	const Message* destroy = Find(timeline->Requests, "destroy");
	GYRO_REQUIRE(destroy != nullptr);
	GYRO_CHECK(destroy->Destructor);
}

// Every protocol this machine has, which is the test that actually holds the line.
//
// The five above assert counts, and counts only prove the parser did not lose anything in a file
// somebody already looked at. What breaks this build is a protocol suite *update* introducing
// something outside the subset — a new attribute, a construct the DTD grew — and the only way to
// see that on the day it lands rather than on the day gyro binds the new protocol is to read all of
// them. There is no assertion here beyond "it parsed", because the message is the assertion: a
// failure names the file, the line, and what it found.
GYRO_TEST(Xml, TheWholeProtocolSuiteParses)
{
	std::size_t parsed = 0;
	std::error_code failure;

	for (const std::filesystem::directory_entry& entry :
	     std::filesystem::recursive_directory_iterator{ GYRO_WAYLAND_PROTOCOLS_DIR, failure })
	{
		if (!entry.is_regular_file() || entry.path().extension() != ".xml")
		{
			continue;
		}

		const std::string path = entry.path().string();
		const std::optional<std::string> text = ReadFile(path);
		if (!text)
		{
			GYRO_FAIL(std::format("cannot read {}", path));
			continue;
		}

		Diagnostic diagnostic;
		if (const Result<Protocol> protocol = ParseProtocol(*text, &diagnostic); !protocol)
		{
			GYRO_FAIL(std::format("{}:{}", path, diagnostic));
			continue;
		}
		++parsed;
	}

	GYRO_CHECK(!failure);

	// A directory that has moved would otherwise pass by reading nothing at all, which is the one
	// way a sweep can report success for having done no work.
	GYRO_CHECK(parsed > 20);
}

// ---------------------------------------------------------------------------------------------
// The two shapes that are invisible unless you look for them.

// `wl_registry.bind` is the only interface-less new_id in the published protocols, and on the wire
// it is three values rather than one: the interface name as a string, the version as a uint, and
// then the id. An emitter that reads Interface only when it is non-empty generates a demarshaller
// that consumes one word where the client sent four or more, and every subsequent message in that
// buffer is then read at the wrong offset.
GYRO_TEST(Xml, ABindArgumentCarriesNoInterface)
{
	const Corpus corpus = Load(WaylandXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	const Interface* registry = Find(*corpus.Model, "wl_registry");
	GYRO_REQUIRE(registry != nullptr);

	const Message* bind = Find(registry->Requests, "bind");
	GYRO_REQUIRE(bind != nullptr);
	GYRO_REQUIRE_EQ(bind->Arguments.size(), std::size_t{ 2 });

	GYRO_CHECK_EQ(bind->Arguments[0].Name, std::string{ "name" });
	GYRO_CHECK(bind->Arguments[0].Kind == ArgumentKind::Uint);

	GYRO_CHECK_EQ(bind->Arguments[1].Name, std::string{ "id" });
	GYRO_CHECK(bind->Arguments[1].Kind == ArgumentKind::NewId);
	GYRO_CHECK(bind->Arguments[1].Interface.empty());

	// The contrast that makes the absence mean something: every other new_id in the protocol names
	// its interface, so an empty one is a fact about this request rather than about the parser.
	const Interface* display = Find(*corpus.Model, "wl_display");
	GYRO_REQUIRE(display != nullptr);
	const Message* getRegistry = Find(display->Requests, "get_registry");
	GYRO_REQUIRE(getRegistry != nullptr);
	GYRO_REQUIRE_EQ(getRegistry->Arguments.size(), std::size_t{ 1 });
	GYRO_CHECK(getRegistry->Arguments[0].Kind == ArgumentKind::NewId);
	GYRO_CHECK_EQ(getRegistry->Arguments[0].Interface, std::string{ "wl_registry" });
}

// An untyped *object* is the other empty Interface, and it means something else entirely: one word
// on the wire, an id of a type the protocol declines to name. The two must not be conflated, which
// is why both are asserted in the same place.
GYRO_TEST(Xml, AnUntypedObjectIsNotABind)
{
	const Corpus corpus = Load(WaylandXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	const Interface* display = Find(*corpus.Model, "wl_display");
	GYRO_REQUIRE(display != nullptr);

	const Message* error = Find(display->Events, "error");
	GYRO_REQUIRE(error != nullptr);
	GYRO_REQUIRE_EQ(error->Arguments.size(), std::size_t{ 3 });
	GYRO_CHECK(error->Arguments[0].Kind == ArgumentKind::Object);
	GYRO_CHECK(error->Arguments[0].Interface.empty());
}

// `enum=` names an enum on this interface, or on another one, and the dot is the only thing that
// says which. It is carried verbatim because a qualified name routinely points outside the document
// it appears in — an emitter resolving `wl_output.transform` from xdg-output has to go and find
// wayland.xml, which is a decision this parser has no standing to make.
GYRO_TEST(Xml, AnEnumerationReferenceMayBeQualified)
{
	const Corpus corpus = Load(WaylandXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	// The same enum reached both ways in one document, which is the pair that makes the rule
	// unambiguous: `wl_shm.format` declares it and refers to it unqualified; `wl_shm_pool`, a
	// different interface in the same protocol, has to spell the interface out.
	const Interface* shm = Find(*corpus.Model, "wl_shm");
	GYRO_REQUIRE(shm != nullptr);
	const Message* format = Find(shm->Events, "format");
	GYRO_REQUIRE(format != nullptr);
	GYRO_REQUIRE_EQ(format->Arguments.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(format->Arguments[0].Enumeration, std::string{ "format" });

	const Interface* pool = Find(*corpus.Model, "wl_shm_pool");
	GYRO_REQUIRE(pool != nullptr);
	const Message* createBuffer = Find(pool->Requests, "create_buffer");
	GYRO_REQUIRE(createBuffer != nullptr);
	GYRO_REQUIRE_EQ(createBuffer->Arguments.size(), std::size_t{ 6 });
	GYRO_CHECK_EQ(createBuffer->Arguments.back().Enumeration, std::string{ "wl_shm.format" });

	// A qualified reference on a signed argument, which is the combination an emitter is most
	// likely to get wrong: wl_output.transform is declared without a bitfield and read as an int.
	const Interface* surface = Find(*corpus.Model, "wl_surface");
	GYRO_REQUIRE(surface != nullptr);
	const Message* setTransform = Find(surface->Requests, "set_buffer_transform");
	GYRO_REQUIRE(setTransform != nullptr);
	GYRO_REQUIRE_EQ(setTransform->Arguments.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(setTransform->Arguments[0].Enumeration, std::string{ "wl_output.transform" });
	GYRO_CHECK(setTransform->Arguments[0].Kind == ArgumentKind::Int);

	// And an argument that is a plain number, so that a non-empty Enumeration means something.
	const Message* damage = Find(surface->Requests, "damage");
	GYRO_REQUIRE(damage != nullptr);
	GYRO_REQUIRE(!damage->Arguments.empty());
	GYRO_CHECK(damage->Arguments[0].Enumeration.empty());
}

// ---------------------------------------------------------------------------------------------
// The rest of the corpus facts, gathered where they occur.

GYRO_TEST(Xml, TheAttributesThatChangeAGeneratedBinding)
{
	const Corpus corpus = Load(WaylandXml);
	if (!corpus.Model)
	{
		GYRO_FAIL(corpus.Note);
		return;
	}

	// A destructor: the object is gone after this, whatever the shadow object thinks.
	const Interface* pool = Find(*corpus.Model, "wl_shm_pool");
	GYRO_REQUIRE(pool != nullptr);
	const Message* destroy = Find(pool->Requests, "destroy");
	GYRO_REQUIRE(destroy != nullptr);
	GYRO_CHECK(destroy->Destructor);
	GYRO_CHECK(destroy->Arguments.empty());
	GYRO_CHECK_EQ(destroy->Since, 1);

	// A destructor that is an *event*, which is the shape most likely to be assumed away.
	const Interface* callback = Find(*corpus.Model, "wl_callback");
	GYRO_REQUIRE(callback != nullptr);
	GYRO_CHECK(callback->Frozen);
	const Message* done = Find(callback->Events, "done");
	GYRO_REQUIRE(done != nullptr);
	GYRO_CHECK(done->Destructor);

	// A nullable object: attaching a null buffer is how a surface is unmapped, so this is a value
	// the protocol relies on rather than a defensive allowance.
	const Interface* surface = Find(*corpus.Model, "wl_surface");
	GYRO_REQUIRE(surface != nullptr);
	GYRO_CHECK(!surface->Frozen);
	const Message* attach = Find(surface->Requests, "attach");
	GYRO_REQUIRE(attach != nullptr);
	GYRO_REQUIRE_EQ(attach->Arguments.size(), std::size_t{ 3 });
	GYRO_CHECK(attach->Arguments[0].AllowNull);
	GYRO_CHECK_EQ(attach->Arguments[0].Interface, std::string{ "wl_buffer" });
	GYRO_CHECK(!attach->Arguments[1].AllowNull);

	// Deprecation on an event that is still spoken by clients bound below version 8. Both numbers
	// have to survive: the opcode is still in the table, and the comment above it should say so.
	const Interface* pointer = Find(*corpus.Model, "wl_pointer");
	GYRO_REQUIRE(pointer != nullptr);
	const Message* axisDiscrete = Find(pointer->Events, "axis_discrete");
	GYRO_REQUIRE(axisDiscrete != nullptr);
	GYRO_CHECK_EQ(axisDiscrete->Since, 5);
	GYRO_CHECK_EQ(axisDiscrete->DeprecatedSince, 8);

	const Message* motion = Find(pointer->Events, "motion");
	GYRO_REQUIRE(motion != nullptr);
	GYRO_CHECK_EQ(motion->DeprecatedSince, 0);
	GYRO_REQUIRE(!motion->Arguments.empty());
	GYRO_CHECK(motion->Arguments.back().Kind == ArgumentKind::Fixed);

	// A bitfield, and a version on the enum itself rather than on its entries.
	const Interface* manager = Find(*corpus.Model, "wl_data_device_manager");
	GYRO_REQUIRE(manager != nullptr);
	const Enumeration* action = Find(manager->Enumerations, "dnd_action");
	GYRO_REQUIRE(action != nullptr);
	GYRO_CHECK(action->Bitfield);
	GYRO_CHECK_EQ(action->Since, 3);
	GYRO_REQUIRE_EQ(action->Entries.size(), std::size_t{ 4 });
	GYRO_CHECK_EQ(action->Entries[3].Value, std::uint32_t{ 4 });

	// Hexadecimal entry values, which is how every DRM fourcc in the shm format list is written.
	const Interface* shm = Find(*corpus.Model, "wl_shm");
	GYRO_REQUIRE(shm != nullptr);
	const Enumeration* formats = Find(shm->Enumerations, "format");
	GYRO_REQUIRE(formats != nullptr);
	GYRO_CHECK(!formats->Bitfield);
	const EnumerationEntry* xrgb = Find(formats->Entries, "xrgb8888");
	GYRO_REQUIRE(xrgb != nullptr);
	GYRO_CHECK_EQ(xrgb->Value, std::uint32_t{ 1 });
	const EnumerationEntry* uyvy = Find(formats->Entries, "uyvy");
	GYRO_REQUIRE(uyvy != nullptr);
	GYRO_CHECK_EQ(uyvy->Value, std::uint32_t{ 0x59565955 });

	// An entry introduced later than its enum.
	const Interface* keyboard = Find(*corpus.Model, "wl_keyboard");
	GYRO_REQUIRE(keyboard != nullptr);
	const Enumeration* keyState = Find(keyboard->Enumerations, "key_state");
	GYRO_REQUIRE(keyState != nullptr);
	const EnumerationEntry* repeated = Find(keyState->Entries, "repeated");
	GYRO_REQUIRE(repeated != nullptr);
	GYRO_CHECK_EQ(repeated->Since, 10);
	GYRO_CHECK(!repeated->Summary.empty());
}

// ---------------------------------------------------------------------------------------------
// The XML the subset has to handle regardless of what the corpus happens to contain.

GYRO_TEST(Xml, BothQuotingStylesAndEntitiesInAttributes)
{
	const std::string xml = Document(
		R"(<interface name='single_quoted' version='2'>)"
		"\n"
		R"(<description summary="a &lt;tag&gt; &amp; an &apos;apostrophe&apos;">body</description>)"
		"\n"
		R"(<request name='go'><arg name="what" type='string' summary='says &quot;hello&quot;'/></request>)"
		"\n"
		R"(</interface>)"
	);

	Diagnostic diagnostic;
	const Result<Protocol> parsed = ParseProtocol(xml, &diagnostic);
	if (!parsed)
	{
		GYRO_FAIL(std::format("{}", diagnostic));
		return;
	}

	const Interface* interface = Find(*parsed, "single_quoted");
	GYRO_REQUIRE(interface != nullptr);
	GYRO_CHECK_EQ(interface->Version, 2);
	GYRO_CHECK_EQ(interface->Summary, std::string{ "a <tag> & an 'apostrophe'" });

	const Message* go = Find(interface->Requests, "go");
	GYRO_REQUIRE(go != nullptr);
	GYRO_REQUIRE_EQ(go->Arguments.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(go->Arguments[0].Summary, std::string{ "says \"hello\"" });
}

GYRO_TEST(Xml, NumericCharacterReferences)
{
	const std::string xml = WithinInterface(
		R"(<description summary="decimal &#169; hex &#xA9; wide &#x1F600;">&#65;&#x42;C</description>)"
	);

	Diagnostic diagnostic;
	const Result<Protocol> parsed = ParseProtocol(xml, &diagnostic);
	if (!parsed)
	{
		GYRO_FAIL(std::format("{}", diagnostic));
		return;
	}

	const Interface* interface = Find(*parsed, "test_thing");
	GYRO_REQUIRE(interface != nullptr);
	GYRO_CHECK_EQ(interface->Summary, std::string{ "decimal © hex © wide \U0001F600" });
	GYRO_CHECK_EQ(interface->Description, std::string{ "ABC" });
}

GYRO_TEST(Xml, CommentsAreSkippedWhereverTheyAppear)
{
	const std::string xml = "<?xml version=\"1.0\"?>\n"
							"<!-- between the declaration and the root -->\n"
							"<protocol name=\"test\"><!-- inside the root -->\n"
							"<interface name=\"thing\" version=\"1\"><!-- <request name=\"commented_out\"/> -->\n"
							"<request name=\"real\"/><!-- after a request -->\n"
							"<description summary=\"s\">text <!-- inside text --> more</description>\n"
							"</interface></protocol>";

	Diagnostic diagnostic;
	const Result<Protocol> parsed = ParseProtocol(xml, &diagnostic);
	if (!parsed)
	{
		GYRO_FAIL(std::format("{}", diagnostic));
		return;
	}

	const Interface* interface = Find(*parsed, "thing");
	GYRO_REQUIRE(interface != nullptr);
	GYRO_CHECK_EQ(interface->Requests.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(interface->Requests[0].Name, std::string{ "real" });
	GYRO_CHECK_EQ(interface->Description, std::string{ "text  more" });
}

GYRO_TEST(Xml, ADeclarationIsOptionalAndAByteOrderMarkIsNot)
{
	const Result<Protocol> bare = ParseProtocol("<protocol name=\"bare\"></protocol>");
	GYRO_REQUIRE(bare.has_value());
	GYRO_CHECK_EQ(bare->Name, std::string{ "bare" });

	// A protocol with no interfaces parses. The DTD says `interface+`, and this deliberately does
	// not enforce the cardinality: an empty protocol emits an empty binding, which is useless but
	// not wrong, and refusing it would be the parser having an opinion about the emitter's input.
	GYRO_CHECK(bare->Interfaces.empty());

	const Result<Protocol> marked =
		ParseProtocol("\xEF\xBB\xBF<?xml version=\"1.0\"?><protocol name=\"marked\"></protocol>");
	GYRO_REQUIRE(marked.has_value());
	GYRO_CHECK_EQ(marked->Name, std::string{ "marked" });
}

// A generated comment is the only place most people ever read a protocol's prose, so the
// indentation the XML author used to line the paragraph up under its tag has to come off — and the
// relative indentation inside it has to stay, or every list in the wayland protocol becomes one
// run-on sentence.
GYRO_TEST(Xml, DescriptionTextKeepsItsShapeAndLosesItsIndent)
{
	const std::string xml = WithinInterface(
		"<description summary=\"s\">\n"
		"        First paragraph.\n"
		"\n"
		"          - an indented item\n"
		"          - another\n"
		"        Last line.   \n"
		"      </description>"
	);

	Diagnostic diagnostic;
	const Result<Protocol> parsed = ParseProtocol(xml, &diagnostic);
	if (!parsed)
	{
		GYRO_FAIL(std::format("{}", diagnostic));
		return;
	}

	const Interface* interface = Find(*parsed, "test_thing");
	GYRO_REQUIRE(interface != nullptr);
	GYRO_CHECK_EQ(
		interface->Description, std::string{ "First paragraph.\n\n  - an indented item\n  - another\nLast line." }
	);
}

// An attribute and a description both offering a summary. The attribute is the one written against
// this entry, so it wins; without a rule the two would be a coin toss decided by parse order.
GYRO_TEST(Xml, AnEntrySummaryPrefersItsAttribute)
{
	const std::string xml = WithinInterface(
		"<enum name=\"e\">\n"
		"<entry name=\"both\" value=\"1\" summary=\"from the attribute\">"
		"<description summary=\"from the description\">body</description></entry>\n"
		"<entry name=\"described\" value=\"2\">"
		"<description summary=\"from the description\">body</description></entry>\n"
		"</enum>"
	);

	Diagnostic diagnostic;
	const Result<Protocol> parsed = ParseProtocol(xml, &diagnostic);
	if (!parsed)
	{
		GYRO_FAIL(std::format("{}", diagnostic));
		return;
	}

	const Interface* interface = Find(*parsed, "test_thing");
	GYRO_REQUIRE(interface != nullptr);
	GYRO_REQUIRE_EQ(interface->Enumerations.size(), std::size_t{ 1 });

	const EnumerationEntry* both = Find(interface->Enumerations[0].Entries, "both");
	GYRO_REQUIRE(both != nullptr);
	GYRO_CHECK_EQ(both->Summary, std::string{ "from the attribute" });
	GYRO_CHECK_EQ(both->Description, std::string{ "body" });

	const EnumerationEntry* described = Find(interface->Enumerations[0].Entries, "described");
	GYRO_REQUIRE(described != nullptr);
	GYRO_CHECK_EQ(described->Summary, std::string{ "from the description" });
}

// Nothing in the corpus reaches the top bit today. The width is chosen for the bitfield that
// eventually will, and a parser that silently kept it in an int would turn it negative.
GYRO_TEST(Xml, AnEntryValueUsesTheWholeUnsignedRange)
{
	const std::string xml = WithinInterface(
		"<enum name=\"e\" bitfield=\"true\"><entry name=\"top\" value=\"0x80000000\"/>"
		"<entry name=\"all\" value=\"4294967295\"/></enum>"
	);

	Diagnostic diagnostic;
	const Result<Protocol> parsed = ParseProtocol(xml, &diagnostic);
	if (!parsed)
	{
		GYRO_FAIL(std::format("{}", diagnostic));
		return;
	}

	const Interface* interface = Find(*parsed, "test_thing");
	GYRO_REQUIRE(interface != nullptr);
	GYRO_REQUIRE_EQ(interface->Enumerations.size(), std::size_t{ 1 });
	GYRO_REQUIRE_EQ(interface->Enumerations[0].Entries.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(interface->Enumerations[0].Entries[0].Value, std::uint32_t{ 0x80000000 });
	GYRO_CHECK_EQ(interface->Enumerations[0].Entries[1].Value, std::uint32_t{ 0xFFFFFFFF });
}

// ---------------------------------------------------------------------------------------------
// The refusals. Each names the kind and the line, because "it failed" is what a parser that
// rejects every document also does.

GYRO_TEST(Xml, AnElementOutsideTheSubsetIsRefused)
{
	const Rejection rejected = Reject(Document("<extension name=\"vendor\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 3 });
	GYRO_CHECK(rejected.Detail.Message.find("extension") != std::string::npos);

	// The two channels agree: the Error a caller branches on carries the same verdict the
	// diagnostic spells out, so a build tool that logs only one of them is not missing the other.
	GYRO_CHECK_EQ(rejected.Context, Name(ParseFailure::UnexpectedElement));
}

GYRO_TEST(Xml, AKnownElementInTheWrongPlaceIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("<enum name=\"e\">\n<request name=\"nope\"/>\n</enum>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 5 });
	GYRO_CHECK(rejected.Detail.Message.find("<enum>") != std::string::npos);
}

GYRO_TEST(Xml, TheRootMustBeAProtocol)
{
	const Rejection rejected = Reject("<?xml version=\"1.0\"?>\n<interface name=\"lonely\" version=\"1\"/>");
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 2 });
}

// The typo this parser exists to catch. `allow_null` is not `allow-null`, and a parser that skipped
// what it did not recognise would produce a binding that rejects the null buffer wl_surface.attach
// is documented to take — a client unmapping a surface would be killed for a protocol error.
GYRO_TEST(Xml, AMisspeltAttributeIsRefused)
{
	const Rejection rejected = Reject(WithinInterface(
		"<request name=\"go\">\n<arg name=\"b\" type=\"object\" allow_null=\"true\"/>\n"
		"</request>"
	));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedAttribute);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 5 });
	GYRO_CHECK(rejected.Detail.Message.find("allow_null") != std::string::npos);
}

GYRO_TEST(Xml, AnInterfaceWithoutAVersionIsRefused)
{
	const Rejection rejected = Reject(Document("<interface name=\"thing\">\n<request name=\"go\"/>\n</interface>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::MissingAttribute);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 3 });
	GYRO_CHECK(rejected.Detail.Message.find("version") != std::string::npos);
}

GYRO_TEST(Xml, AnArgumentWithoutATypeIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("<request name=\"go\"><arg name=\"x\"/></request>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::MissingAttribute);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
	GYRO_CHECK(rejected.Detail.Message.find("type") != std::string::npos);
}

GYRO_TEST(Xml, ADescriptionWithoutASummaryIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("<description>body</description>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::MissingAttribute);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
}

GYRO_TEST(Xml, AnUnknownArgumentTypeIsRefused)
{
	const Rejection rejected =
		Reject(WithinInterface("<request name=\"go\"><arg name=\"x\" type=\"blob\"/></request>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::InvalidValue);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
	GYRO_CHECK(rejected.Detail.Message.find("blob") != std::string::npos);
}

// "destructor" is the only thing `type=` may say on a message. Anything else read as "not a
// destructor" would leak the shadow object every time the request arrived.
GYRO_TEST(Xml, AMessageTypeOtherThanDestructorIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("<request name=\"go\" type=\"constructor\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::InvalidValue);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
}

GYRO_TEST(Xml, AVersionThatIsNotANumberIsRefused)
{
	const Rejection rejected = Reject(Document("<interface name=\"thing\" version=\"two\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::InvalidValue);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 3 });
}

// `since="0"` is a typo that reads as "always" if it is taken at face value, and there is no
// version zero of an interface for it to have meant.
GYRO_TEST(Xml, AVersionOfZeroIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("<request name=\"go\" since=\"0\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::InvalidValue);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
}

GYRO_TEST(Xml, AnEntryValueWiderThan32BitsIsRefused)
{
	const Rejection rejected =
		Reject(WithinInterface("<enum name=\"e\"><entry name=\"huge\" value=\"0x100000000\"/></enum>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::InvalidValue);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
}

GYRO_TEST(Xml, AFlagThatIsNotTrueOrFalseIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("<enum name=\"e\" bitfield=\"yes\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::InvalidValue);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
}

GYRO_TEST(Xml, AMismatchedCloseTagIsRefused)
{
	const Rejection rejected = Reject(Document("<interface name=\"thing\" version=\"1\">\n</interfce>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::NotWellFormed);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
	GYRO_CHECK(rejected.Detail.Message.find("interfce") != std::string::npos);
}

// The position reported is the *opening* tag, which is the line somebody has to go and fix. An
// unterminated element runs to end of file, so reporting where the parser noticed would point at
// the last line of the document every time.
GYRO_TEST(Xml, AnUnclosedElementIsRefusedAtItsOpeningTag)
{
	const Rejection rejected =
		Reject("<protocol name=\"test\">\n<interface name=\"thing\" version=\"1\">\n<request name=\"go\"/>\n");
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::NotWellFormed);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 2 });
}

GYRO_TEST(Xml, AnUnquotedAttributeValueIsRefused)
{
	const Rejection rejected = Reject(Document("<interface name=thing version=\"1\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::NotWellFormed);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 3 });
	GYRO_CHECK_EQ(rejected.Detail.Column, std::size_t{ 17 });
}

GYRO_TEST(Xml, AnAttributeGivenTwiceIsRefused)
{
	const Rejection rejected = Reject(Document("<interface name=\"thing\" version=\"1\" name=\"again\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::NotWellFormed);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 3 });
}

// The five predefined entities are the only ones there are, because a DOCTYPE is refused and there
// is nowhere else a sixth could have been declared. Expanding an unknown one to nothing would
// silently shorten a summary; expanding it to its own text would put a stray `&` in a header.
GYRO_TEST(Xml, AnUnknownEntityReferenceIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("<description summary=\"nbsp&nbsp;here\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::NotWellFormed);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
	GYRO_CHECK(rejected.Detail.Message.find("nbsp") != std::string::npos);
}

GYRO_TEST(Xml, AnUnclosedCommentIsRefused)
{
	const Rejection rejected = Reject(Document("<!-- this never ends"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::NotWellFormed);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 3 });
}

GYRO_TEST(Xml, ContentAfterTheRootIsRefused)
{
	const Rejection rejected = Reject(Document("") + "<protocol name=\"second\"/>\n");
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::NotWellFormed);
	GYRO_CHECK(rejected.Detail.Message.find("after") != std::string::npos);
}

GYRO_TEST(Xml, TextWhereAnElementBelongsIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("stray prose"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::NotWellFormed);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
}

GYRO_TEST(Xml, MarkupInsideADescriptionIsRefused)
{
	const Rejection rejected = Reject(WithinInterface("<description summary=\"s\">a <b>bold</b> claim</description>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 4 });
}

GYRO_TEST(Xml, TwoDescriptionsOnOneElementAreRefused)
{
	const Rejection rejected =
		Reject(WithinInterface("<description summary=\"first\"/>\n<description summary=\"second\"/>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 5 });
}

// Both are well-formed XML and both are refused, which is the posture worth stating: this is not an
// XML parser that happens to read protocols, it is a protocol reader that happens to use XML. A
// DOCTYPE could declare entities the parser would then resolve wrongly, and a CDATA section is text
// arriving by a route the entity decoder never sees.
GYRO_TEST(Xml, ADocumentTypeDeclarationIsRefused)
{
	const Rejection rejected = Reject(
		"<?xml version=\"1.0\"?>\n<!DOCTYPE protocol SYSTEM \"wayland.dtd\">\n"
		"<protocol name=\"test\"/>"
	);
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 2 });
}

GYRO_TEST(Xml, ACdataSectionIsRefused)
{
	const Rejection rejected = Reject(Document("<![CDATA[<interface name=\"smuggled\" version=\"1\"/>]]>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 3 });
}

GYRO_TEST(Xml, AProcessingInstructionIsRefused)
{
	const Rejection rejected = Reject(Document("<?generator hint=\"skip\"?>"));
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
	GYRO_CHECK_EQ(rejected.Detail.Line, std::size_t{ 3 });
}

GYRO_TEST(Xml, AnEmptyDocumentIsRefused)
{
	const Rejection rejected = Reject("   \n  ");
	GYRO_REQUIRE(rejected.Refused);
	GYRO_CHECK(rejected.Detail.Failure == ParseFailure::UnexpectedElement);
}
