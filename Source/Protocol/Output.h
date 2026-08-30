#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Scene/Output.h"
#include "Wayland/Server/Wayland.h"

class HostContext;
class SceneStore;

// `wl_output`: the displays, told to the clients on them.
//
// **What a client needs from this is the scale, and everything else here is the frame around it.**
// Decision 164 derives an output's scale from one angular preference and the panel's own geometry, so
// gyro now runs panels at scales other than one — and a client that is never told lays out at 1x. On
// a laptop panel at 2x that is a window drawn a quarter of the area it should be and then magnified
// onto the glass: every window on the machine soft, which is worse than the tiny-but-sharp windows
// that came before this global existed. So the derivation and the global belong in the same session's
// work, and `wl_surface.enter` is the half that actually delivers it — a client picks its buffer
// scale from the outputs it has entered, and one that has entered none stays at 1 however many
// outputs it has bound.
//
// **Advertised at version 3, and the number is a promise about the events gyro sends** — the rule
// `Protocol/Compositor.h` sets. 3 is `geometry`, `mode`, `done`, `scale` and `release`, and 4 would
// oblige a `name` and a `description` per output. Those are supposed to be the connector's — `eDP-1`,
// and something a person would recognise — and the connector's name stops at `Drm/Device.h` and never
// crosses into the world. An invented one would be worse than none: `name` is what a client keys its
// per-monitor state on, so a compositor that made one up would have applications remembering the
// wrong window positions rather than none.
//
// **Two things this sends that are not the truth yet, and both are the same absence.** The mode's
// refresh rate goes out as zero and the physical size as zero by zero, because neither is in the
// record `Scene/Output.h` holds: decision 97 gives the panel's timing to the frame side, and decision
// 164's whole argument is that millimetres are not a perceptual quantity until a distance is applied,
// which has already happened by the time a scale reaches here. Zero is what the protocol says to send
// for a physical size that does not make sense, and a client must not schedule against a refresh
// rate in any case — the frame callback is the contract and it is answered from what actually reached
// the glass. What it costs is a media player that reads the mode to guess a display cadence, which is
// worth saying out loud rather than leaving as a zero somebody finds later.
inline constexpr std::uint32_t OutputVersion = 3;

class HostOutput;

// One output as one client sees it: a resource, and the burst of events the protocol says a bind
// begins with.
class ClientOutput final : public Wayland::Server::WlOutputIgnoring
{
public:
	explicit ClientOutput(HostOutput& output) noexcept : m_Output{ &output } {}

	~ClientOutput() override;

	void OnGone() override { delete this; }

	// **The contract begins with events rather than with a request**, which is what `OnBound` exists
	// for: a client binds `wl_output` and waits for the geometry, the mode, the scale and the `done`
	// that closes them, and nothing it sends prompts them.
	void OnBound() override;

	// The same burst again, because something about the output changed. `done` is what makes the group
	// atomic, and a client applies nothing until it arrives.
	void Send() const;

	// The output going away under a client that is still holding a resource for it. The global is
	// already gone by then, so what is left is to tell this object it has nothing behind it — a
	// resource whose display was unplugged answers `release` and nothing else.
	void Forget() noexcept { m_Output = nullptr; }

private:
	HostOutput* m_Output = nullptr;
};

// One output's global, and every resource bound off it.
//
// **The bindings are held because an output is not immutable.** A person drags a monitor in a
// settings panel and its position changes; a preference changes and every scale on the machine moves
// at once. Both are a re-send to whoever is bound rather than a new global, and a global with no way
// back to its resources could only answer the first of them.
class HostOutput final : public Wayland::Server::WlOutputBinding
{
public:
	HostOutput() = default;

	~HostOutput() override;

	// Advertise this output. False where the display refused the global, which is an allocation
	// failing at startup — the caller logs it and carries on with one fewer output advertised, because
	// a compositor that will not come up because a second monitor could not be announced is worse than
	// one where that monitor is invisible to clients.
	[[nodiscard]] bool Open(wl_display& display, const SceneOutput& output);

	// Withdraw the global and cut every resource loose from it. A client is told the global is gone and
	// keeps whatever it bound until it releases it, which is the protocol's own shape for a monitor
	// being unplugged.
	void Close() noexcept;

	// What the world now says about this output. Re-sends to every binding where anything a client can
	// see has moved, and does nothing where the change was to something it cannot.
	void Update(const SceneOutput& output);

	[[nodiscard]] const SceneOutput& Facts() const noexcept { return m_Facts; }

	[[nodiscard]] OutputId Id() const noexcept { return m_Facts.Id; }

	// This output's resource in one client's id space, or an invalid one where that client never bound
	// it. `wl_surface.enter` names an object rather than an output, so the surface's own client is what
	// decides which resource the event carries.
	//
	// **The first binding rather than all of them**, which is a simplification and is stated as one: a
	// client may bind the same global twice and the protocol has an `enter` per resource. Nothing does
	// — a toolkit binds each global once — and the cost of being wrong is a second copy of a window's
	// output list going unmentioned, in a client that has deliberately made two of everything.
	[[nodiscard]] Wayland::Server::WlOutput ResourceFor(const wl_client& client) const noexcept;

	void Attach(ClientOutput& binding);
	void Detach(ClientOutput& binding) noexcept;

	Wayland::Server::WlOutputHandler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	wl_global* m_Global = nullptr;
	SceneOutput m_Facts{};
	std::vector<ClientOutput*> m_Bindings;
};

// Every output gyro advertises, kept in the order the world holds them — which is the order
// `Scene/Reach.h`'s mask is indexed by, so a reach bit and a global are the same subscript.
class HostOutputs
{
public:
	// Bring the advertised set in line with the world's. Keyed on identity rather than on position: a
	// monitor unplugged from between two others is a global withdrawn, and the ones that stayed keep
	// the resources their clients are holding.
	void Sync(wl_display& display, std::span<const SceneOutput> outputs);

	[[nodiscard]] std::span<const std::unique_ptr<HostOutput>> All() const noexcept { return m_Outputs; }

private:
	std::vector<std::unique_ptr<HostOutput>> m_Outputs;
};

// Tell each window which outputs it is on.
//
// **This is what makes the scale reach a client at all.** A toolkit reads `wl_surface.enter`, takes
// the largest scale among the outputs a window is on, and redraws its buffer at that — so a window
// that is never entered draws at 1x forever whatever the output said when it was bound.
//
// It is a comparison per `Advance` against `Scene/Reach.h` rather than a signal out of the scene, for
// `Protocol/Seat.h`'s reason on focus: the commit that moved the window and the step that notices are
// the same wakeup of the same thread, and a client that has moved between two outputs inside one
// iteration should hear about the destination rather than about both.
void SyncOutputEntry(HostContext& context, const HostOutputs& outputs, const SceneStore& scene);
