#pragma once

#include <cstdint>
#include <string_view>

// Who may see a global, and what a connection is trusted with.
//
// **`wl_registry` advertisement is a function of the connection rather than a static table**, which
// Docs/Architecture.md#filtered-globals calls the structural commitment: a shell reaches session
// switching, output configuration and window enumeration through globals an ordinary application must
// never be handed, and the tier a global sits in is the whole of what decides that. This file is that
// document's list transcribed, and it is deliberately the *only* copy — a second list in a second
// place is how a global ends up in one tier at bind and another in review.
//
// **Trust belongs to the listener rather than to the connection.** Connection identity comes from the
// socket a client arrived on (decision 23), so a `System` connection cannot arrive on the ordinary
// per-user socket and being the shell is something a process is *granted* at the socket it reaches
// rather than something it claims. `Server` records the level when it admits the connection, beside
// the session, for the same reason it records the session: a `wl_registry.bind` two calls deep cannot
// ask the socket anything.
//
// **A development run produces `Trust::System` and a real session does not yet.** `Server::BindSystem`
// binds `gyro-system-N` beside the ordinary socket where gyro bound that socket itself, and a client
// reaching it is granted this — which is decision 183 and is how a shell connects today. Which
// listener carries it for a *session*, who creates it and how a process is judged worthy of it are
// still open (Docs/Open.md, *The System tier needs its own listener*), and that half is an ABI between
// gyro and the session agent rather than anything this file can settle.

// What a connection is trusted with. Two values because the enum is the expensive thing to add later
// and its membership is not — Docs/Architecture.md#filtered-globals says exactly that, and a third
// rung would be a policy change rather than a structural one.
enum class Trust : std::uint8_t
{
	// An application. Every client on this machine today.
	User,

	// A shell, a screen recorder, a settings panel: something granted the run of the session it is in.
	System,
};

// Which clients a global is advertised to.
enum class GlobalTier : std::uint8_t
{
	// Every client. The vocabulary an application is written against.
	Shared,

	// Every client, and the objects it mints reach only the session the client belongs to.
	//
	// **Visible exactly where `Shared` is, and the distinction is still worth recording.** Session
	// scoping is a fact about what a global's objects *address* rather than about who may bind it: a
	// `wl_data_device_manager` is advertised to everyone and reaches one session's selection, and
	// hiding it would leave an application with no clipboard at all. Keeping the rung means this file
	// is Docs/Architecture.md's list rather than a lossy summary of it, and it is the rung that would
	// grow teeth first if a session's globals ever had to be minted per session rather than filtered.
	Session,

	// Only a connection that arrived on a `System` listener.
	System,
};

// Whether a client at this trust level is shown a global in this tier.
[[nodiscard]] constexpr bool Visible(GlobalTier tier, Trust trust) noexcept
{
	return tier != GlobalTier::System || trust == Trust::System;
}

// The tier an interface sits in, by its wire name.
//
// **By interface rather than by advertised global, because that is the shape of the policy.** The tier
// list is a list of protocols, and a second `wl_output` is not a second decision — so registering a
// tier per `wl_global*` would be fourteen call sites today that all say the same thing, and one of
// them getting it wrong would be invisible. The cost is that the table is matched on a string a header
// spells; `Tier.Test.cpp` is what holds the two together.
//
// **An interface with no row is `System`, which is a refusal rather than a default.** A global gyro
// advertises and this table does not name is a bug, and the two ways it can fail are not equal: shown
// to everyone, an unlisted System global is a shell's authority handed to an application, and it is
// silent. Hidden from everyone it is every client losing the protocol at once, on the first run, in
// front of whoever added it. What *catches* it is `Tier.Test.cpp` walking the interfaces gyro
// advertises before any of this ships — so there is no log line here, because the state it would
// report is one a passing test says cannot exist.
[[nodiscard]] GlobalTier TierOf(std::string_view interface) noexcept;

// Whether the table names this interface at all.
//
// **A separate question from `TierOf`, and the first System-tier global is what made it one.** While
// every global gyro served was an application's, *is it visible to a `User`* was a complete test for a
// forgotten row — an omission and a refusal are the same answer, so failing to be visible caught both.
// A global that is deliberately `System` gives that answer legitimately, and a walk asserting
// visibility would have to skip it, which is a test that stops checking the thing it is for.
//
// So the omission is asked about directly. Nothing in the dispatch path calls this and nothing should:
// at runtime the two cases are identical on purpose, and it is `Tier.Test.cpp` that has to tell them
// apart — before a global ships hidden from the clients it was written for.
[[nodiscard]] bool Listed(std::string_view interface) noexcept;
