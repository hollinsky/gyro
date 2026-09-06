#include "Emit.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <format>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "Core/Result.h"
#include "Emit.Client.h"
#include "Emit.Server.h"
#include "Emit.Shared.h"
#include "Naming.h"
#include "Protocol.h"

// The front of the generator: what a set of protocols has to satisfy before a line of either arm
// runs, and the walk that asks the arm the caller named for its files.
//
// **Validation is direction-aware and the emission is not.** A shape the two arms disagree about is
// refused here rather than discovered halfway through a file — `wl_registry.bind` is legal on the
// client arm and inexpressible on the server one — so an arm never has to answer for a message it
// cannot spell.

namespace
{
Result<Catalog> BuildCatalog(std::span<const ProtocolSource> protocols, Direction direction, EmitDiagnostic* diagnostic)
{
	Catalog catalog;

	// Every name this generator puts at namespace scope, in one set. A proxy, a listener, an
	// `Ignoring` and a flattened enumeration all land there, so `wl_output` + `transform` and a
	// hypothetical interface named `wl_output_transform` are the same C++ word and one of them has to
	// lose. Refusing is what turns that into a sentence naming both rather than a redefinition error
	// in a file nobody wrote.
	std::map<std::string, std::string> spellings;

	for (std::size_t index = 0; index < protocols.size(); ++index)
	{
		for (const Interface& interface : protocols[index].Model.Interfaces)
		{
			if (const auto [_, inserted] = catalog.try_emplace(interface.Name, Located{ index, &interface }); !inserted)
			{
				return Refuse(
					diagnostic,
					EmitFailure::NameCollision,
					std::format("two protocols in the set define the interface {}", interface.Name)
				);
			}

			// The classes this direction puts at namespace scope for the interface. They differ: the
			// client arm emits a listener only where there are events to answer, and the server arm
			// emits all four for every interface it emits at all.
			std::vector<std::pair<std::string, std::string>> claimed;

			if (direction == Direction::Server)
			{
				// The two libwayland implements still get a class — `wl_fixes.destroy_registry` takes a
				// `wl_registry` — so the name is still claimed. The three that answer for an interface
				// are not, because that reduced form has none of them.
				claimed.emplace_back(ResourceName(interface.Name), interface.Name);

				if (!IsLibwaylandsOwn(interface))
				{
					claimed.emplace_back(HandlerName(interface.Name), interface.Name);
					claimed.emplace_back(IgnoringName(interface.Name), interface.Name);
					claimed.emplace_back(BindingName(interface.Name), interface.Name);
				}
			}
			else
			{
				claimed.emplace_back(ProxyName(interface.Name), interface.Name);

				if (WantsListener(interface))
				{
					claimed.emplace_back(ListenerName(interface.Name), interface.Name);
					claimed.emplace_back(IgnoringName(interface.Name), interface.Name);
				}
			}

			for (const Enumeration& enumeration : interface.Enumerations)
			{
				claimed.emplace_back(
					EnumName(interface.Name, enumeration.Name), std::format("{}.{}", interface.Name, enumeration.Name)
				);
			}

			for (auto& [spelling, origin] : claimed)
			{
				if (const auto [existing, inserted] = spellings.try_emplace(spelling, origin); !inserted)
				{
					return Refuse(
						diagnostic,
						EmitFailure::NameCollision,
						std::format("{} and {} both spell {}", existing->second, origin, spelling)
					);
				}
			}
		}
	}

	return catalog;
}

Result<void> ValidateMessage(
	const Message& message,
	const Interface& owner,
	bool isEvent,
	Direction direction,
	const Catalog& catalog,
	EmitDiagnostic* diagnostic
)
{
	std::size_t created = 0;

	for (const Argument& argument : message.Arguments)
	{
		if (argument.Kind == ArgumentKind::NewId)
		{
			++created;

			// An untyped `new_id` becomes a template parameter on the message the *sender* writes,
			// which is a thing a caller can supply and a dispatcher cannot: the receiving side would
			// have to name the interface from the wire and instantiate a binding for it, which C++
			// has no form for.
			//
			// Which half that catches flips with the direction, because what a client sends a server
			// receives. The only untyped `new_id` in the published protocols is `wl_registry.bind`,
			// and the server arm never reaches this line for it — libwayland implements the registry,
			// so the interface is skipped before validation begins.
			const bool dispatched = (direction == Direction::Client) == isEvent;

			if (dispatched && argument.Interface.empty())
			{
				return Refuse(
					diagnostic,
					EmitFailure::UntypedEventArgument,
					std::format(
						"the {} {}.{} creates an object of no named interface, which a dispatcher "
						"cannot construct",
						isEvent ? "event" : "request",
						owner.Name,
						message.Name
					)
				);
			}
		}

		if (!argument.Interface.empty() && !catalog.contains(argument.Interface))
		{
			return Refuse(
				diagnostic,
				EmitFailure::UnknownInterface,
				std::format(
					"{}.{} names the interface {}, which no protocol in the set defines; add the "
					"protocol that does",
					owner.Name,
					message.Name,
					argument.Interface
				)
			);
		}

		if (argument.Enumeration.empty())
		{
			continue;
		}

		const auto [interface, enumeration] = SplitEnumeration(argument, owner);
		const auto found = catalog.find(interface);

		if (found == catalog.end())
		{
			return Refuse(
				diagnostic,
				EmitFailure::UnknownInterface,
				std::format(
					"{0}.{1} draws its {2} argument from {3}.{4}, and no protocol in the set defines {3}",
					owner.Name,
					message.Name,
					argument.Name,
					interface,
					enumeration
				)
			);
		}

		const std::vector<Enumeration>& enumerations = found->second.Definition->Enumerations;
		const bool declared = std::ranges::any_of(enumerations, [&](const Enumeration& candidate) {
			return candidate.Name == enumeration;
		});

		if (!declared)
		{
			return Refuse(
				diagnostic,
				EmitFailure::UnknownEnumeration,
				std::format(
					"{0}.{1} draws its {2} argument from {3}.{4}, which {3} does not declare",
					owner.Name,
					message.Name,
					argument.Name,
					interface,
					enumeration
				)
			);
		}
	}

	if (created > 1)
	{
		return Refuse(
			diagnostic,
			EmitFailure::UntypedEventArgument,
			std::format(
				"{}.{} creates more than one object, which a single return value cannot carry", owner.Name, message.Name
			)
		);
	}

	return {};
}

Result<void>
ValidateInterface(const Interface& interface, Direction direction, const Catalog& catalog, EmitDiagnostic* diagnostic)
{
	// **Which half of the interface lands on the class flips with the direction.** A proxy declares a
	// method per *request* and its listener a handler per *event*; a resource declares a method per
	// *event* and its handler one per request. So the same protocol is checked against two different
	// reserved sets, and a name that is a collision on one arm can be perfectly emittable on the
	// other.
	const bool sendingIsRequests = direction == Direction::Client;
	const std::vector<Message>& sent = sendingIsRequests ? interface.Requests : interface.Events;
	const std::vector<Message>& received = sendingIsRequests ? interface.Events : interface.Requests;

	// One namespace of members per class: the messages it sends, and everything the generated class
	// declares for itself.
	std::map<std::string, std::string> members;

	auto claim = [&](std::string spelling, std::string origin) -> Result<void> {
		const bool reserved = sendingIsRequests ? IsReservedProxyMember(spelling) : IsReservedResourceMember(spelling);

		if (reserved)
		{
			return Refuse(
				diagnostic,
				EmitFailure::NameCollision,
				std::format(
					"{} spells {}, which every generated {} already declares",
					origin,
					spelling,
					sendingIsRequests ? "proxy" : "resource"
				)
			);
		}

		if (const auto [existing, inserted] = members.try_emplace(spelling, origin); !inserted)
		{
			return Refuse(
				diagnostic,
				EmitFailure::NameCollision,
				std::format("{} and {} both spell {} on {}", existing->second, origin, spelling, interface.Name)
			);
		}

		return {};
	};

	for (const Enumeration& enumeration : interface.Enumerations)
	{
		// Enumerators share one scope, so two entries that PascalCase alike are a redefinition.
		std::map<std::string, std::string> entries;

		for (const EnumerationEntry& entry : enumeration.Entries)
		{
			if (const auto [existing, inserted] = entries.try_emplace(Pascal(entry.Name), entry.Name); !inserted)
			{
				return Refuse(
					diagnostic,
					EmitFailure::NameCollision,
					std::format(
						"{} and {} in {}.{} both spell {}",
						existing->second,
						entry.Name,
						interface.Name,
						enumeration.Name,
						Pascal(entry.Name)
					)
				);
			}
		}
	}

	for (const Message& message : sent)
	{
		if (Result<void> claimed = claim(
				Pascal(message.Name),
				std::format("the {} {}.{}", sendingIsRequests ? "request" : "event", interface.Name, message.Name)
			);
		    !claimed)
		{
			return claimed;
		}

		if (Result<void> valid =
		        ValidateMessage(message, interface, !sendingIsRequests, direction, catalog, diagnostic);
		    !valid)
		{
			return valid;
		}
	}

	// The receiving half lives on the listener or the handler rather than on the class the caller
	// holds, and carries an `On` prefix, so it collides only with itself.
	std::map<std::string, std::string> handlers;

	for (const Message& message : received)
	{
		if (const auto [existing, inserted] = handlers.try_emplace(Pascal(message.Name), message.Name); !inserted)
		{
			return Refuse(
				diagnostic,
				EmitFailure::NameCollision,
				std::format(
					"the {}s {} and {} on {} both spell On{}",
					sendingIsRequests ? "event" : "request",
					existing->second,
					message.Name,
					interface.Name,
					Pascal(message.Name)
				)
			);
		}

		if (Result<void> valid = ValidateMessage(message, interface, sendingIsRequests, direction, catalog, diagnostic);
		    !valid)
		{
			return valid;
		}
	}

	return {};
}

// Two protocols that name each other's interfaces, which would be two headers each including the
// other. Reported here rather than by the compiler, which would report it as an unknown type in
// generated code and name neither protocol.
Result<void>
ValidateAcyclic(std::span<const ProtocolSource> protocols, const Catalog& catalog, EmitDiagnostic* diagnostic)
{
	for (std::size_t index = 0; index < protocols.size(); ++index)
	{
		for (const std::size_t referenced : References(protocols[index].Model, index, catalog))
		{
			if (References(protocols[referenced].Model, referenced, catalog).contains(index))
			{
				return Refuse(
					diagnostic,
					EmitFailure::CyclicReference,
					std::format(
						"{} and {} reference each other, so neither header can include the other",
						protocols[index].Model.Name,
						protocols[referenced].Model.Name
					)
				);
			}
		}
	}

	return {};
}
} // namespace

Result<std::vector<EmittedFile>>
Emit(std::span<const ProtocolSource> protocols, Direction direction, EmitDiagnostic* diagnostic)
{
	const Result<Catalog> catalog = BuildCatalog(protocols, direction, diagnostic);

	if (!catalog)
	{
		return std::unexpected{ catalog.error() };
	}

	for (const ProtocolSource& source : protocols)
	{
		for (const Interface& interface : source.Model.Interfaces)
		{
			// The two libwayland implements itself are not emitted and so are not checked. Validating
			// them anyway would refuse `wl_registry.bind` for a shape no generated line has to express.
			if (direction == Direction::Server && IsLibwaylandsOwn(interface))
			{
				continue;
			}

			if (Result<void> valid = ValidateInterface(interface, direction, *catalog, diagnostic); !valid)
			{
				return std::unexpected{ valid.error() };
			}
		}
	}

	if (Result<void> acyclic = ValidateAcyclic(protocols, *catalog, diagnostic); !acyclic)
	{
		return std::unexpected{ acyclic.error() };
	}

	std::vector<EmittedFile> files;

	if (direction == Direction::Server)
	{
		files.push_back(EmitFaultHeader());
		files.push_back(EmitFaultSource());
		files.push_back(EmitWeakHeader());
	}

	for (std::size_t index = 0; index < protocols.size(); ++index)
	{
		if (direction == Direction::Server)
		{
			files.push_back(
				EmittedFile{
					.Path = ServerHeaderName(protocols[index]),
					.Text = EmitServerHeader(protocols[index], index, *catalog, protocols),
				}
			);

			files.push_back(
				EmittedFile{
					.Path = std::format("Wayland/Server/{}.cpp", Unit(protocols[index].Path)),
					.Text = EmitServerSource(protocols[index], *catalog),
				}
			);

			continue;
		}

		files.push_back(
			EmittedFile{
				.Path = HeaderName(protocols[index]),
				.Text = EmitClientHeader(protocols[index], index, *catalog, protocols),
			}
		);

		files.push_back(
			EmittedFile{
				.Path = std::format("Wayland/{}.cpp", Unit(protocols[index].Path)),
				.Text = EmitClientSource(protocols[index], *catalog),
			}
		);
	}

	return files;
}
