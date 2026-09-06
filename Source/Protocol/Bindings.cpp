#include "Protocol/Bindings.h"

#include <algorithm>
#include <vector>

#include "Core/Clock.h"
#include "Core/Input.h"

void Binding::Press(Instant when) const
{
	if (!Object().IsValid())
	{
		return;
	}

	// **Seconds and nanoseconds rather than the truncated milliseconds the seat sends**, which
	// [Bindings.h](Bindings.h) and the protocol both carry the argument for: this instant is an
	// animation's origin rather than a note about when something happened, and rounding it to a
	// millisecond is rounding where the motion starts. Decision 57's conversion at the edge, exactly as
	// `wp_presentation` does it one file over.
	const std::int64_t nanoseconds = Monotonic::ToNanoseconds(when);
	const std::uint64_t seconds = static_cast<std::uint64_t>(nanoseconds) / 1'000'000'000U;
	const auto remainder = static_cast<std::uint32_t>(static_cast<std::uint64_t>(nanoseconds) % 1'000'000'000U);

	Object().Pressed(
		static_cast<std::uint32_t>(seconds >> 32U), static_cast<std::uint32_t>(seconds & 0xffffffffU), remainder
	);
}

void Binding::OnGone()
{
	if (m_Global != nullptr)
	{
		m_Global->Remove(*this);
	}

	delete this;
}

void BindingsManager::OnGone()
{
	// **Nothing happens to the chords this manager claimed**, which is the protocol's own rule and is
	// what `Binding` holding the global rather than this object already arranges. A shell that has bound
	// everything it wants is entitled to drop the factory.
	if (m_Global != nullptr)
	{
		m_Global->Remove(*this);
	}

	delete this;
}

Wayland::Server::GyroBindingV1Handler*
BindingsManager::OnClaim(Wayland::Server::GyroBindingsV1Modifier modifiers, std::uint32_t keysym)
{
	// The wire enumeration and `KeyModifier` are the same four values, which is what
	// [Keymap.h](Keymap.h) says they are for and what `Bindings.Test.cpp` is what holds together. Bits
	// outside the four are dropped rather than refused: a client that set one asked for a modifier this
	// version has no name for, and the honest answer to that is the chord it did describe.
	const auto claimed = static_cast<KeyModifier>(
		static_cast<std::uint32_t>(modifiers) &
		static_cast<std::uint32_t>(KeyModifier::Shift | KeyModifier::Control | KeyModifier::Alt | KeyModifier::Super)
	);

	if (m_Global == nullptr)
	{
		// The host is going away underneath a client that is still sending. Refusing is the only answer
		// that does not hand back an object nothing will ever match against, and `Create`'s contract
		// makes it a `no_memory` on a connection that is about to close anyway.
		return nullptr;
	}

	auto* const binding = new Binding{ *m_Global, claimed, keysym };

	m_Global->Add(*binding);

	return binding;
}

BindingsGlobal::~BindingsGlobal()
{
	// The host is going away, which happens with the display still up in a test and after it in a run.
	// A manager that outlived this object would call back into freed memory from its `OnGone`.
	for (BindingsManager* const manager : m_Managers)
	{
		manager->Forget();
	}

	for (Binding* const binding : m_Bindings)
	{
		binding->Forget();
	}
}

Wayland::Server::GyroBindingsV1Handler* BindingsGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	auto* const manager = new BindingsManager{ *this };

	Add(*manager);

	return manager;
}

bool BindingsGlobal::Takes(const KeyEvent& event, const Keymap& keymap)
{
	if (!event.Pressed)
	{
		// **A release is answered from what its press did rather than matched again.** The modifiers may
		// well have moved between the two — a person lets go of `Super` before `Space` as often as after
		// — so matching the release would let it through in exactly the case the press was taken, and the
		// window in front would receive half a keystroke.
		const auto at = std::find(m_Taken.begin(), m_Taken.end(), event.Code);

		if (at == m_Taken.end())
		{
			return false;
		}

		m_Taken.erase(at);

		return true;
	}

	const std::uint32_t keysym = keymap.Keysym(event.Code);
	const KeyModifier held = keymap.Held();

	bool matched = false;

	// **Every binding that matches, rather than the first.** [Bindings.h](Bindings.h) carries why there
	// is no ownership of a key here. Over a copy, for the reason every walk that writes to a client in
	// this module takes one: an event is a wire write, and a client whose connection has already failed
	// is torn down inside it — which runs `OnGone` on the very bindings being iterated.
	const std::vector<Binding*> bindings{ m_Bindings.begin(), m_Bindings.end() };

	for (const Binding* const binding : bindings)
	{
		if (!binding->Matches(held, keysym))
		{
			continue;
		}

		binding->Press(event.When);

		matched = true;
	}

	if (matched && std::find(m_Taken.begin(), m_Taken.end(), event.Code) == m_Taken.end())
	{
		m_Taken.push_back(event.Code);
	}

	return matched;
}

void BindingsGlobal::Add(BindingsManager& manager)
{
	m_Managers.push_back(&manager);
}

void BindingsGlobal::Remove(BindingsManager& manager) noexcept
{
	std::erase(m_Managers, &manager);
}

void BindingsGlobal::Add(Binding& binding)
{
	m_Bindings.push_back(&binding);
}

void BindingsGlobal::Remove(Binding& binding) noexcept
{
	std::erase(m_Bindings, &binding);
}
