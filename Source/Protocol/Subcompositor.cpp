#include "Protocol/Subcompositor.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include "Scene/Commit.h"
#include "Scene/Store.h"
#include "World/Content.h"

namespace
{
// Whether `surface` is `parent` or anything under it, which is the loop the protocol's `bad_parent`
// error exists to refuse. Walked from the parent upward rather than from the surface downward,
// because a surface's children are two runs and its parent is one pointer — and the chain is at most
// as deep as the client's own nesting.
[[nodiscard]] bool Encloses(const ClientSurface& surface, const ClientSurface& parent) noexcept
{
	for (const ClientSurface* at = &parent; at != nullptr; at = at->ParentSurface())
	{
		if (at == &surface)
		{
			return true;
		}
	}

	return false;
}
} // namespace

ClientSubsurface::ClientSubsurface(HostContext& context, ClientSurface* surface, ClientSurface* parent) noexcept
	: m_Context{ &context }, m_Surface{ surface }, m_Parent{ parent }
{}

ClientSubsurface::~ClientSubsurface()
{
	Unmap();

	if (m_Parent != nullptr)
	{
		m_Parent->RemoveChild(*this);
		m_Parent = nullptr;
	}

	if (m_Surface != nullptr)
	{
		m_Surface->ForgetRole(*this);
		m_Surface = nullptr;
	}
}

bool ClientSubsurface::IsSynchronized() const noexcept
{
	// **The parent's mode wins, which is what makes the mode compose down a tree.** A toolkit that put
	// a video in a desynchronized subsurface and then dragged the whole window would otherwise have the
	// video arrive a frame off the frame around it, on exactly the interaction decision 51 says users
	// judge most harshly.
	return m_Synchronized || (m_Parent != nullptr && m_Parent->IsSynchronized());
}

void ClientSubsurface::OnSetDesync()
{
	m_Synchronized = false;

	// **Whatever was cached applies now**, which is the protocol's own rule: a client that stops
	// batching is one that wants what it has already stated to be on screen, and holding it until the
	// parent's next commit would leave a video player that switched modes showing a stale frame for as
	// long as its window sat still.
	if (!IsSynchronized() && m_Surface != nullptr)
	{
		m_Surface->ApplyCached();

		Sync();
	}
}

ClientSubsurface* ClientSubsurface::SiblingFor(Wayland::Server::WlSurface reference) const noexcept
{
	const ClientSurface* const named = ClientSurface::Of(reference);

	if (named == nullptr || m_Parent == nullptr)
	{
		return nullptr;
	}

	const ClientSurface::SurfaceStack& stack = m_Parent->Pending();

	for (const std::vector<ClientSubsurface*>* const run : { &stack.Below, &stack.Above })
	{
		for (ClientSubsurface* const sibling : *run)
		{
			if (sibling->Content() == named)
			{
				return sibling;
			}
		}
	}

	return nullptr;
}

void ClientSubsurface::Restage(ClientSubsurface* reference, bool above)
{
	if (m_Parent == nullptr)
	{
		return;
	}

	ClientSurface::SurfaceStack& stack = m_Parent->Pending();

	// Out of wherever it is first, so that a subsurface moving from one run to the other cannot end up
	// in both — and so that `place_above` naming the sibling this one is already above is a no-op
	// rather than a duplicate entry drawing the window twice.
	const auto matches = [this](const ClientSubsurface* held) noexcept { return held == this; };

	std::erase_if(stack.Below, matches);
	std::erase_if(stack.Above, matches);

	// **The parent surface itself as the reference**, which is the request the whole container shape
	// exists for: above it is the front of the upper run, below it is the back of the lower one, and
	// the two runs meet exactly where the parent's own pixels are.
	if (reference == nullptr)
	{
		if (above)
		{
			stack.Above.insert(stack.Above.begin(), this);
		}
		else
		{
			stack.Below.push_back(this);
		}

		return;
	}

	for (std::vector<ClientSubsurface*>* const run : { &stack.Below, &stack.Above })
	{
		const auto at = std::find(run->begin(), run->end(), reference);

		if (at != run->end())
		{
			run->insert(above ? std::next(at) : at, this);

			return;
		}
	}

	// The reference was a sibling a moment ago and is not in either run now, which is only reachable
	// if it has just been taken out from under this call. Topmost is where a subsurface starts.
	stack.Above.push_back(this);
}

void ClientSubsurface::OnPlaceAbove(Wayland::Server::WlSurface sibling)
{
	ClientSubsurface* const reference = SiblingFor(sibling);

	if (reference == nullptr && ClientSurface::Of(sibling) != m_Parent)
	{
		Object().PostError(
			Wayland::Server::WlSubsurfaceError::BadSurface,
			"wl_subsurface.place_above naming a surface that is neither a sibling nor the parent"
		);

		return;
	}

	Restage(reference, true);
}

void ClientSubsurface::OnPlaceBelow(Wayland::Server::WlSurface sibling)
{
	ClientSubsurface* const reference = SiblingFor(sibling);

	if (reference == nullptr && ClientSurface::Of(sibling) != m_Parent)
	{
		Object().PostError(
			Wayland::Server::WlSubsurfaceError::BadSurface,
			"wl_subsurface.place_below naming a surface that is neither a sibling nor the parent"
		);

		return;
	}

	Restage(reference, false);
}

void ClientSubsurface::OnSurfaceCommitted(ClientSurface& surface)
{
	(void)surface;

	// Reached only for a desynchronized subsurface — a synchronized one caches and never gets here —
	// so this is a client that has asked for its own cadence and gets it, subject to its parent being
	// on screen at all.
	Sync();
}

void ClientSubsurface::OnSurfaceGone()
{
	Unmap();

	m_Surface = nullptr;

	// The role object outlives the surface, which the protocol calls a client error and still requires
	// the compositor to survive. What it must not do is stay in the parent's stacking runs, where the
	// next commit would ask it for a node it can no longer have.
	if (m_Parent != nullptr)
	{
		m_Parent->RemoveChild(*this);
		m_Parent = nullptr;
	}
}

void ClientSubsurface::ForgetParent() noexcept
{
	Unmap();

	m_Parent = nullptr;
}

void ClientSubsurface::ParentCommitted()
{
	m_Position = m_PendingPosition;

	if (m_Surface != nullptr)
	{
		// The cache becomes current and this surface's own children commit behind it, which is the
		// recursion: a synchronized grandchild's state lands when its own parent's does, and its own
		// parent's lands here.
		m_Surface->ApplyCached();
	}

	Sync();
}

Vector3<double> ClientSubsurface::Origin(const SceneStore& scene) const noexcept
{
	Vector3<double> origin{ static_cast<double>(m_Position.X), static_cast<double>(m_Position.Y), 0.0 };

	if (m_Parent == nullptr)
	{
		return origin;
	}

	if (const Entity* const pixels = scene.Find(m_Parent->ContentNode()); pixels != nullptr)
	{
		const Vector3<double> parent = pixels->Translation.Model();

		origin.X += parent.X;
		origin.Y += parent.Y;
	}

	return origin;
}

void ClientSubsurface::Sync()
{
	SceneStore* const scene = m_Context->Store();

	if (scene == nullptr || m_Surface == nullptr)
	{
		return;
	}

	const EntityId container = m_Parent != nullptr ? m_Parent->Container() : EntityId{};
	const SurfaceState& state = m_Surface->Current();

	// **A subsurface is on screen when it has pixels and its parent is on screen**, which is the
	// protocol's own definition of mapped and covers both ways it stops being: a client attaching a
	// null buffer to hide one part of its window, and a window closing with everything under it.
	if (container.IsNull() || state.Content.IsNull())
	{
		Unmap();

		return;
	}

	const std::int32_t scale = state.BufferScale > 0 ? state.BufferScale : 1;

	const Size<SurfaceSpace, float> extent{ static_cast<float>(state.ContentSize.Width / scale),
		                                    static_cast<float>(state.ContentSize.Height / scale) };

	// The texels behind the quad, stated for `Protocol/Shell.cpp`'s reason: a node that says nothing
	// here can never be told apart from one being stretched, and a video in a subsurface at its own
	// texel size is the item most worth putting on a plane.
	const Rect<BufferSpace> source{
		{}, { static_cast<float>(state.ContentSize.Width), static_cast<float>(state.ContentSize.Height) }
	};

	const bool mapping = m_Node.IsNull();

	if (mapping)
	{
		const std::optional<EntityId> node = scene->CreateContainer(container, { .Extent = extent });

		if (!node)
		{
			m_Surface->Object().PostNoMemory();

			return;
		}

		const std::optional<EntityId> content = scene->CreateImage(
			*node,
			{ .Extent = extent },
			ImageContent{ .Texture = state.Content, .Source = source, .Frame = {}, .Color = ColorState::Srgb() }
		);

		if (!content)
		{
			SceneCommit undo{ *scene, CommitAuthor::Client };

			static_cast<void>(undo.Retire(*node));

			m_Surface->Object().PostNoMemory();

			return;
		}

		m_Node = *node;
		m_Content = *content;

		// **Only the image, unlike a window.** `Protocol/Shell.cpp` binds both of its nodes because focus
		// rests on the container; focus never rests on a subsurface — decision 162's walk goes up to the
		// nearest entity the focus stack already holds, which is the window this is part of — so what has
		// to resolve back to a surface here is the pixels, for the frame callback and for the pointer.
		m_Context->Bind(*content, *m_Surface);
	}

	{
		SceneCommit commit{ *scene, CommitAuthor::Client };

		static_cast<void>(commit.Attach(m_Content, state.Content, source));
		static_cast<void>(commit.Resize(m_Content, extent));
		static_cast<void>(commit.Resize(m_Node, extent));
		static_cast<void>(commit.AcceptInput(m_Content, state.Input));

		// Written only where it moved, per `Protocol/Shell.cpp`: a subsurface repainting itself sixty
		// times a second has not moved, and a retarget per commit on a channel whose target never changed
		// is the cost *doing nothing must cost nothing* exists to keep out.
		const Vector3<double> origin = Origin(*scene);
		const Entity* const node = scene->Find(m_Node);

		if (node != nullptr && node->Translation.Model() != origin)
		{
			// Immediate, per decision 68's neighbouring argument: a spring between a video player's
			// controls and the video underneath would make them lag it.
			static_cast<void>(commit.Move(m_Node, origin, Immediate()));
		}
	}
}

void ClientSubsurface::Unmap() noexcept
{
	if (m_Node.IsNull())
	{
		return;
	}

	// The children first and out of the surface, because each of them has its own binding to take out
	// and its own children below that — the retire below takes the whole subtree, and a child that
	// found its id already freed could not tell that from never having had one.
	if (m_Surface != nullptr)
	{
		m_Surface->UnmapChildren();
	}

	m_Context->Unbind(m_Content);

	if (SceneStore* const scene = m_Context->Store(); scene != nullptr)
	{
		SceneCommit commit{ *scene, CommitAuthor::Client };

		// Retired rather than removed, per decision 114 and for the same reason a window is: the subtree
		// keeps its place while whatever is still moving on it finishes.
		static_cast<void>(commit.Retire(m_Node));
	}

	m_Node = {};
	m_Content = {};
}

Wayland::Server::WlSubsurfaceHandler*
ClientSubcompositor::OnGetSubsurface(Wayland::Server::WlSurface surface, Wayland::Server::WlSurface parent)
{
	ClientSurface* const child = ClientSurface::Of(surface);
	ClientSurface* const onto = ClientSurface::Of(parent);

	// An inert object rather than a null, for `Protocol/Shell.cpp`'s reason: the client is holding an
	// id and libwayland needs something behind it, and the connection is already on its way out.
	if (child == nullptr || onto == nullptr)
	{
		Object().PostError(
			Wayland::Server::WlSubcompositorError::BadSurface,
			"wl_subcompositor.get_subsurface naming something that is not a wl_surface"
		);

		return new ClientSubsurface{ *m_Context, nullptr, nullptr };
	}

	// **The parent may not be the surface or anything under it**, which is the protocol's `bad_parent`
	// and is a cycle in the scene tree if it is let through — a preorder walk that never terminates, on
	// the frame thread, inside the section decision 36 makes an abort.
	if (Encloses(*child, *onto))
	{
		Object().PostError(
			Wayland::Server::WlSubcompositorError::BadParent,
			"wl_subcompositor.get_subsurface with a parent that is the surface or one of its descendants"
		);

		return new ClientSubsurface{ *m_Context, nullptr, nullptr };
	}

	auto* const subsurface = new ClientSubsurface{ *m_Context, child, onto };

	// **The role is claimed last**, because `AdoptRole` is the one place *one role at a time* lives and
	// a second claim has to fail the same way wherever it came from. A surface that already has one
	// keeps it, and this object stays inert rather than being joined to a tree it does not belong in.
	if (!child->AdoptRole(*subsurface))
	{
		Object().PostError(
			Wayland::Server::WlSubcompositorError::BadSurface,
			"wl_subcompositor.get_subsurface on a surface that already has a role"
		);

		return subsurface;
	}

	onto->AddChild(*subsurface);

	return subsurface;
}

Wayland::Server::WlSubcompositorHandler* SubcompositorGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientSubcompositor{ *m_Context };
}
