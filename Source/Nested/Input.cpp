#include "Nested/Input.h"

#include <spdlog/spdlog.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <format>

namespace Nested
{
namespace
{
// What one detent is worth where the host does not say. `wl_pointer.axis` carries a distance in
// surface units already, so this is only ever the fallback for the `value120` bookkeeping below.
constexpr double DetentsPer120 = 120.0;

[[nodiscard]] ScrollAxis Axis(Wayland::WlPointerAxis axis) noexcept
{
	return axis == Wayland::WlPointerAxis::HorizontalScroll ? ScrollAxis::Horizontal : ScrollAxis::Vertical;
}
} // namespace

Result<void> NestedInput::Open()
{
	// Non-blocking because `Drain` reads once and must not block the dispatch thread where the counter
	// is already zero — a wait that ended on another descriptor is the ordinary case.
	const int descriptor = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	if (descriptor < 0)
	{
		return Failure(errno, "opening the nested input handoff");
	}

	m_Wake = Fd{ descriptor };

	return {};
}

void NestedInput::Bind(const Wayland::WlRegistry& registry, std::uint32_t name, std::uint32_t version)
{
	// Five is where `axis_source` and `axis_stop` arrive, which is the difference between a two-finger
	// flick that decays and one that stops dead — `Core/Input.h` has why that is not a detail. Above
	// that nothing here reads, so the ceiling is what these bindings were generated against and the
	// floor is what the host has.
	m_SeatObject = registry.Bind<Wayland::WlSeat>(name, std::min(version, 5U), m_Seat);
}

void NestedInput::Track(std::size_t index, Wire::ObjectId surface, PixelSize<DeviceSpace> size)
{
	if (index >= m_Windows.size())
	{
		return;
	}

	Window& window = m_Windows[index];

	// Named once. This runs again on every resize — the surface id survives one and the size does not —
	// and re-formatting the strings would hand `InputDevice` a view of a buffer that had just moved.
	if (window.Connector.empty())
	{
		window.Connector = std::format("nested-{}", index);
		window.Name = std::format("host pointer on nested-{}", index);
	}

	window.Surface = surface;
	window.Size = size;

	m_Count = std::max(m_Count, index + 1);
}

std::string_view NestedInput::Connector(std::size_t index) const noexcept
{
	return index < m_Count ? std::string_view{ m_Windows[index].Connector } : std::string_view{};
}

std::size_t NestedInput::Find(Wire::ObjectId surface) const noexcept
{
	for (std::size_t index = 0; index < m_Count; ++index)
	{
		if (m_Windows[index].Surface == surface)
		{
			return index;
		}
	}

	return m_Count;
}

void NestedInput::Push(const NestedInputEvent& event) noexcept
{
	const std::uint32_t tail = m_Tail.load(std::memory_order_relaxed);
	const std::uint32_t head = m_Head.load(std::memory_order_acquire);

	if (tail - head >= NestedInputQueue)
	{
		// **Dropped rather than blocked, and never logged here.** This runs inside the frame loop's
		// step, where allocating is an abort and a formatted line is an allocation; the count is what
		// crosses and `Drain` is where it is said.
		m_Dropped.fetch_add(1, std::memory_order_relaxed);

		return;
	}

	m_Queue[tail % NestedInputQueue] = event;
	m_Tail.store(tail + 1, std::memory_order_release);
	m_Queued = true;
}

void NestedInput::Notify() noexcept
{
	if (!m_Queued || !m_Wake.IsValid())
	{
		return;
	}

	m_Queued = false;

	const std::uint64_t one = 1;

	// Unchecked for `DispatchWait::Nudge`'s reason: the only reachable failure is a saturated counter,
	// which is a wakeup the dispatch thread has not consumed yet — exactly the one this wanted.
	[[maybe_unused]] const ssize_t rung = ::write(m_Wake.Get(), &one, sizeof(one));
}

Result<void> NestedInput::Drain()
{
	if (!m_Wake.IsValid())
	{
		return Failure(EBADF, "draining a nested input handoff that was never opened");
	}

	std::uint64_t counter = 0;

	[[maybe_unused]] const ssize_t taken = ::read(m_Wake.Get(), &counter, sizeof(counter));

	// **The ring is drained whether or not the doorbell rang.** A wakeup this thread took for another
	// descriptor is the ordinary case, and reading the queue is cheaper than the syscall that told us
	// about it.
	const std::uint32_t tail = m_Tail.load(std::memory_order_acquire);
	std::uint32_t head = m_Head.load(std::memory_order_relaxed);

	while (head != tail)
	{
		const NestedInputEvent event = m_Queue[head % NestedInputQueue];

		m_Head.store(head + 1, std::memory_order_release);
		++head;

		const Window& window = m_Windows[event.Window];
		const InputDeviceId device{ static_cast<std::uint32_t>(event.Window), 1 };

		switch (event.What)
		{
			case NestedInputEvent::Kind::Added:
				Added.Emit(
					InputDevice{
						.Id = device,
						.Name = window.Name,
						// The host puts the pointer at a place on this window, so it is stated rather than
				        // displaced — which is the whole reason the root has to bind it to an output.
						.Absolute = true,
						// **No millimetres, deliberately.** `Compositor/Binding.h`'s size rung matches a slab of
				        // glass against a panel's EDID, and a window has neither; answering with the host's idea
				        // of its own monitor would offer that rung evidence it has not got. The connector below
				        // is what binds this device, and it is correct by construction.
						.Size = std::nullopt,
						.Output = window.Connector,
					}
				);
				break;

			case NestedInputEvent::Kind::Position:
				Position.Emit(
					PointerPosition{
						.NormalizedX = event.NormalizedX,
						.NormalizedY = event.NormalizedY,
						.When = event.When,
						.Device = device,
					}
				);
				break;

			case NestedInputEvent::Kind::Button:
				Button.Emit(
					PointerButton{
						.Code = event.Code,
						.Pressed = event.Pressed,
						.When = event.When,
						.Device = device,
					}
				);
				break;

			case NestedInputEvent::Kind::Scroll:
				Scroll.Emit(
					PointerScroll{
						.Axis = event.Axis,
						.Source = event.Source,
						.Distance = event.Distance,
						.Clicks120 = event.Clicks120,
						.Stop = event.Stop,
						.When = event.When,
						.Device = device,
					}
				);
				break;
		}
	}

	// **Said from here because here is where saying it is legal**, and said once per new drop rather
	// than once per drop: a queue that overflowed once is a hiccup and one that overflows every drain
	// is the dispatch thread not running, which is the thing worth a line.
	if (const std::uint64_t dropped = m_Dropped.load(std::memory_order_relaxed); dropped != m_Reported)
	{
		spdlog::warn(
			"nested input: {} event(s) dropped between the host and the dispatch thread", dropped - m_Reported
		);

		m_Reported = dropped;
	}

	return {};
}

void NestedInput::Seat::OnCapabilities(Wayland::WlSeatCapability capabilities)
{
	const bool pointer = (capabilities & Wayland::WlSeatCapability::Pointer) == Wayland::WlSeatCapability::Pointer;

	if (!pointer)
	{
		// A seat can lose the capability, and the object is released rather than left bound: a
		// `wl_pointer` on a seat with no pointer is one the host is entitled to stop talking to, and
		// holding it would leave the window's focus stuck wherever it last was.
		if (m_Input->m_PointerObject.IsValid())
		{
			m_Input->m_PointerObject.Release();
			m_Input->m_PointerObject = {};
			m_Input->m_Focus = NoFocus;
		}

		return;
	}

	if (!m_Input->m_PointerObject.IsValid())
	{
		m_Input->m_PointerObject = Object().GetPointer(m_Input->m_Pointer);
	}
}

void NestedInput::Pointer::OnEnter(
	std::uint32_t serial,
	Wayland::WlSurface surface,
	Wire::Fixed surfaceX,
	Wire::Fixed surfaceY
)
{
	const std::size_t window = m_Input->Find(surface.Id());

	m_Input->m_Focus = window < m_Input->m_Count ? window : NoFocus;

	if (window >= m_Input->m_Count)
	{
		return;
	}

	// **The host's cursor is hidden the moment the pointer is over a gyro window**, and this is the
	// one request this file sends. gyro draws its own glyph — decision 152 — so leaving the host's
	// visible would put two arrows on screen a few pixels apart, which reads as a broken compositor
	// rather than as a nested one. A null surface is what `wl_pointer.set_cursor` spells that with.
	Object().SetCursor(serial, Wayland::WlSurface{}, 0, 0);

	if (!m_Input->m_Windows[window].Announced)
	{
		m_Input->m_Windows[window].Announced = true;
		m_Input->Push(
			NestedInputEvent{ .What = NestedInputEvent::Kind::Added, .Window = static_cast<std::uint32_t>(window) }
		);
	}

	OnMotion(0, surfaceX, surfaceY);
}

void NestedInput::Pointer::OnLeave(std::uint32_t, Wayland::WlSurface surface)
{
	if (m_Input->Find(surface.Id()) == m_Input->m_Focus)
	{
		m_Input->m_Focus = NoFocus;
	}

	// **Nothing is emitted, and the cursor stays where it was left.** There is no *the pointer is
	// gone* in `Seam/Input.h` and there should not be one for this: what a person sees is a cursor
	// parked against the edge of the window they just left, which is what a cursor does at the edge of
	// a monitor. Hiding it instead would make gyro's own glyph blink out every time the mouse crossed
	// a host window's title bar.
}

void NestedInput::Pointer::OnMotion(std::uint32_t, Wire::Fixed surfaceX, Wire::Fixed surfaceY)
{
	const std::size_t window = m_Input->m_Focus;

	if (window >= m_Input->m_Count)
	{
		return;
	}

	const PixelSize<DeviceSpace> size = m_Input->m_Windows[window].Size;

	if (size.Width <= 0 || size.Height <= 0)
	{
		return;
	}

	// **Surface-local coordinates are the window's own pixels while gyro commits at scale one**, which
	// it does: `Nested/Host.h` binds no `wp_viewporter` and the window is the mode's resolution. The
	// day a nested output takes a fractional scale, this division is what has to learn about it.
	m_Input->Push(
		NestedInputEvent{
			.What = NestedInputEvent::Kind::Position,
			.Window = static_cast<std::uint32_t>(window),
			.NormalizedX = surfaceX.ToDouble() / static_cast<double>(size.Width),
			.NormalizedY = surfaceY.ToDouble() / static_cast<double>(size.Height),
			.When = m_Input->m_Clock->Now(),
		}
	);
}

void NestedInput::Pointer::OnButton(
	std::uint32_t,
	std::uint32_t,
	std::uint32_t button,
	Wayland::WlPointerButtonState state
)
{
	if (m_Input->m_Focus >= m_Input->m_Count)
	{
		return;
	}

	m_Input->Push(
		NestedInputEvent{
			.What = NestedInputEvent::Kind::Button,
			.Window = static_cast<std::uint32_t>(m_Input->m_Focus),
			// Already evdev's numbering: `wl_pointer.button` says so, and it is the same space
	        // `Core/Input.h` carries — so this crosses unconverted, which is the point of that rule.
			.Code = button,
			.Pressed = state == Wayland::WlPointerButtonState::Pressed,
			.When = m_Input->m_Clock->Now(),
		}
	);
}

void NestedInput::Pointer::OnAxisSource(Wayland::WlPointerAxisSource axisSource)
{
	switch (axisSource)
	{
		case Wayland::WlPointerAxisSource::Finger:
			m_Input->m_Source = ScrollSource::Finger;
			break;

		case Wayland::WlPointerAxisSource::Continuous:
			m_Input->m_Source = ScrollSource::Continuous;
			break;

		case Wayland::WlPointerAxisSource::Wheel:
		case Wayland::WlPointerAxisSource::WheelTilt:
			m_Input->m_Source = ScrollSource::Wheel;
			break;
	}
}

void NestedInput::Pointer::OnAxisValue120(Wayland::WlPointerAxis, std::int32_t value120)
{
	// Held for the `axis` it belongs to, because the wire sends the high-resolution step *before* the
	// distance it describes and `Core/Input.h` carries both on one increment.
	m_Input->m_Pending120 = static_cast<double>(value120) / DetentsPer120;
}

void NestedInput::Pointer::OnAxis(std::uint32_t, Wayland::WlPointerAxis axis, Wire::Fixed value)
{
	const double clicks120 = m_Input->m_Pending120;

	m_Input->m_Pending120 = 0.0;

	if (m_Input->m_Focus >= m_Input->m_Count)
	{
		return;
	}

	m_Input->Push(
		NestedInputEvent{
			.What = NestedInputEvent::Kind::Scroll,
			.Window = static_cast<std::uint32_t>(m_Input->m_Focus),
			.Axis = Axis(axis),
			.Source = m_Input->m_Source,
			.Distance = value.ToDouble(),
			.Clicks120 = clicks120,
			.When = m_Input->m_Clock->Now(),
		}
	);
}

void NestedInput::Pointer::OnAxisStop(std::uint32_t, Wayland::WlPointerAxis axis)
{
	if (m_Input->m_Focus >= m_Input->m_Count)
	{
		return;
	}

	m_Input->Push(
		NestedInputEvent{
			.What = NestedInputEvent::Kind::Scroll,
			.Window = static_cast<std::uint32_t>(m_Input->m_Focus),
			.Axis = Axis(axis),
			.Source = m_Input->m_Source,
			.Stop = true,
			.When = m_Input->m_Clock->Now(),
		}
	);
}
} // namespace Nested
