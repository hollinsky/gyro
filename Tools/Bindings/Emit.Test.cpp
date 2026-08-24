#include "Emit.h"

#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Protocol.h"
#include "Testing/Test.h"
#include "Xml.h"

// Two halves, on the same division as Tools/Bindings/Xml.Test.cpp.
//
// The first emits the protocols this machine actually has and asserts on properties of the text that
// no eyeball can hold — chiefly that **every event an interface declares has a case in its
// dispatcher**, across every interface of every protocol at once. That is the whole of decision 2's
// "a generated dispatch table cannot have a hole", and it is the one claim in the generated header's
// own comment that would be a lie if the emitter dropped a case. Counting is what proves it; reading
// the wl_surface dispatcher and nodding does not.
//
// The second half is documents small enough to read in a line, each asserting *which* refusal came
// back. The emitter's refusals are all of the same shape — a protocol somebody is adding does
// something the generated C++ cannot express — so the message has to name the protocol's own
// vocabulary rather than the generator's, and the tests hold it to that.

namespace
{
constexpr std::string_view WaylandXml = GYRO_WAYLAND_DATA_DIR "/wayland.xml";
constexpr std::string_view XdgShellXml = GYRO_WAYLAND_PROTOCOLS_DIR "/stable/xdg-shell/xdg-shell.xml";
constexpr std::string_view LinuxDmabufXml = GYRO_WAYLAND_PROTOCOLS_DIR "/stable/linux-dmabuf/linux-dmabuf-v1.xml";
constexpr std::string_view PresentationTimeXml =
	GYRO_WAYLAND_PROTOCOLS_DIR "/stable/presentation-time/presentation-time.xml";

std::optional<std::string> ReadFile(std::string_view path)
{
	std::ifstream file{ std::string{ path }, std::ios::binary };
	if (!file)
	{
		return std::nullopt;
	}
	return std::string{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
}

// The corpus, parsed and emitted in one call because Tools/Bindings/Emit.h requires it.
struct Generated
{
	std::vector<ProtocolSource> Sources;
	std::vector<EmittedFile> Files;
	std::string Note;

	[[nodiscard]] const std::string* Find(std::string_view path) const
	{
		for (const EmittedFile& file : Files)
		{
			if (file.Path == path)
			{
				return &file.Text;
			}
		}

		return nullptr;
	}
};

Generated Generate(std::span<const std::string_view> paths, Direction direction = Direction::Client)
{
	Generated generated;

	for (const std::string_view path : paths)
	{
		const std::optional<std::string> text = ReadFile(path);

		if (!text.has_value())
		{
			generated.Note = std::format("{} could not be read", path);
			return generated;
		}

		Diagnostic diagnostic;
		Result<Protocol> parsed = ParseProtocol(*text, &diagnostic);

		if (!parsed)
		{
			generated.Note = std::format("{}: {}", path, std::format("{}", diagnostic));
			return generated;
		}

		generated.Sources.push_back(ProtocolSource{ .Model = std::move(*parsed), .Path = std::string{ path } });
	}

	EmitDiagnostic diagnostic;
	Result<std::vector<EmittedFile>> emitted = Emit(generated.Sources, direction, &diagnostic);

	if (!emitted)
	{
		generated.Note = diagnostic.Message;
		return generated;
	}

	generated.Files = std::move(*emitted);

	return generated;
}

// One region of a generated file, from an opening line to whatever closes it. The terminator is a
// parameter because the three shapes worth looking at close differently: a free function at `\n}` in
// column zero, a class at `\n};`, and a template member of a class one tab in.
std::string_view Region(std::string_view source, std::string_view opening, std::string_view closing)
{
	const std::size_t start = source.find(opening);

	if (start == std::string_view::npos)
	{
		return {};
	}

	const std::size_t stop = source.find(closing, start);

	return source.substr(start, stop == std::string_view::npos ? std::string_view::npos : stop - start);
}

std::string_view Body(std::string_view source, std::string_view signature)
{
	return Region(source, signature, "\n}\n");
}

std::size_t Occurrences(std::string_view haystack, std::string_view needle)
{
	std::size_t count = 0;

	for (std::size_t at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + 1))
	{
		++count;
	}

	return count;
}

std::string Pascalled(std::string_view wire)
{
	std::string result;
	bool boundary = true;

	for (const char character : wire)
	{
		if (character == '_')
		{
			boundary = true;
			continue;
		}

		result.push_back(
			boundary && character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A') : character
		);
		boundary = false;
	}

	return result;
}

// A whole document, so a refusal test reads as one line of protocol.
std::string Document(std::string_view name, std::string_view body)
{
	return std::format("<?xml version=\"1.0\"?>\n<protocol name=\"{}\">\n{}\n</protocol>\n", name, body);
}

struct Rejection
{
	bool Refused = false;
	EmitDiagnostic Detail;
};

Rejection Reject(std::span<const std::string> documents, Direction direction = Direction::Client)
{
	std::vector<ProtocolSource> sources;

	for (const std::string& document : documents)
	{
		Result<Protocol> parsed = ParseProtocol(document, nullptr);

		if (!parsed)
		{
			return Rejection{ .Refused = false, .Detail = {} };
		}

		sources.push_back(ProtocolSource{ .Model = std::move(*parsed), .Path = "test.xml" });
	}

	Rejection rejection;
	rejection.Refused = !Emit(sources, direction, &rejection.Detail).has_value();

	return rejection;
}

Rejection Reject(std::string document)
{
	const std::string documents[] = { std::move(document) };

	return Reject(documents);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// The corpus.

GYRO_TEST(Emit, EveryEventHasACaseInItsDispatcher)
{
	// The claim the generated header makes about itself, checked over every interface in the four
	// protocols at once rather than at the one somebody happened to look at. A dropped case would be
	// an event that arrives, matches nothing, and ends the connection — the nested backend losing the
	// host mid-session with no message naming the event that did it.
	const std::string_view paths[] = { WaylandXml, XdgShellXml, LinuxDmabufXml, PresentationTimeXml };
	const Generated generated = Generate(paths);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	std::size_t checked = 0;

	for (const ProtocolSource& source : generated.Sources)
	{
		const std::string* const text = generated.Find(std::format("Wayland/{}.cpp", Pascalled(source.Model.Name)));
		GYRO_REQUIRE(text != nullptr);

		for (const Interface& interface : source.Model.Interfaces)
		{
			// `wl_display` is the one interface with events and no listener, because the wire runtime
			// answers them. Everything else with events has a dispatcher.
			if (interface.Events.empty() || interface.Name == "wl_display")
			{
				continue;
			}

			const std::string_view body = Body(*text, std::format("void {}::Dispatch(", Pascalled(interface.Name)));

			if (body.empty())
			{
				GYRO_FAIL(std::format("{} has events and no dispatcher", interface.Name));
				continue;
			}

			for (std::size_t opcode = 0; opcode < interface.Events.size(); ++opcode)
			{
				if (Occurrences(body, std::format("\t\tcase {}:\n", opcode)) != 1)
				{
					GYRO_FAIL(
						std::format(
							"{}.{} has no case in the dispatcher", interface.Name, interface.Events[opcode].Name
						)
					);
				}
			}

			// And nothing beyond them, which is the other half: a case the protocol does not declare
			// would be an opcode read against the wrong signature.
			GYRO_CHECK_EQ(Occurrences(body, "\t\tcase "), interface.Events.size());

			++checked;
		}
	}

	// A count, so that a Generate() that silently produced nothing cannot pass this test.
	GYRO_CHECK(checked > 20);
}

GYRO_TEST(Emit, EveryEventHasAPureHandlerAndAnIgnorableSibling)
{
	const std::string_view paths[] = { WaylandXml, XdgShellXml };
	const Generated generated = Generate(paths);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	for (const ProtocolSource& source : generated.Sources)
	{
		const std::string* const text = generated.Find(std::format("Wayland/{}.h", Pascalled(source.Model.Name)));
		GYRO_REQUIRE(text != nullptr);

		for (const Interface& interface : source.Model.Interfaces)
		{
			if (interface.Events.empty() || interface.Name == "wl_display")
			{
				continue;
			}

			for (const Message& event : interface.Events)
			{
				// The declaration wraps its parameters where they are long, so the handler is found by
				// its opening rather than by a whole signature.
				const std::string handler = std::format("On{}(", Pascalled(event.Name));

				if (text->find(handler) == std::string::npos)
				{
					GYRO_FAIL(std::format("{}.{} has no handler", interface.Name, event.Name));
				}
			}

			GYRO_CHECK(text->find(std::format("class {}Ignoring", Pascalled(interface.Name))) != std::string::npos);
		}
	}
}

GYRO_TEST(Emit, TheUntypedNewIdIsATemplateThatPutsTheNameAndVersionFirst)
{
	// `wl_registry.bind` is not one id on the wire but three values, and an emitter that read the
	// interface attribute only when it was present would generate a signature short by two and a
	// marshaller that writes the wrong number of words. See Tools/Bindings/Protocol.h.
	const std::string_view paths[] = { WaylandXml };
	const Generated generated = Generate(paths);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const header = generated.Find("Wayland/Wayland.h");
	GYRO_REQUIRE(header != nullptr);

	// A template member closes one tab in, so the region ends at its own brace rather than at the
	// enclosing class's.
	const std::string_view listening = Region(*header, "\t\trequires (requires { typename T::Listener; })", "\n\t}\n");
	const std::string_view silent = Region(*header, "\t\trequires (!requires { typename T::Listener; })", "\n\t}\n");

	GYRO_REQUIRE(!listening.empty());
	GYRO_REQUIRE(!silent.empty());

	// The wire order, and the name coming from the type rather than from a string the caller repeats.
	for (const std::string_view overload : { listening, silent })
	{
		GYRO_CHECK(overload.find("wireWriter.PutUint(name);") != std::string_view::npos);
		GYRO_CHECK(overload.find("wireWriter.PutString(T::WireName);") != std::string_view::npos);
		GYRO_CHECK(overload.find("wireWriter.PutUint(version);") != std::string_view::npos);
		GYRO_CHECK(overload.find("wireWriter.PutNewId(wireId);") != std::string_view::npos);

		// A version the bindings do not describe is caught here rather than sent.
		GYRO_CHECK(overload.find("version > T::WireVersion") != std::string_view::npos);
	}

	// Only the overload for an interface that has events takes one, and it binds before it sends.
	GYRO_CHECK(listening.find("wireCreated.Listen(listener);") != std::string_view::npos);

	// The other one binds too, with no listener behind it: an id that was marshalled and never bound
	// is freed the moment the proxy goes, and the host's `delete_id` for it then lands on a slot
	// nothing is using. See `WlRegion::Listen()`.
	GYRO_CHECK(silent.find("wireCreated.Listen();") != std::string_view::npos);
	GYRO_CHECK(silent.find("Listen(listener)") == std::string_view::npos);
}

GYRO_TEST(Emit, AnObjectAnEventCreatesIsBoundToWhatTheHandlerReturns)
{
	// `zwp_linux_buffer_params_v1.created` hands back a `wl_buffer`, whose `release` event is how a
	// nested backend learns it may reuse a frame. The host allocated that id, so nothing on this side
	// reserved it — the only moment it can be given an implementation is the handler that receives it.
	const std::string_view paths[] = { WaylandXml, LinuxDmabufXml };
	const Generated generated = Generate(paths);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const header = generated.Find("Wayland/LinuxDmabufV1.h");
	const std::string* const source = generated.Find("Wayland/LinuxDmabufV1.cpp");
	GYRO_REQUIRE(header != nullptr);
	GYRO_REQUIRE(source != nullptr);

	GYRO_CHECK(header->find("virtual WlBufferListener* OnCreated(WlBuffer buffer) = 0;") != std::string::npos);

	const std::string_view dispatch = Body(*source, "void ZwpLinuxBufferParamsV1::Dispatch(");
	GYRO_REQUIRE(!dispatch.empty());

	GYRO_CHECK(dispatch.find("wireListener->OnCreated(wireCreated)") != std::string_view::npos);
	GYRO_CHECK(dispatch.find("if (wireImplementation == nullptr)") != std::string_view::npos);
	GYRO_CHECK(dispatch.find("wireCreated.Listen(*wireImplementation);") != std::string_view::npos);

	// And the whole message is demarshalled before the listener is consulted, per Wire/Connection.h's
	// null-`self` contract — the read is above the guard, not below it.
	GYRO_CHECK(
		dispatch.find("const Wire::ObjectId buffer = wireReader.GetNewId();") <
		dispatch.find("if (wireListener == nullptr)")
	);

	// And `Ignoring` does not answer it, because declining an object is not a thing that can be
	// ignored — the null return is a fault.
	const std::string_view ignoring = Region(*header, "class ZwpLinuxBufferParamsV1Ignoring : public", "\n};\n");
	GYRO_REQUIRE(!ignoring.empty());
	GYRO_CHECK(ignoring.find("OnCreated") == std::string_view::npos);
	GYRO_CHECK(ignoring.find("OnFailed") != std::string_view::npos);
}

GYRO_TEST(Emit, ARequestNewerThanTheBoundVersionIsRefusedRatherThanSent)
{
	const std::string_view paths[] = { WaylandXml };
	const Generated generated = Generate(paths);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const source = generated.Find("Wayland/Wayland.cpp");
	GYRO_REQUIRE(source != nullptr);

	// `wl_surface.set_buffer_scale` arrived in version 3. Sending it to an object bound at 1 is a
	// protocol error the host answers by killing the connection, which for the nested backend is every
	// window on the screen going away — so it stops here, as a fault the next Flush reports.
	const std::string_view scale = Body(*source, "void WlSurface::SetBufferScale(");
	GYRO_REQUIRE(!scale.empty());
	GYRO_CHECK(scale.find("if (m_Version < 3)") != std::string_view::npos);
	GYRO_CHECK(scale.find("RecordFault(") != std::string_view::npos);

	// A request that has always existed carries no gate, so the ordinary call is a straight run of
	// Put calls with nothing to branch on.
	const std::string_view commit = Body(*source, "void WlSurface::Commit(");
	GYRO_REQUIRE(!commit.empty());
	GYRO_CHECK(commit.find("m_Version <") == std::string_view::npos);
}

GYRO_TEST(Emit, EnumerationsCarryTheirInterfaceAndBitfieldsGetOperators)
{
	const std::string_view paths[] = { WaylandXml, PresentationTimeXml };
	const Generated generated = Generate(paths);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const header = generated.Find("Wayland/Wayland.h");
	GYRO_REQUIRE(header != nullptr);

	GYRO_CHECK(header->find("enum class WlOutputTransform : std::uint32_t") != std::string::npos);
	GYRO_CHECK(header->find("\t_90 = 1,") != std::string::npos);
	GYRO_CHECK(header->find("\tFlipped90 = 5,") != std::string::npos);

	// A bitfield is not an exclusive set, and a scoped enum has no operators of its own — so the ones
	// a caller needs to say "maximized and activated" are generated beside it.
	GYRO_CHECK(header->find("constexpr WlSeatCapability operator|(") != std::string::npos);
	GYRO_CHECK(header->find("constexpr bool Any(WlSeatCapability value)") != std::string::npos);

	// And an exclusive one gets none of them, which is what makes the distinction worth carrying.
	GYRO_CHECK(header->find("operator|(WlOutputTransform") == std::string::npos);

	const std::string* const presentation = generated.Find("Wayland/PresentationTime.h");
	GYRO_REQUIRE(presentation != nullptr);
	GYRO_CHECK(presentation->find("constexpr WpPresentationFeedbackKind operator|(") != std::string::npos);
}

GYRO_TEST(Emit, ANullableStringIsAnOptionalAndAnEmptyOneIsNot)
{
	// The one argument shape a caller cannot spell without help: the protocol's null string is a
	// length of zero and the empty string is a length of one and a lone NUL, and Wire/Writer.h has a
	// separate verb for each. An optional is what lets a call site choose.
	const std::string_view paths[] = { WaylandXml, XdgShellXml };
	const Generated generated = Generate(paths);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const header = generated.Find("Wayland/Wayland.h");
	const std::string* const source = generated.Find("Wayland/Wayland.cpp");
	GYRO_REQUIRE(header != nullptr);
	GYRO_REQUIRE(source != nullptr);

	// `wl_data_source.set_actions` has no nullable string; `wl_data_offer.accept` does.
	GYRO_CHECK(
		header->find("Accept(std::uint32_t serial, std::optional<std::string_view> mimeType)") != std::string::npos
	);

	const std::string_view accept = Body(*source, "void WlDataOffer::Accept(");
	GYRO_REQUIRE(!accept.empty());
	GYRO_CHECK(accept.find("wireWriter.PutString(*mimeType);") != std::string_view::npos);
	GYRO_CHECK(accept.find("wireWriter.PutNullString();") != std::string_view::npos);

	// A required string stays a view, because there is nothing for an optional to say.
	const std::string_view title = Body(*source, "void XdgToplevel::SetTitle(");
	GYRO_CHECK(title.empty() || title.find("has_value") == std::string_view::npos);
}

GYRO_TEST(Emit, WlDisplayIsEmittedWithoutAListener)
{
	// Its two events are the wire runtime's, per Wire/Connection.h, and `Connection::Bind` refuses
	// the id outright — so a listener generated for it could never be attached to anything.
	const std::string_view paths[] = { WaylandXml };
	const Generated generated = Generate(paths);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const header = generated.Find("Wayland/Wayland.h");
	GYRO_REQUIRE(header != nullptr);

	GYRO_CHECK(header->find("class WlDisplay\n{") != std::string::npos);
	GYRO_CHECK(header->find("class WlDisplayListener") == std::string::npos);

	// The requests are still there, because a client cannot reach a registry without them.
	GYRO_CHECK(header->find("GetRegistry(") != std::string::npos);
	GYRO_CHECK(header->find("Sync(") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// What the emitter refuses.

// --- The server arm ----------------------------------------------------------------------------

GYRO_TEST(Emit, EveryRequestHasASlotInTheDispatchTable)
{
	// Decision 2's second claim, counted rather than read. A hole in the table is a null function
	// pointer libwayland indexes into and aborts on, and an abort here is not one client dying — it is
	// every client of every user on the machine, at a moment a client chose.
	const std::string_view paths[] = { WaylandXml, XdgShellXml, LinuxDmabufXml, PresentationTimeXml };
	const Generated generated = Generate(paths, Direction::Server);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	std::size_t checked = 0;

	for (const ProtocolSource& source : generated.Sources)
	{
		const std::string* const text =
			generated.Find(std::format("Wayland/Server/{}.cpp", Pascalled(source.Model.Name)));
		const std::string* const header =
			generated.Find(std::format("Wayland/Server/{}.h", Pascalled(source.Model.Name)));

		GYRO_REQUIRE(text != nullptr);
		GYRO_REQUIRE(header != nullptr);

		for (const Interface& interface : source.Model.Interfaces)
		{
			// The two libwayland answers itself, which is why they have no table to check.
			if (interface.Name == "wl_display" || interface.Name == "wl_registry")
			{
				continue;
			}

			const std::string_view table =
				Region(*text, std::format("constexpr Wire{}Implementation ", Pascalled(interface.Name)), "\n};\n");

			if (table.empty() && !interface.Requests.empty())
			{
				GYRO_FAIL(std::format("{} has requests and no table", interface.Name));
				continue;
			}

			// One entry per request and nothing else: an entry the protocol does not declare would be
			// a slot libwayland reads a client's opcode against.
			GYRO_CHECK_EQ(Occurrences(table, "\n\t&Wire"), interface.Requests.size());

			for (const Message& request : interface.Requests)
			{
				// No trailing newline: the last entry's is where the region stops.
				const std::string slot =
					std::format("\n\t&Wire{}Request{},", Pascalled(interface.Name), Pascalled(request.Name));

				if (Occurrences(table, slot) != 1)
				{
					GYRO_FAIL(std::format("{}.{} has no slot in the table", interface.Name, request.Name));
				}

				// And the pure virtual it is filled from, so the two lists cannot drift: a request in
				// the table with no handler method would not compile, and this is the other direction.
				const std::string handler = std::format("On{}(", Pascalled(request.Name));

				if (Occurrences(*header, std::format("\tvirtual void {}", handler)) +
				        Occurrences(*header, std::format("Handler* On{}(", Pascalled(request.Name))) ==
				    0)
				{
					GYRO_FAIL(std::format("{}.{} has no pure handler", interface.Name, request.Name));
				}
			}

			++checked;
		}
	}

	GYRO_CHECK(checked > 20);
}

GYRO_TEST(Emit, AResourceIsNeverCreatedWithoutItsImplementation)
{
	// The first of decision 2's two claims, and the one a wrapper is supposed to make unspellable.
	// `wl_resource_create` leaves the implementation null; a request arriving in that window aborts
	// the process. So every call to it in the emitted tree has to sit inside a `Create` that sets one,
	// and there has to be no other way to reach it.
	const std::string_view paths[] = { WaylandXml, XdgShellXml, LinuxDmabufXml, PresentationTimeXml };
	const Generated generated = Generate(paths, Direction::Server);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	std::size_t created = 0;

	for (const ProtocolSource& source : generated.Sources)
	{
		const std::string* const text =
			generated.Find(std::format("Wayland/Server/{}.cpp", Pascalled(source.Model.Name)));
		GYRO_REQUIRE(text != nullptr);

		// Every creation is paired, file by file. Counting both across the whole file is what catches
		// a second creation path being added later without one.
		GYRO_CHECK_EQ(Occurrences(*text, "wl_resource_create("), Occurrences(*text, "wl_resource_set_implementation("));

		for (const Interface& interface : source.Model.Interfaces)
		{
			if (interface.Name == "wl_display" || interface.Name == "wl_registry")
			{
				continue;
			}

			const std::string name = Pascalled(interface.Name);
			const std::string_view body = Body(*text, std::format("{0} {0}::Create(", name));

			if (body.empty())
			{
				GYRO_FAIL(std::format("{} has no typed constructor", interface.Name));
				continue;
			}

			GYRO_CHECK_EQ(Occurrences(body, "wl_resource_create("), std::size_t{ 1 });
			GYRO_CHECK_EQ(Occurrences(body, "wl_resource_set_implementation("), std::size_t{ 1 });

			// Set before the handler is told about the object, and therefore before anything the
			// handler does could reach back into the connection.
			GYRO_CHECK(body.find("wl_resource_set_implementation(") < body.find("handler.m_Object ="));

			++created;
		}
	}

	GYRO_CHECK(created > 20);
}

GYRO_TEST(Emit, AnObjectArgumentIsCheckedBeforeItsImplementationIsRead)
{
	// A client names its own objects in its requests, and the id it names is one it chose — so the
	// `wl_region` in `set_input_region` may be a `wl_buffer`, or an object from another protocol
	// entirely, or one it just destroyed. Reading the user data off it without asking is a type
	// confusion inside gyro that a client can reach on purpose, which is worse than the aborts the
	// rest of this file is about because nothing crashes.
	const std::string_view paths[] = { WaylandXml, XdgShellXml, LinuxDmabufXml, PresentationTimeXml };
	const Generated generated = Generate(paths, Direction::Server);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	std::size_t checked = 0;

	for (const ProtocolSource& source : generated.Sources)
	{
		const std::string* const text =
			generated.Find(std::format("Wayland/Server/{}.cpp", Pascalled(source.Model.Name)));
		GYRO_REQUIRE(text != nullptr);

		for (const Interface& interface : source.Model.Interfaces)
		{
			if (interface.Name == "wl_display" || interface.Name == "wl_registry")
			{
				continue;
			}

			const std::string name = Pascalled(interface.Name);
			const std::string_view body = Body(*text, std::format("{0}Handler* {0}::Implementation(", name));

			if (body.empty())
			{
				GYRO_FAIL(std::format("{} has no way back from an object to its implementation", interface.Name));
				continue;
			}

			// The check is against the interface *and* the dispatch table, which is stricter than the
			// interface alone: a resource of the right interface created by somebody else's bindings
			// is refused rather than reinterpreted as one of these.
			GYRO_CHECK(body.find("wl_resource_instance_of(") != std::string_view::npos);
			GYRO_CHECK(body.find(std::format("&Wire{}Table", name)) != std::string_view::npos);
			GYRO_CHECK(body.find("wl_resource_instance_of(") < body.find("wl_resource_get_user_data("));

			++checked;
		}
	}

	GYRO_CHECK(checked > 20);
}

GYRO_TEST(Emit, AHandlerIsToldWhenTheObjectItWasBuiltForCouldNotBeMade)
{
	// A request that mints an object asks the handler for an implementation and then creates the
	// resource that adopts it. When the allocation fails, `Create` has already ended the client — and
	// the implementation the call site just built belongs to nobody: no resource will ever destroy it
	// and no `OnGone` will ever tell whoever is holding a pointer to it. Every object-minting request
	// in the tree leaks one under memory pressure, which is the moment it can least afford to.
	//
	// `OnGone`'s contract already covers this exactly — the object is not there, and the handler may
	// delete itself — so the fix is to call it rather than to invent a second verb.
	const std::string_view paths[] = { WaylandXml, XdgShellXml };
	const Generated generated = Generate(paths, Direction::Server);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const text = generated.Find("Wayland/Server/Wayland.cpp");
	GYRO_REQUIRE(text != nullptr);

	const std::string_view frame = Body(*text, "void WireWlSurfaceRequestFrame(");
	GYRO_REQUIRE(!frame.empty());

	GYRO_CHECK(frame.find("if (!wireObject.IsValid())") != std::string_view::npos);
	GYRO_CHECK(frame.find("wireImplementation->OnGone();") != std::string_view::npos);

	// And it is every one of them rather than this one: the two counts are the same number because
	// the same emitter writes both halves, and a creation path added without the failure branch shows
	// up here as a mismatch.
	GYRO_CHECK_EQ(
		Occurrences(*text, "if (!wireObject.IsValid())"), Occurrences(*text, "wireImplementation->OnGone();")
	);
	GYRO_CHECK(Occurrences(*text, "wireImplementation->OnGone();") > 5);
}

GYRO_TEST(Emit, AHandlerIsToldTheMomentTheObjectItAnswersForExists)
{
	// Some interfaces owe events before their client has asked anything: `wl_shm` its format list,
	// `wl_output` its geometry and mode. There is no request to hang those on and the factory is the
	// generated trampoline, so without a hook the call site would have to remember a second step after
	// every bind — and forgetting it is a toolkit that waits forever for a list nobody sent.
	//
	// Not pure, unlike `OnGone`: most interfaces owe nothing at creation, and the failure is a client
	// that waits rather than the abort a missing `OnGone` is.
	const std::string_view paths[] = { WaylandXml, XdgShellXml };
	const Generated generated = Generate(paths, Direction::Server);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const header = generated.Find("Wayland/Server/Wayland.h");
	const std::string* const text = generated.Find("Wayland/Server/Wayland.cpp");
	GYRO_REQUIRE(header != nullptr && text != nullptr);

	GYRO_CHECK(header->find("virtual void OnBound() {}") != std::string_view::npos);

	const std::string_view bind = Body(*text, "void WireWlShmBind(");
	GYRO_REQUIRE(!bind.empty());

	// The order is the whole of it: created, checked, and only then told. A hook that ran before the
	// resource existed would be one that could not send anything.
	const std::size_t created = bind.find("WlShm::Create(");
	const std::size_t bound = bind.find("wireImplementation->OnBound();");

	GYRO_REQUIRE(created != std::string_view::npos && bound != std::string_view::npos);
	GYRO_CHECK(created < bound);

	// And on every creation path rather than only the globals — a `new_id` in a request mints objects
	// the same way, and `xdg_toplevel` owes a `configure` the moment it is made.
	GYRO_CHECK(Occurrences(*text, "wireImplementation->OnBound();") > 20);
}

GYRO_TEST(Emit, TheEmittedWireTablesSayWhatTheProtocolSays)
{
	// The one part of the output no compiler checks. libwayland demarshals a client's bytes against
	// these strings, so a wrong one is a request read at the wrong offsets — descriptors landing on
	// the wrong buffer, and nothing detecting it. Rebuilt here from the model rather than compared
	// against a fixture, because a fixture only proves the fixture.
	const std::string_view paths[] = { WaylandXml, XdgShellXml, LinuxDmabufXml };
	const Generated generated = Generate(paths, Direction::Server);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	std::size_t checked = 0;

	for (const ProtocolSource& source : generated.Sources)
	{
		// The core protocol is in the set so that references out of the other two resolve, and it
		// emits no tables of its own: libwayland is built with it and exports them, and a second
		// definition would be a duplicate symbol.
		if (source.Model.Name == "wayland")
		{
			continue;
		}

		const std::string* const text =
			generated.Find(std::format("Wayland/Server/{}.cpp", Pascalled(source.Model.Name)));
		GYRO_REQUIRE(text != nullptr);

		for (const Interface& interface : source.Model.Interfaces)
		{
			for (const std::vector<Message>* messages : { &interface.Requests, &interface.Events })
			{
				for (const Message& message : *messages)
				{
					std::string signature;

					if (message.Since > 1)
					{
						signature += std::to_string(message.Since);
					}

					for (const Argument& argument : message.Arguments)
					{
						if (argument.AllowNull)
						{
							signature += '?';
						}

						// Spelled out rather than taken from the first letter of `Name`, which would give
						// `fixed` and `fd` the same character.
						switch (argument.Kind)
						{
							case ArgumentKind::Int:
								signature += 'i';
								break;
							case ArgumentKind::Uint:
								signature += 'u';
								break;
							case ArgumentKind::Fixed:
								signature += 'f';
								break;
							case ArgumentKind::String:
								signature += 's';
								break;
							case ArgumentKind::Object:
								signature += 'o';
								break;
							case ArgumentKind::NewId:
								signature += 'n';
								break;
							case ArgumentKind::Array:
								signature += 'a';
								break;
							case ArgumentKind::Fd:
								signature += 'h';
								break;
						}
					}

					if (Occurrences(*text, std::format("{{ \"{}\", \"{}\", ", message.Name, signature)) == 0)
					{
						GYRO_FAIL(
							std::format("{}.{} is not described as \"{}\"", interface.Name, message.Name, signature)
						);
					}

					++checked;
				}
			}
		}
	}

	GYRO_CHECK(checked > 60);
}

GYRO_TEST(Emit, TheDisplayAndTheRegistryAreLibwaylandsAndStillHaveAName)
{
	// Decision 2 leaves both to libwayland, so neither gets a handler or a table. They keep a class,
	// because `wl_fixes.destroy_registry` takes a `wl_registry` as an ordinary argument and a
	// generated signature that said `wl_resource*` there would be saying less than the XML did.
	const std::string_view paths[] = { WaylandXml };
	const Generated generated = Generate(paths, Direction::Server);

	if (!generated.Note.empty())
	{
		GYRO_FAIL(generated.Note);
		return;
	}

	const std::string* const header = generated.Find("Wayland/Server/Wayland.h");
	const std::string* const text = generated.Find("Wayland/Server/Wayland.cpp");

	GYRO_REQUIRE(header != nullptr);
	GYRO_REQUIRE(text != nullptr);

	GYRO_CHECK(header->find("class WlRegistry\n") != std::string::npos);
	GYRO_CHECK(header->find("class WlDisplay\n") != std::string::npos);

	// No handler, no binding, no table, and no `Create` — libwayland makes these.
	GYRO_CHECK(header->find("class WlRegistryHandler\n") == std::string::npos);
	GYRO_CHECK(header->find("class WlDisplayHandler\n") == std::string::npos);
	GYRO_CHECK(text->find("WlRegistry WlRegistry::Create(") == std::string::npos);
	GYRO_CHECK(text->find("WireWlRegistryTable") == std::string::npos);

	// And the argument that made them worth naming.
	GYRO_CHECK(header->find("OnDestroyRegistry(WlRegistry registry)") != std::string::npos);
}

GYRO_TEST(Emit, TheUntypedNewIdIsRefusedOnTheArmThatWouldHaveToDispatchIt)
{
	// The same protocol, accepted on one arm and refused on the other. A `new_id` with no interface is
	// three wire values with the type chosen at runtime: the side that *sends* it can name the type as
	// a template argument, and the side that receives it would have to instantiate a class from a
	// string. `wl_registry.bind` is the only one in the published protocols, and the server arm never
	// reaches this because libwayland owns the registry.
	const std::string documents[] = {
		Document(
			"test",
			"<interface name=\"test_thing\" version=\"1\">\n"
			"  <request name=\"bind\"><arg name=\"id\" type=\"new_id\"/></request>\n"
			"</interface>"
		),
	};

	GYRO_CHECK(!Reject(documents, Direction::Client).Refused);

	const Rejection refused = Reject(documents, Direction::Server);

	GYRO_CHECK(refused.Refused);
	GYRO_CHECK_EQ(refused.Detail.Failure, EmitFailure::UntypedEventArgument);
	GYRO_CHECK(refused.Detail.Message.find("the request test_thing.bind") != std::string::npos);
}

GYRO_TEST(Emit, AnEventMayNotLandOnAMemberEveryResourceHas)
{
	// The mirror of the proxy rule, and the point is that it is a *different* list. An event named
	// `create` collides with the typed constructor; a request named `create` is fine, because a
	// request arrives on the handler behind an `On` prefix.
	const std::string collides[] = {
		Document(
			"test",
			"<interface name=\"test_thing\" version=\"1\">\n"
			"  <event name=\"create\"/>\n"
			"</interface>"
		),
	};

	const Rejection refused = Reject(collides, Direction::Server);

	GYRO_CHECK(refused.Refused);
	GYRO_CHECK_EQ(refused.Detail.Failure, EmitFailure::NameCollision);
	GYRO_CHECK(refused.Detail.Message.find("every generated resource already declares") != std::string::npos);

	const std::string fine[] = {
		Document(
			"test",
			"<interface name=\"test_thing\" version=\"1\">\n"
			"  <request name=\"create\"/>\n"
			"</interface>"
		),
	};

	GYRO_CHECK(!Reject(fine, Direction::Server).Refused);
}

GYRO_TEST(Emit, AnInterfaceOutsideTheSetIsNamedRatherThanErased)
{
	// The failure this catches is quiet: emitted as an untyped id, the binding would compile and say
	// less than the XML did, and the reader would never know a type had gone missing.
	const Rejection rejection = Reject(Document(
		"lonely",
		R"(<interface name="lonely_thing" version="1">)"
		R"(<request name="attach"><arg name="buffer" type="object" interface="wl_buffer"/></request>)"
		R"(</interface>)"
	));

	GYRO_REQUIRE(rejection.Refused);
	GYRO_CHECK_EQ(rejection.Detail.Failure, EmitFailure::UnknownInterface);
	GYRO_CHECK(rejection.Detail.Message.find("wl_buffer") != std::string::npos);
	GYRO_CHECK(rejection.Detail.Message.find("lonely_thing.attach") != std::string::npos);
}

GYRO_TEST(Emit, AnEnumReferenceIsResolvedRatherThanAssumed)
{
	const std::string documents[] = {
		Document(
			"host",
			R"(<interface name="host_thing" version="1">)"
			R"(<enum name="transform"><entry name="normal" value="0"/></enum>)"
			R"(</interface>)"
		),
		Document(
			"guest",
			R"(<interface name="guest_thing" version="1">)"
			R"(<request name="go"><arg name="how" type="uint" enum="host_thing.missing"/></request>)"
			R"(</interface>)"
		),
	};

	const Rejection rejection = Reject(documents);

	GYRO_REQUIRE(rejection.Refused);
	GYRO_CHECK_EQ(rejection.Detail.Failure, EmitFailure::UnknownEnumeration);
	GYRO_CHECK(rejection.Detail.Message.find("host_thing.missing") != std::string::npos);
}

GYRO_TEST(Emit, ARequestMayNotLandOnAMemberEveryProxyHas)
{
	// Renaming would be the obvious repair and it is the wrong one: it puts a name in the header that
	// nobody can find in the protocol. Refusing costs whoever adds the protocol a conversation.
	const Rejection rejection =
		Reject(Document("clash", R"(<interface name="clash_thing" version="1"><request name="version"/></interface>)"));

	GYRO_REQUIRE(rejection.Refused);
	GYRO_CHECK_EQ(rejection.Detail.Failure, EmitFailure::NameCollision);
	GYRO_CHECK(rejection.Detail.Message.find("clash_thing.version") != std::string::npos);
}

GYRO_TEST(Emit, TwoNamesThatSpellOneCppWordAreRefused)
{
	// `wl_shell_surface` has a `resize` request beside a `resize` enumeration, which is why an
	// interface-qualified enumeration name is the only spelling. This is the same collision one level
	// up: an interface and another interface's enumeration flattening to one word.
	const Rejection rejection = Reject(Document(
		"flat",
		R"(<interface name="thing" version="1">)"
		R"(<enum name="mode"><entry name="one" value="0"/></enum>)"
		R"(</interface>)"
		R"(<interface name="thing_mode" version="1"><request name="go"/></interface>)"
	));

	GYRO_REQUIRE(rejection.Refused);
	GYRO_CHECK_EQ(rejection.Detail.Failure, EmitFailure::NameCollision);
	GYRO_CHECK(rejection.Detail.Message.find("ThingMode") != std::string::npos);
}

GYRO_TEST(Emit, AnEventThatCreatesAnUnnamedObjectIsRefused)
{
	// A request with an untyped new_id becomes a template and the caller supplies the type. An event
	// cannot: the dispatcher would have to construct a proxy for an interface named on the wire, and
	// C++ has no form for that. No published protocol does it, and this says so at the file that would.
	const Rejection rejection = Reject(Document(
		"anonymous",
		R"(<interface name="anon_thing" version="1">)"
		R"(<event name="made"><arg name="id" type="new_id"/></event>)"
		R"(</interface>)"
	));

	GYRO_REQUIRE(rejection.Refused);
	GYRO_CHECK_EQ(rejection.Detail.Failure, EmitFailure::UntypedEventArgument);
	GYRO_CHECK(rejection.Detail.Message.find("anon_thing.made") != std::string::npos);
}

GYRO_TEST(Emit, TwoProtocolsThatIncludeEachOtherAreRefused)
{
	// Reported here rather than by the compiler, which would report it as an unknown type in a file
	// nobody wrote and name neither protocol.
	const std::string documents[] = {
		Document(
			"left",
			R"(<interface name="left_thing" version="1">)"
			R"(<request name="go"><arg name="other" type="object" interface="right_thing"/></request>)"
			R"(</interface>)"
		),
		Document(
			"right",
			R"(<interface name="right_thing" version="1">)"
			R"(<request name="go"><arg name="other" type="object" interface="left_thing"/></request>)"
			R"(</interface>)"
		),
	};

	const Rejection rejection = Reject(documents);

	GYRO_REQUIRE(rejection.Refused);
	GYRO_CHECK_EQ(rejection.Detail.Failure, EmitFailure::CyclicReference);
	GYRO_CHECK(rejection.Detail.Message.find("left") != std::string::npos);
	GYRO_CHECK(rejection.Detail.Message.find("right") != std::string::npos);
}
