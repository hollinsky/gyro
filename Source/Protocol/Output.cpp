#include "Protocol/Output.h"

#include <algorithm>

#include "Protocol/Context.h"
#include "Protocol/Shell.h"
#include "Protocol/Surface.h"
#include "Scene/Reach.h"
#include "Scene/Store.h"

namespace
{
// The mode's rate, in the thousandths of a hertz `wl_output.mode` is stated in.
//
// **The nominal period rather than the cadence gyro is achieving**, which is what the event means: it
// describes the mode, and a mode's rate does not move while frames do. Zero for an output with no
// period — a headless sweep, a file — where it is honestly unknown rather than a rounding of
// something.
//
// Rounded to nearest, because 16'666'666 ns is 60'000.002 mHz and truncating it reports a panel as
// 59.999 Hz to every client that prints one.
[[nodiscard]] std::int32_t Millihertz(Duration period)
{
	const std::int64_t nanoseconds = period.count();

	if (nanoseconds <= 0)
	{
		return 0;
	}

	constexpr std::int64_t Thousandths = 1'000'000'000'000;

	return static_cast<std::int32_t>((Thousandths + (nanoseconds / 2)) / nanoseconds);
}

// What a client is told about one output, as one group. Sent on bind and again whenever any of it
// moves, with `done` closing the group — a client applies nothing until that arrives, which is what
// keeps a scale and the position it belongs with from being read half apart.
void Describe(Wayland::Server::WlOutput object, const SceneOutput& output)
{
	object.Geometry(
		static_cast<std::int32_t>(output.Bounds.Origin.X),
		static_cast<std::int32_t>(output.Bounds.Origin.Y),
		0,
		0,
		Wayland::Server::WlOutputSubpixel::Unknown,
		"gyro",
		"gyro",
		static_cast<Wayland::Server::WlOutputTransform>(output.Orientation)
	);

	// The mode is the panel's own grid, in its own pixels, which is what the protocol asks for and is
	// the one number here that is not affected by the scale.
	object.Mode(
		Wayland::Server::WlOutputMode::Current | Wayland::Server::WlOutputMode::Preferred,
		output.Grid.Width,
		output.Grid.Height,
		Millihertz(output.Period)
	);

	// **Decision 56 in one line.** `wl_output.scale` is an integer and a derived scale routinely is
	// not, so a client that speaks only this interface is told the ceiling and draws at it, and gyro
	// downscales — minification degrades gracefully where magnification does not. The exact rational
	// reaches a client through `wp_fractional_scale_v1`, which is its own commit and is what stops a
	// window on a 1.7x panel from rendering 18% more pixels than it needs.
	object.Scale(output.Density.CeilToInteger());

	object.Done();
}

// Whether anything a client can see about an output has moved. Deliberately not `operator==` on the
// record: the session an output shows and its generation are gyro's own business, and re-sending a
// group because one of them changed would have every client redraw when a person switched users.
[[nodiscard]] bool Visible(const SceneOutput& before, const SceneOutput& after)
{
	return before.Bounds != after.Bounds || before.Density != after.Density || before.Grid != after.Grid ||
	       before.Period != after.Period || before.Orientation != after.Orientation;
}
} // namespace

ClientOutput::~ClientOutput()
{
	if (m_Output != nullptr)
	{
		m_Output->Detach(*this);
	}
}

void ClientOutput::OnBound()
{
	if (m_Output == nullptr)
	{
		return;
	}

	m_Output->Attach(*this);

	Send();
}

void ClientOutput::Send() const
{
	if (m_Output == nullptr)
	{
		return;
	}

	Describe(Object(), m_Output->Facts());
}

HostOutput::~HostOutput()
{
	Close();
}

bool HostOutput::Open(wl_display& display, const SceneOutput& output)
{
	m_Facts = output;
	m_Global = Wayland::Server::WlOutput::Advertise(display, OutputVersion, *this);

	return m_Global != nullptr;
}

void HostOutput::Close() noexcept
{
	// The bindings first, so that a resource whose client outlives the monitor answers `release` and
	// reads nothing off a `SceneOutput` that is about to be somebody else's.
	for (ClientOutput* const binding : m_Bindings)
	{
		binding->Forget();
	}

	m_Bindings.clear();

	if (m_Global != nullptr)
	{
		wl_global_destroy(m_Global);
		m_Global = nullptr;
	}
}

void HostOutput::Update(const SceneOutput& output)
{
	const bool moved = Visible(m_Facts, output);

	m_Facts = output;

	if (!moved)
	{
		return;
	}

	// Over a copy for `SyncActivation`'s reason: sending is a wire write, and a client whose connection
	// has already failed is torn down inside libwayland — which destroys its resources and so edits
	// this list underneath the walk.
	const std::vector<ClientOutput*> bindings{ m_Bindings };

	for (const ClientOutput* const binding : bindings)
	{
		binding->Send();
	}
}

Wayland::Server::WlOutput HostOutput::ResourceFor(const wl_client& client) const noexcept
{
	for (const ClientOutput* const binding : m_Bindings)
	{
		if (binding->Object().WireClient() == &client)
		{
			return binding->Object();
		}
	}

	return {};
}

void HostOutput::Attach(ClientOutput& binding)
{
	m_Bindings.push_back(&binding);
}

void HostOutput::Detach(ClientOutput& binding) noexcept
{
	std::erase(m_Bindings, &binding);
}

Wayland::Server::WlOutputHandler* HostOutput::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	// Owns itself from here, as every object under every global does: destroyed by the client's
	// `release`, or with the client.
	return new ClientOutput{ *this };
}

void HostOutputs::Sync(wl_display& display, std::span<const SceneOutput> outputs)
{
	// Withdrawn first, so a machine that swapped one monitor for another inside a single iteration does
	// not hold two globals for one desk while the new one is advertised.
	std::erase_if(m_Outputs, [outputs](const std::unique_ptr<HostOutput>& held) {
		const bool kept =
			std::ranges::any_of(outputs, [&held](const SceneOutput& output) { return output.Id == held->Id(); });

		if (!kept)
		{
			held->Close();
		}

		return !kept;
	});

	std::vector<std::unique_ptr<HostOutput>> ordered;
	ordered.reserve(outputs.size());

	for (const SceneOutput& output : outputs)
	{
		const auto found = std::ranges::find_if(m_Outputs, [&output](const std::unique_ptr<HostOutput>& held) {
			return held->Id() == output.Id;
		});

		if (found != m_Outputs.end())
		{
			(*found)->Update(output);
			ordered.push_back(std::move(*found));

			continue;
		}

		auto advertised = std::make_unique<HostOutput>();

		if (!advertised->Open(display, output))
		{
			// Not fatal, unlike the globals `ClientHost::Open` refuses to start without: this is one
			// monitor's advertisement rather than the interface, so what is lost is that clients cannot
			// see that display while everything already on screen keeps working. It is logged by nothing
			// here because this runs per iteration and a failure that repeats would be a log per frame.
			continue;
		}

		ordered.push_back(std::move(advertised));
	}

	m_Outputs = std::move(ordered);
}

void SyncOutputEntry(HostContext& context, const HostOutputs& outputs, const SceneStore& scene)
{
	const std::span<const std::unique_ptr<HostOutput>> advertised = outputs.All();

	if (advertised.empty())
	{
		return;
	}

	// Iterated over a copy for `SyncActivation`'s reason: an `enter` is a wire write and a client whose
	// connection has already failed is torn down inside libwayland, which unmaps its windows and edits
	// the registry this walk is over.
	const std::vector<ClientXdgSurface*> windows{ context.Windows().begin(), context.Windows().end() };

	for (ClientXdgSurface* const window : windows)
	{
		ClientSurface* const surface = window->Content();

		if (surface == nullptr)
		{
			continue;
		}

		// **The window's container rather than the image under it**, which is what
		// `Protocol/Floor.h` places and therefore the entity that has a position in the world at all.
		// An unmapped window reaches nothing, which is the answer that takes it off every output it
		// was on — a client whose window is gone is not on a display.
		const OutputReach reach = window->IsMapped() ? Reach(scene, window->Window()) : 0;
		const OutputReach entered = surface->Entered();

		if (reach == entered)
		{
			continue;
		}

		wl_client* const client = surface->Object().WireClient();

		if (client == nullptr)
		{
			continue;
		}

		// What was actually told, which is not always what is true: an `enter` names an object, and a
		// client that has not bound this output has none to name.
		OutputReach applied = entered;

		for (std::size_t index = 0; index < advertised.size() && index < MaxReachableOutputs; ++index)
		{
			const OutputReach bit = OutputReach{ 1 } << index;

			if (((reach ^ entered) & bit) == 0)
			{
				continue;
			}

			const Wayland::Server::WlOutput object = advertised[index]->ResourceFor(*client);
			const bool entering = (reach & bit) != 0;

			if (!object.IsValid())
			{
				// **An enter with nothing to name stays owed and a leave does not.** A client that has
				// not walked the registry yet — or has bound only what it uses — has to hear about this
				// output the moment it binds one, so the bit is left unentered and the comparison sends
				// it on the next iteration. A leave, by contrast, is a client that let its own resource
				// go: it already knows, and holding the bit would compare unequal forever.
				applied = entering ? applied : (applied & ~bit);

				continue;
			}

			if (entering)
			{
				surface->Object().Enter(object);
			}
			else
			{
				surface->Object().Leave(object);
			}

			applied = (applied & ~bit) | (reach & bit);
		}

		surface->SetEntered(applied);
	}
}
