#include "Protocol/Scene.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "Core/Clock.h"
#include "Protocol/Context.h"
#include "Protocol/Floor.h"
#include "Protocol/Foreign.h"
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

	m_Placements.clear();
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

	return new SceneManager{ *m_Context };
}
