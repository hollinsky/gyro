#include "Protocol/Scene.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "Core/Clock.h"
#include "Protocol/Context.h"
#include "Protocol/Floor.h"
#include "Protocol/Foreign.h"
#include "Protocol/Output.h"
#include "Protocol/Shell.h"
#include "Scene/Commit.h"
#include "Scene/Store.h"

namespace
{
// The wire's three fields as one instant. `wp_presentation`'s encoding read the other way round from
// `gyro_binding_v1.pressed`, which is where a shell got the number it echoes back here.
//
// **Saturated rather than wrapped**, because what arrives is a client's arithmetic on a value gyro
// gave it and a shell that added instead of subtracting must not land in the past. The clamp in
// `SceneCommit` handles the forward direction; this one only has to make sure the sum is a number.
[[nodiscard]] Instant InstantOf(std::uint32_t high, std::uint32_t low, std::uint32_t nanoseconds) noexcept
{
	const auto seconds = (static_cast<std::uint64_t>(high) << 32U) | static_cast<std::uint64_t>(low);
	constexpr auto ceiling = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

	if (seconds > (ceiling - nanoseconds) / 1'000'000'000U)
	{
		return Instant{ Duration::max() };
	}

	return Monotonic::FromNanoseconds(static_cast<std::int64_t>(seconds * 1'000'000'000U + nanoseconds));
}

[[nodiscard]] Vector3<double> PositionOf(wl_fixed_t x, wl_fixed_t y, wl_fixed_t z) noexcept
{
	return { wl_fixed_to_double(x), wl_fixed_to_double(y), wl_fixed_to_double(z) };
}

[[nodiscard]] wl_fixed_t FixedOf(double value) noexcept
{
	return wl_fixed_from_double(value);
}

// The mapped window an entity names, or null.
//
// **A scan over the window registry rather than a table**, which is
// [Foreign.h](Foreign.h)'s own answer to the same question one file over: the length is the windows
// on a desk, and this runs once per placement a shell states rather than per frame.
[[nodiscard]] ClientXdgSurface* WindowFor(const HostContext& context, EntityId window) noexcept
{
	if (window.IsNull())
	{
		return nullptr;
	}

	const std::span<ClientXdgSurface* const> windows = context.Windows();

	const auto found = std::find_if(windows.begin(), windows.end(), [window](const ClientXdgSurface* const held) {
		return held->Window() == window;
	});

	return found == windows.end() ? nullptr : *found;
}
} // namespace

void ClientContainer::OnGone()
{
	if (m_Manager != nullptr)
	{
		m_Manager->Remove(*this);
	}

	delete this;
}

void ClientContainer::OnBound()
{
	Object().State(FixedOf(m_State.X), FixedOf(m_State.Y), FixedOf(m_State.Z), m_Shown ? 1U : 0U);
}

void ClientContainer::OnRemove()
{
	// **Inert here and removed at commit**, which is the split the double buffering forces and which is
	// also the right one: a shell that moves the windows out and then takes the workspace away has
	// stated one arrangement, and applying the removal now would land the windows on the floor a frame
	// before the workspace went. Requests after this one do nothing, which is what the protocol says
	// this object becomes.
	m_Removing = !m_Container.IsNull();

	m_Position.reset();
	m_Visible.reset();
}

void ClientContainer::OnSetPosition(wl_fixed_t x, wl_fixed_t y, wl_fixed_t z)
{
	if (m_Container.IsNull() || m_Removing)
	{
		return;
	}

	m_Position = PositionOf(x, y, z);
}

void ClientContainer::OnSetVisible(std::uint32_t visible)
{
	if (m_Container.IsNull() || m_Removing)
	{
		return;
	}

	m_Visible = visible != 0;
}

void ClientContainer::Apply(SceneCommit& commit)
{
	if (m_Position)
	{
		static_cast<void>(commit.Move(m_Container, *m_Position));
	}

	if (m_Visible)
	{
		static_cast<void>(*m_Visible ? commit.Show(m_Container) : commit.Hide(m_Container));
	}

	m_Position.reset();
	m_Visible.reset();
}

void SceneManager::OnGone()
{
	if (m_Context != nullptr)
	{
		m_Context->Remove(*this);
	}

	// **The claim goes back and the containers stay**, which is the asymmetry decision 141 turns on: a
	// shell exiting must leave the session's arrangement exactly as it was and must leave gyro able to
	// keep serving it, so the Floorplanner comes back and not one window moves.
	if (m_Context != nullptr && m_Placing)
	{
		if (SessionFloors* const floors = m_Context->Floors(); floors != nullptr)
		{
			wl_client* const client = Object().IsValid() ? Object().WireClient() : nullptr;

			floors->ReleasePlacement(m_Context->Session(client), client);
		}
	}

	for (ClientContainer* const container : m_Containers)
	{
		container->Forget();
	}

	delete this;
}

void SceneManager::OnBound()
{
	if (m_Context == nullptr)
	{
		return;
	}

	if (const HostOutputs* const outputs = m_Context->Outputs(); outputs != nullptr)
	{
		SendWorkAreas(*outputs);
	}
}

void SceneManager::OnSetCapabilities(Wayland::Server::GyroSceneV1Capability capabilities)
{
	// **Applied immediately rather than at the next commit**, and the reason is what a commit is for: a
	// commit exists so that everything a person watches move starts together, and this moves nothing. It
	// is a standing statement about the shell — *these are the controls I answer for* — which reaches a
	// client on its next configure through `SyncWindows` rather than as a change to the world.
	m_Capabilities = CapabilitiesOf(capabilities);
}

void SceneManager::OnSetWindowSize(
	Wayland::Server::ExtForeignToplevelHandleV1 toplevel,
	std::int32_t width,
	std::int32_t height
)
{
	auto* const handle = static_cast<ForeignToplevelHandle*>(toplevel.Implementation());

	// Silently ignored for a window that has already gone, per `OnPlaceWindow`: the `closed` event and
	// this request cross on the wire, and ending the shell over an application quitting would make every
	// exit a chance to take the desktop down.
	if (handle == nullptr || handle->IsClosed())
	{
		return;
	}

	// **A negative size is refused where zero is not.** Zero is the protocol's own *pick your own size*
	// and is what hands a window back its natural size when it stops being maximised; a negative one is
	// a shell that has subtracted a panel's height from a screen it got wrong, and quietly clamping it
	// would leave a window at some arbitrary minimum with nothing naming the disagreement.
	if (width < 0 || height < 0)
	{
		Object().PostError(
			Wayland::Server::GyroSceneV1Error::BadSize, "gyro_scene_v1.set_window_size with a negative extent"
		);

		return;
	}

	Staged(handle->Window()).Size = PixelSize<SurfaceSpace>{ width, height };
}

void SceneManager::OnSetWindowStates(
	Wayland::Server::ExtForeignToplevelHandleV1 toplevel,
	Wayland::Server::GyroSceneV1State states
)
{
	auto* const handle = static_cast<ForeignToplevelHandle*>(toplevel.Implementation());

	if (handle == nullptr || handle->IsClosed())
	{
		return;
	}

	Staged(handle->Window()).States = StatesOf(states);
}

SceneManager::Arrangement& SceneManager::Staged(EntityId window)
{
	const auto found = std::find_if(m_Arrangements.begin(), m_Arrangements.end(), [window](const Arrangement& held) {
		return held.Window == window;
	});

	if (found != m_Arrangements.end())
	{
		return *found;
	}

	m_Arrangements.push_back(Arrangement{ .Window = window, .Size = std::nullopt, .States = std::nullopt });

	return m_Arrangements.back();
}

void SceneManager::SendWorkAreas(const HostOutputs& outputs)
{
	wl_client* const client = Object().IsValid() ? Object().WireClient() : nullptr;

	if (client == nullptr)
	{
		return;
	}

	// **Taken rather than read, and each match is consumed**, which is `HostOutputs::Sync`'s own shape
	// one file over and is there for the same reason: an `OutputId` is minted by whatever configured the
	// display, so two outputs a caller built without one compare equal — and a keyed lookup would then
	// answer with the first screen's rectangle for the second and re-send it every iteration for as long
	// as the machine ran. Consuming the entry makes the duplicate case pair up in order instead.
	std::vector<Reported> previous = std::move(m_WorkAreas);

	std::vector<Reported> reported;
	reported.reserve(outputs.All().size());

	for (const std::unique_ptr<HostOutput>& output : outputs.All())
	{
		const Rect<GlobalSpace> area = WorkArea(output->Facts());

		const auto before = std::find_if(previous.begin(), previous.end(), [&output](const Reported& held) {
			return held.Output == output->Id();
		});

		if (before != previous.end())
		{
			const bool unchanged = before->Area == area;

			previous.erase(before);

			if (unchanged)
			{
				reported.push_back(Reported{ .Output = output->Id(), .Area = area });

				continue;
			}
		}

		// **An output this client has not bound stays owed rather than being dropped**, which is
		// `SyncOutputEntry`'s rule for an `enter` with nothing to name and is right for the same reason:
		// the event carries a `wl_output` object, and a shell that walks the registry a moment later has
		// to hear about the screen it missed. Leaving the entry out of `reported` is what makes the next
		// walk try again.
		const Wayland::Server::WlOutput object = output->ResourceFor(*client);

		if (!object.IsValid())
		{
			continue;
		}

		// Truncated rather than rounded, for `ClientXdgSurface::RefreshRoom`'s reason: this is a ceiling
		// on how much room a window has, and rounding up recommends a window half a pixel wider than the
		// screen it is on.
		Object().WorkArea(
			object,
			static_cast<std::int32_t>(area.Origin.X),
			static_cast<std::int32_t>(area.Origin.Y),
			static_cast<std::int32_t>(area.Extent.Width),
			static_cast<std::int32_t>(area.Extent.Height)
		);

		reported.push_back(Reported{ .Output = output->Id(), .Area = area });
	}

	// Whatever is left in `previous` is a monitor that has gone, and it goes with it: the walk is over
	// the outputs that exist, so an entry nothing matched no longer has a screen to be about.
	m_WorkAreas = std::move(reported);
}

void SceneManager::OnClaimPlacement()
{
	wl_client* const client = Object().IsValid() ? Object().WireClient() : nullptr;
	SessionFloors* const floors = m_Context == nullptr ? nullptr : m_Context->Floors();

	if (floors == nullptr || client == nullptr)
	{
		return;
	}

	if (!floors->ClaimPlacement(m_Context->Session(client), client))
	{
		// **An error rather than a silent second placer**, and the contrast with a claimed chord is the
		// argument: several clients may hold one chord because a keystroke can be told to all of them,
		// and placement has exactly one answer. What a person would see if this were tolerated is a
		// window put in two places in one frame, or — worse and more likely — a shell that quietly never
		// places anything, which reads as a compositor that has stopped opening windows.
		Object().PostError(
			Wayland::Server::GyroSceneV1Error::PlacementTaken,
			"gyro_scene_v1.claim_placement while another client is already placing this session's windows"
		);

		return;
	}

	m_Placing = true;
}

Wayland::Server::GyroContainerV1Handler*
SceneManager::OnGetContainer(std::string_view name, wl_fixed_t x, wl_fixed_t y, wl_fixed_t z)
{
	// **Every refusal below still returns an object**, which is `xdg_surface.get_popup`'s rule in this
	// tree and `gyro_chrome_manager_v1.get_chrome`'s: the client has already spent an id and libwayland
	// needs something behind it. A container object with no container accepts every request and does
	// nothing with any of them.
	if (name.empty() || name.size() > MaximumContainerName)
	{
		Object().PostError(
			Wayland::Server::GyroSceneV1Error::BadName,
			"gyro_scene_v1.get_container with a name that is empty or longer than gyro will hold"
		);

		return new ClientContainer{ nullptr, EntityId{}, Vector3<double>{}, true };
	}

	wl_client* const client = Object().IsValid() ? Object().WireClient() : nullptr;
	SceneStore* const scene = m_Context == nullptr ? nullptr : m_Context->Store();
	SessionFloors* const floors = m_Context == nullptr ? nullptr : m_Context->Floors();

	// Not a client error: a session that has ended between this request being sent and being dispatched
	// has no floor to hang a container under, and neither does a run with no store around it. Ending the
	// connection over gyro's own teardown would turn a logout into a crash report.
	if (scene == nullptr || floors == nullptr || client == nullptr)
	{
		return new ClientContainer{ nullptr, EntityId{}, Vector3<double>{}, true };
	}

	const EntityId container = floors->Declare(*scene, m_Context->Session(client), name, PositionOf(x, y, z));

	// **What the container actually is, which is what tells a restarted shell that its workspaces
	// survived.** For one this call created it repeats what was asked for; for one that was already
	// there it is what came through, which is the answer a shell that has just come back needs and
	// cannot work out for itself. It goes out from `OnBound`, since the resource does not exist yet.
	const Entity* const node = scene->Find(container);

	auto* const held = new ClientContainer{ this,
		                                    container,
		                                    node == nullptr ? Vector3<double>{} : node->Translation.Model(),
		                                    node == nullptr || (node->Flags & Node::Hidden) == 0 };

	m_Containers.push_back(held);

	return held;
}

void SceneManager::OnPlaceWindow(
	Wayland::Server::ExtForeignToplevelHandleV1 toplevel,
	Wayland::Server::GyroContainerV1 container,
	wl_fixed_t x,
	wl_fixed_t y,
	wl_fixed_t z
)
{
	auto* const handle = static_cast<ForeignToplevelHandle*>(toplevel.Implementation());

	// **A window that has already gone is silently ignored rather than being an error.** An application
	// quitting while a shell is halfway through a batch is ordinary, and no amount of care on the
	// shell's part can avoid the race — the `closed` event and this request cross on the wire. Ending
	// the shell over it would make every application's exit a chance to take the desktop down.
	if (handle == nullptr || handle->IsClosed())
	{
		return;
	}

	EntityId parent{};

	if (container.IsValid())
	{
		auto* const held = static_cast<ClientContainer*>(container.Implementation());

		// A removed container is inert, so a placement into one falls back to the floor rather than
		// being dropped: the shell asked for the window to be somewhere, and nowhere is not an answer.
		if (held != nullptr)
		{
			parent = held->Container();
		}
	}

	m_Placements.push_back(Placement{ .Window = handle->Window(), .Container = parent, .At = PositionOf(x, y, z) });
}

void SceneManager::OnCommit(
	Wayland::Server::GyroSceneV1Transition transition,
	std::uint32_t seconds_hi,
	std::uint32_t seconds_lo,
	std::uint32_t nanoseconds
)
{
	const std::optional<Transition> named = TransitionOf(transition);

	if (!named)
	{
		// **Nothing staged is applied**, which is the conservative direction and the only coherent one:
		// the client is about to be gone, and half an arrangement is a worse thing to leave behind than
		// none.
		Object().PostError(
			Wayland::Server::GyroSceneV1Error::BadTransition,
			"gyro_scene_v1.commit with a transition gyro has no springs for"
		);

		return;
	}

	wl_client* const client = Object().IsValid() ? Object().WireClient() : nullptr;
	SceneStore* const scene = m_Context == nullptr ? nullptr : m_Context->Store();
	SessionFloors* const floors = m_Context == nullptr ? nullptr : m_Context->Floors();

	if (scene == nullptr || floors == nullptr || client == nullptr)
	{
		m_Placements.clear();
		m_Arrangements.clear();

		return;
	}

	const SessionId session = m_Context->Session(client);
	const EntityId floor = floors->Container(session);
	const Instant origin = InstantOf(seconds_hi, seconds_lo, nanoseconds);

	// **A window is an entrance or a move, and which it is has to be decided before either runs.**
	// Placing marks the window placed, so a pass that asked the question again would find every
	// entrance answering as a move and slide the window a second time under the shell's transition.
	//
	// A vector for the windows one commit places, which is one on the ordinary case and a workspace's
	// worth on a shell rebuilding its arrangement.
	std::vector<const Placement*> entrances;

	// One transaction for the moves, so that a workspace sliding out and a window moving inside it share
	// an origin whatever order the shell stated them in, and no frame is drawn with half of it applied.
	{
		SceneCommit commit{ *scene, CommitAuthor::Shell, origin, *named };

		for (const Placement& placement : m_Placements)
		{
			const ClientXdgSurface* const window = WindowFor(*m_Context, placement.Window);

			if (window == nullptr)
			{
				continue;
			}

			if (!window->IsPlaced())
			{
				entrances.push_back(&placement);

				continue;
			}

			const EntityId parent = placement.Container.IsNull() ? floor : placement.Container;

			// Reparent and move together, because a position is stated relative to a parent: applying
			// one without the other would put the window at the coordinates it had on the workspace it
			// just left.
			static_cast<void>(commit.Reparent(placement.Window, parent));
			static_cast<void>(commit.Move(placement.Window, placement.At));
		}

		for (ClientContainer* const held : m_Containers)
		{
			if (held->IsRemoving())
			{
				floors->Undeclare(commit, *scene, session, held->Container());
				held->Removed();

				continue;
			}

			held->Apply(commit);
		}
	}

	// **And the entrances, outside the shell's transition**, per `PlaceWindow`: a window placed for the
	// first time has nowhere to have travelled from, so its position lands and gyro plays the opening.
	// What the shell contributes is the timestamp, which is what makes the entrance start when the
	// person pressed the key rather than when the shell got round to answering.
	for (const Placement* const placement : entrances)
	{
		ClientXdgSurface* const window = WindowFor(*m_Context, placement->Window);

		if (window == nullptr)
		{
			continue;
		}

		const EntityId parent = placement->Container.IsNull() ? floor : placement->Container;

		PlaceWindow(*scene, placement->Window, parent, placement->At, origin);

		window->MarkPlaced();
	}

	// **The sizes and the states, and neither of them is inside the transaction above.** What a shell
	// says here is a request to the *application* rather than a write to the world: the size goes out in
	// a configure and the window's extent changes when the client's next buffer arrives, which is the
	// round trip decision 166 says a resize already is and does not remove. So there is nothing here for
	// a commit scope to hold and nothing for the transition to govern — where the window is animates,
	// how big it is arrives, and a maximise states both in one batch so the travel starts at once.
	//
	// **The configure itself is owed rather than sent**, which is `SyncWindows`' comparison doing the
	// work: this writes what the shell said, and the walk at the end of the iteration turns whatever
	// disagrees with what was last sent into one configure per window. A shell that moves forty windows
	// onto a workspace therefore costs each of them one configure rather than one per request it sent.
	for (const Arrangement& arrangement : m_Arrangements)
	{
		ClientXdgSurface* const window = WindowFor(*m_Context, arrangement.Window);
		ClientXdgToplevel* const toplevel = window == nullptr ? nullptr : window->Toplevel();

		if (toplevel == nullptr)
		{
			continue;
		}

		if (arrangement.Size.has_value())
		{
			toplevel->SetWanted(*arrangement.Size);
		}

		if (arrangement.States.has_value())
		{
			toplevel->Declare(*arrangement.States);
		}
	}

	m_Placements.clear();
	m_Arrangements.clear();
}

void SceneManager::Remove(ClientContainer& container) noexcept
{
	const auto found = std::find(m_Containers.begin(), m_Containers.end(), &container);

	if (found != m_Containers.end())
	{
		m_Containers.erase(found);
	}
}

Wayland::Server::GyroSceneV1Handler* SceneGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	auto* const manager = new SceneManager{ *m_Context };

	// Registered before `OnBound` runs, because the walk that answers *who places this session's
	// windows* is over this list and a shell claiming placement inside its own bind would otherwise be
	// invisible to it for one iteration.
	m_Context->Add(*manager);

	return manager;
}

WindowStates StatesOf(Wayland::Server::GyroSceneV1State states) noexcept
{
	using State = Wayland::Server::GyroSceneV1State;

	return WindowStates{ .Maximized = Any(states & State::Maximized),
		                 .Fullscreen = Any(states & State::Fullscreen),
		                 .TiledLeft = Any(states & State::TiledLeft),
		                 .TiledRight = Any(states & State::TiledRight),
		                 .TiledTop = Any(states & State::TiledTop),
		                 .TiledBottom = Any(states & State::TiledBottom) };
}

WindowCapabilities CapabilitiesOf(Wayland::Server::GyroSceneV1Capability capabilities) noexcept
{
	using Capability = Wayland::Server::GyroSceneV1Capability;

	return WindowCapabilities{ .Maximize = Any(capabilities & Capability::Maximize),
		                       .Minimize = Any(capabilities & Capability::Minimize),
		                       .Fullscreen = Any(capabilities & Capability::Fullscreen) };
}

Rect<GlobalSpace> WorkArea(const SceneOutput& output) noexcept
{
	// The whole screen, because nothing can reserve any of it yet. See the header: this is the seam the
	// keepout lands on rather than an arithmetic that has been left out, and having the three callers
	// ask one question is what stops them drifting apart before there is anything to subtract.
	return output.Bounds;
}

SceneManager* PlacerFor(const HostContext& context, SessionId session) noexcept
{
	for (SceneManager* const manager : context.Scenes())
	{
		if (!manager->IsPlacing() || !manager->Object().IsValid())
		{
			continue;
		}

		if (context.Session(manager->Object().WireClient()) == session)
		{
			return manager;
		}
	}

	return nullptr;
}

WindowCapabilities CapabilitiesFor(const HostContext& context, const ClientXdgSurface& window)
{
	wl_client* const client = window.Object().IsValid() ? window.Object().WireClient() : nullptr;

	if (client == nullptr)
	{
		return {};
	}

	const SceneManager* const placer = PlacerFor(context, context.Session(client));

	return placer == nullptr ? WindowCapabilities{} : placer->Capabilities();
}

bool ForwardWindowRequest(
	HostContext& context,
	const ClientXdgSurface& window,
	Wayland::Server::GyroSceneV1Action action,
	Wayland::Server::WlOutput preferred
)
{
	const SceneStore* const scene = context.Store();
	ForeignToplevelGlobal* const foreign = context.Foreign();
	wl_client* const client = window.Object().IsValid() ? window.Object().WireClient() : nullptr;

	// **An unmapped window is refused here rather than being forwarded and dropped.** A client is
	// entitled to ask before its first buffer — a browser started fullscreen does — and there is no
	// entity for the shell to be told about yet, so the answer is the configure the caller sends: the
	// state it asked for is absent, and it asks again once it is on screen.
	if (scene == nullptr || foreign == nullptr || client == nullptr || !window.IsMapped())
	{
		return false;
	}

	SceneManager* const placer = PlacerFor(context, context.Session(client));

	if (placer == nullptr || !placer->Object().IsValid())
	{
		return false;
	}

	wl_client* const shell = placer->Object().WireClient();

	if (shell == nullptr)
	{
		return false;
	}

	const Wayland::Server::ExtForeignToplevelHandleV1 handle = foreign->HandleFor(*shell, window.Window());

	if (!handle.IsValid())
	{
		return false;
	}

	// **The output preference crosses id spaces and has to be translated.** The application named a
	// `wl_output` of its own, and the shell holds a different resource for the same display; sending the
	// application's id to the shell would name whatever object happens to sit at that number in the
	// shell's space, which is a type confusion rather than a wrong monitor. An output the shell has not
	// bound comes back invalid, which is the null the protocol already allows for a client with no
	// preference — the shell then chooses the screen, which was its job anyway.
	Wayland::Server::WlOutput named;

	if (preferred.IsValid())
	{
		if (auto* const bound = static_cast<ClientOutput*>(preferred.Implementation()); bound != nullptr)
		{
			if (const HostOutput* const host = bound->Host(); host != nullptr)
			{
				named = host->ResourceFor(*shell);
			}
		}
	}

	// **The arrival of the request rather than the instant a person clicked**, which no application
	// tells anybody: this is the earliest moment anything in this process knows about, and a shell that
	// echoes it back on the commit answering starts the window moving from here instead of from
	// whenever the answer was worked out. One dispatch iteration of lag rather than two.
	const auto nanoseconds = static_cast<std::uint64_t>(Monotonic::ToNanoseconds(scene->Now()));
	const std::uint64_t seconds = nanoseconds / 1'000'000'000U;

	placer->Object().WindowRequest(
		handle,
		action,
		named,
		static_cast<std::uint32_t>(seconds >> 32U),
		static_cast<std::uint32_t>(seconds & 0xffffffffU),
		static_cast<std::uint32_t>(nanoseconds % 1'000'000'000U)
	);

	return true;
}

void SyncWorkAreas(const HostContext& context, const HostOutputs& outputs)
{
	// Over a copy for `ForeignToplevelGlobal::Sync`'s reason: an event is a wire write, and a client
	// whose connection has already failed is torn down inside libwayland — which destroys its resources
	// and so edits this list underneath the walk.
	const std::vector<SceneManager*> scenes{ context.Scenes().begin(), context.Scenes().end() };

	for (SceneManager* const manager : scenes)
	{
		manager->SendWorkAreas(outputs);
	}
}
