#include "Protocol/Presentation.h"

#include <wayland-server-core.h>

#include <ctime>

ClientPresentationFeedback::ClientPresentationFeedback(HostContext& context, ClientSurface* surface) noexcept
	: m_Context{ &context }, m_Surface{ surface }
{}

ClientPresentationFeedback::~ClientPresentationFeedback()
{
	if (m_Surface != nullptr)
	{
		m_Surface->ForgetFeedback(*this);
		m_Surface = nullptr;
	}
}

void ClientPresentation::OnBound()
{
	// **`CLOCK_MONOTONIC` as the kernel's own number**, because what a client does with this is pass it
	// to `clock_gettime`. Core/Time.h's timebase is that clock, decision 57 makes the conversion happen
	// at ingest, and this is the one place gyro has to *name* the domain to somebody outside the
	// process rather than convert into or out of it.
	Object().ClockId(static_cast<std::uint32_t>(CLOCK_MONOTONIC));
}

Wayland::Server::WpPresentationFeedbackHandler* ClientPresentation::OnFeedback(Wayland::Server::WlSurface surface)
{
	ClientSurface* const onto = ClientSurface::Of(surface);

	// An inert object rather than a null, for `Protocol/Viewporter.cpp`'s reason: the client is holding
	// an id and libwayland needs something behind it. `wp_presentation` declares two errors and neither
	// is *that was not a surface*, so the client is ended through the object it named the argument on —
	// and the feedback answers `discarded` at the first opportunity rather than staying silent, since
	// silence on this protocol is a client that never draws again.
	if (onto == nullptr)
	{
		Object().PostNoMemory();

		return new ClientPresentationFeedback{ *m_Context, nullptr };
	}

	auto* const feedback = new ClientPresentationFeedback{ *m_Context, onto };

	// **Staged rather than live**, which is the whole of what this protocol asks of a compositor: the
	// feedback belongs to the *next* content update, so it waits on the surface's pending list exactly
	// as a frame callback does and becomes due at the commit that carries the pixels it is about.
	onto->AdoptFeedback(*feedback);

	return feedback;
}

Wayland::Server::WpPresentationHandler* PresentationGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientPresentation{ *m_Context };
}
