// open() with O_CLOEXEC, which is POSIX, and libudev and the evdev ioctls, which are Linux's. Named here for
// Compositor/Compositor.cpp's reason: what is wanted from the platform is stated rather than
// inherited from a build flag.
#define _POSIX_C_SOURCE 200809L

#include "Input/Devices.h"

#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

#include "Core/Time.h"

namespace Input
{
namespace
{
// Whether this device is something a person types on, which is a narrower question than *does it
// produce key codes*.
//
// **A lid switch, a power button, a headset jack and a row of hotkeys all report as keyboards**, and
// on this machine they outnumber the one real keyboard. libinput is not wrong — they emit key and
// switch codes, and gyro wants them: `SW_LID` is how the display lifetime learns the laptop closed.
// What separates them is whether the device can produce a *letter*, and that is the only capability
// that matters for the grab below: a lid cannot type into a login prompt.
[[nodiscard]] bool TypesLetters(int descriptor)
{
	// One bit per key code, which is how the kernel reports the whole capability set in one call.
	std::array<unsigned long, (KEY_MAX / (8 * sizeof(unsigned long))) + 1> keys{};

	if (::ioctl(descriptor, EVIOCGBIT(EV_KEY, static_cast<int>(keys.size() * sizeof(unsigned long))), keys.data()) < 0)
	{
		return false;
	}

	const auto has = [&keys](std::size_t code) {
		constexpr std::size_t Bits = 8 * sizeof(unsigned long);

		return (keys[code / Bits] & (1UL << (code % Bits))) != 0;
	};

	// `q` and `a` rather than the whole alphabet: a device with two letters on it is a keyboard, and a
	// device with none cannot be typed on whatever else it carries.
	return has(KEY_Q) && has(KEY_A);
}

// **Exclusive, and only for the devices that can type.** Without this the kernel's own keyboard
// handler keeps translating every keystroke — the `kbd` handler bound to the same device — so
// somebody typing at gyro is also typing into whatever getty is sitting on a VT behind it. That is a
// security hole rather than an untidiness: a logged-in shell back there receives what a person typed
// at the screen. Every other compositor closes it from the other end, by having logind put the
// session's VT into `K_OFF`; gyro has no session and no VT, so the handover step that would do it
// never happens and the exclusivity has to come from the device.
//
// **The grab is released when the descriptor closes, which is what makes it the safe half of the
// trade.** A gyro that crashes leaves a machine whose keyboard still works. What it costs is keyboard
// sysrq, which lives in the handler this shuts out — carried in Open.md, along with the `KDSKBMODE`
// route that keeps it and the reason that route is not free.
//
// Failure is not fatal and not silent: a device somebody else already grabbed is one gyro reads
// alongside them, which is worse than exclusive and much better than no keyboard at all.
void Claim(const char* path, int descriptor)
{
	if (!TypesLetters(descriptor))
	{
		return;
	}

	if (::ioctl(descriptor, EVIOCGRAB, 1) < 0)
	{
		spdlog::warn("{} could not be taken exclusively: {}", path, std::strerror(errno));
	}
}

// **The whole of the session story, and it is an `open`.** A seat manager's version of this hands
// back a descriptor over D-Bus and can take it away again; gyro's does not exist, because there is no
// VT to switch to and nothing to be revoked by. Access is a udev rule on the device node, so a
// failure here is a deployment fact — the wrong group, or a rule that did not apply — and it is
// reported as the errno the caller can act on rather than swallowed into "no input".
int OpenRestricted(const char* path, int flags, void* /*user*/)
{
	// `O_CLOEXEC` on top of libinput's own flags, because everything gyro opens is closed across an
	// exec and there is nothing here to make an exception of.
	const int descriptor = ::open(path, flags | O_CLOEXEC);

	if (descriptor < 0)
	{
		return -errno;
	}

	Claim(path, descriptor);

	return descriptor;
}

void CloseRestricted(int descriptor, void* /*user*/)
{
	::close(descriptor);
}

constexpr libinput_interface Interface{
	.open_restricted = &OpenRestricted,
	.close_restricted = &CloseRestricted,
};

// libinput's own log lines, which are the only account of *why* a device was rejected — a keyboard
// with no keys, a tablet whose calibration is nonsense — and are worth having on a compositor that
// starts before anything a person could read a journal with.
void Log(libinput* /*context*/, libinput_log_priority priority, const char* format, va_list arguments)
{
	// Bounded and on the stack: this runs on the dispatch thread, and a heap allocation to print a
	// diagnostic is a cost paid on the path a keystroke takes.
	std::array<char, 256> line{};

	const int written = std::vsnprintf(line.data(), line.size(), format, arguments);

	if (written <= 0)
	{
		return;
	}

	// libinput's own lines end in a newline and spdlog adds its own.
	std::string_view text{ line.data(), static_cast<std::size_t>(written) };

	while (!text.empty() && text.back() == '\n')
	{
		text.remove_suffix(1);
	}

	if (priority == LIBINPUT_LOG_PRIORITY_ERROR)
	{
		spdlog::error("libinput: {}", text);
	}
	else
	{
		spdlog::debug("libinput: {}", text);
	}
}
// libinput reports `CLOCK_MONOTONIC` microseconds, which Core/Time.h's ingest surface is named for.
// Every event below is stamped through here and nowhere else, so there is one conversion in the input
// path rather than one per event type.
[[nodiscard]] Instant At(std::uint64_t microseconds)
{
	return Monotonic::FromMicroseconds(static_cast<std::int64_t>(microseconds));
}

// An axis a tool reports, or nothing where this tool does not have it — which is per tool rather than
// per tablet, since a puck on the same tablet as a pen reports neither pressure nor tilt.
[[nodiscard]] std::optional<double> Axis(bool present, double value)
{
	return present ? std::optional<double>{ value } : std::nullopt;
}

[[nodiscard]] ToolKind KindOf(libinput_tablet_tool* tool)
{
	switch (::libinput_tablet_tool_get_type(tool))
	{
		case LIBINPUT_TABLET_TOOL_TYPE_PEN:
			return ToolKind::Pen;
		case LIBINPUT_TABLET_TOOL_TYPE_ERASER:
			return ToolKind::Eraser;
		case LIBINPUT_TABLET_TOOL_TYPE_BRUSH:
			return ToolKind::Brush;
		case LIBINPUT_TABLET_TOOL_TYPE_PENCIL:
			return ToolKind::Pencil;
		case LIBINPUT_TABLET_TOOL_TYPE_AIRBRUSH:
			return ToolKind::Airbrush;
		case LIBINPUT_TABLET_TOOL_TYPE_MOUSE:
			return ToolKind::Mouse;
		case LIBINPUT_TABLET_TOOL_TYPE_LENS:
			return ToolKind::Lens;
		case LIBINPUT_TABLET_TOOL_TYPE_TOTEM:
			return ToolKind::Totem;
		default:
			return ToolKind::Unknown;
	}
}

// Everything a tablet event carries except which transition it was.
//
// **The axes are read on every event and not only on an axis one.** libinput's tablet events all
// carry the tool's full state, so a proximity-in already knows where the tip is and a tip-down already
// knows its pressure — and a consumer that had to remember the last axis event to interpret a tip
// would be keeping a copy of what it was just handed.
[[nodiscard]] ToolEvent StateOf(libinput_event_tablet_tool* event, InputDeviceId device)
{
	libinput_tablet_tool* const tool = ::libinput_event_tablet_tool_get_tool(event);

	return ToolEvent{
		.Kind = KindOf(tool),
		.Serial = ::libinput_tablet_tool_get_serial(tool),
		.TipDown = ::libinput_event_tablet_tool_get_tip_state(event) == LIBINPUT_TABLET_TOOL_TIP_DOWN,
		// Normalized by transforming onto a one-unit-wide area, which is the door libinput gives for a
		// fraction rather than the millimetres `get_x` reports — see Core/Input.h on why a coordinate
		// here is the device's own and not a place on a screen.
		.NormalizedX = ::libinput_event_tablet_tool_get_x_transformed(event, 1),
		.NormalizedY = ::libinput_event_tablet_tool_get_y_transformed(event, 1),
		.Pressure = Axis(::libinput_tablet_tool_has_pressure(tool), ::libinput_event_tablet_tool_get_pressure(event)),
		.Distance = Axis(::libinput_tablet_tool_has_distance(tool), ::libinput_event_tablet_tool_get_distance(event)),
		.TiltXDegrees = Axis(::libinput_tablet_tool_has_tilt(tool), ::libinput_event_tablet_tool_get_tilt_x(event)),
		.TiltYDegrees = Axis(::libinput_tablet_tool_has_tilt(tool), ::libinput_event_tablet_tool_get_tilt_y(event)),
		.RotationDegrees =
			Axis(::libinput_tablet_tool_has_rotation(tool), ::libinput_event_tablet_tool_get_rotation(event)),
		.Slider =
			Axis(::libinput_tablet_tool_has_slider(tool), ::libinput_event_tablet_tool_get_slider_position(event)),
		.When = At(::libinput_event_tablet_tool_get_time_usec(event)),
		.Device = device,
	};
}
} // namespace

Devices::~Devices()
{
	if (m_Context != nullptr)
	{
		::libinput_unref(m_Context);
	}

	if (m_Udev != nullptr)
	{
		::udev_unref(m_Udev);
	}
}

Result<std::unique_ptr<Devices>> Devices::Open(std::string seat)
{
	std::unique_ptr<Devices> devices{ new Devices{} };

	devices->m_Seat = std::move(seat);
	devices->m_Udev = ::udev_new();

	if (devices->m_Udev == nullptr)
	{
		return Failure(ENOMEM, "opening a udev context for input");
	}

	devices->m_Context = ::libinput_udev_create_context(&Interface, nullptr, devices->m_Udev);

	if (devices->m_Context == nullptr)
	{
		return Failure(ENODEV, "creating a libinput context");
	}

	::libinput_log_set_handler(devices->m_Context, &Log);
	::libinput_log_set_priority(devices->m_Context, LIBINPUT_LOG_PRIORITY_ERROR);

	// **The seat is where permission actually bites.** This enumerates the seat's devices and opens
	// every one of them through the interface above, so a machine whose udev rules have not been
	// applied fails here rather than at the first keystroke that does not arrive.
	if (::libinput_udev_assign_seat(devices->m_Context, devices->m_Seat.c_str()) != 0)
	{
		return Failure(EACCES, std::format("assigning libinput to seat {}", devices->m_Seat));
	}

	return devices;
}

InputDeviceId Devices::Identify(libinput_event* event) const
{
	const auto found = m_Devices.find(::libinput_event_get_device(event));

	return found == m_Devices.end() ? InputDeviceId{} : found->second;
}

RawFd Devices::Descriptor() const noexcept
{
	return m_Context != nullptr ? RawFd{ ::libinput_get_fd(m_Context) } : RawFd{};
}

Result<void> Devices::Drain()
{
	if (m_Context == nullptr)
	{
		return Failure(ENODEV, "draining input that was never opened");
	}

	// One read of the descriptor into libinput's own queue, then the queue to empty. Both halves are
	// needed: `libinput_dispatch` is what makes the file unreadable again, and it is the queue rather
	// than the file that the events come out of.
	if (const int dispatched = ::libinput_dispatch(m_Context); dispatched != 0 && dispatched != -EAGAIN)
	{
		return Failure(-dispatched, "reading input events");
	}

	while (libinput_event* const event = ::libinput_get_event(m_Context))
	{
		switch (::libinput_event_get_type(event))
		{
			case LIBINPUT_EVENT_DEVICE_ADDED:
			{
				// Said out loud because the failure this catches is silence: udev rules that admit the
				// touchpad and not the keyboard produce a compositor that looks like it has input.
				libinput_device* const device = ::libinput_event_get_device(event);

				// **Three states rather than two**, because the middle one is most of the list on a
				// laptop and reads as a bug otherwise: a lid, a power button and a headset jack all
				// report a keyboard capability, and none of them is a keyboard. Only the last state is
				// taken exclusively, so the line also says which devices gyro is standing in front of.
				const bool keys = ::libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD);

				// **A refusal is a log line and not a failure**, because the alternative is a machine
				// with no input at all over a device somebody plugged into the two hundred and
				// fifty-seventh port. What that device produces is dropped below, where a null id is
				// what an unidentified event has.
				const std::optional<InputDeviceId> id = m_Ids.Allocate();

				if (!id.has_value())
				{
					spdlog::error("input: no identity left for {}", ::libinput_device_get_name(device));

					break;
				}

				m_Devices.emplace(device, *id);

				spdlog::info(
					"input: {} ({}) is {}", ::libinput_device_get_name(device), keys ? "keys" : "no keys", *id
				);

				break;
			}

			case LIBINPUT_EVENT_DEVICE_REMOVED:
			{
				libinput_device* const device = ::libinput_event_get_device(event);
				const auto found = m_Devices.find(device);

				if (found == m_Devices.end())
				{
					break;
				}

				// Announced before it is freed, so an observer holding a touch sequence keyed to this
				// device can still compare the id it is holding against the one being retired.
				Removed.Emit(found->second);

				m_Ids.Free(found->second);
				m_Devices.erase(found);

				spdlog::info("input: {} is gone", ::libinput_device_get_name(device));

				break;
			}

			case LIBINPUT_EVENT_KEYBOARD_KEY:
			{
				libinput_event_keyboard* const key = ::libinput_event_get_keyboard_event(event);

				// **The device's own timestamp, converted here and nowhere else.** libinput reports
				// `CLOCK_MONOTONIC` microseconds, which is the domain Core/Time.h's ingest surface is
				// named for, and this is the one place in the input path entitled to make that judgement.
				Key.Emit(
					KeyEvent{
						.Code = ::libinput_event_keyboard_get_key(key),
						.Pressed = ::libinput_event_keyboard_get_key_state(key) == LIBINPUT_KEY_STATE_PRESSED,
						.When = At(::libinput_event_keyboard_get_time_usec(key)),
						.Device = Identify(event),
					}
				);

				break;
			}

			case LIBINPUT_EVENT_POINTER_MOTION:
			{
				libinput_event_pointer* const motion = ::libinput_event_get_pointer_event(event);

				// **Both displacements, and neither derived from the other.** The accelerated one moves
				// the cursor; the unaccelerated one is what `zwp_relative_pointer_v1` owes a game that
				// is reading the mouse directly, so that a curve is not applied to it twice.
				//
				// **Acceleration has already happened, here, per event.** libinput's curve is nonlinear
				// in velocity, so this is the last place the numbers can be summed without changing what
				// they mean — `Scene/Pointer.h` does the summing on the far side and says so.
				Motion.Emit(
					PointerMotion{
						.DeltaX = ::libinput_event_pointer_get_dx(motion),
						.DeltaY = ::libinput_event_pointer_get_dy(motion),
						.UnacceleratedX = ::libinput_event_pointer_get_dx_unaccelerated(motion),
						.UnacceleratedY = ::libinput_event_pointer_get_dy_unaccelerated(motion),
						.When = At(::libinput_event_pointer_get_time_usec(motion)),
						.Device = Identify(event),
					}
				);

				break;
			}

			case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
			{
				libinput_event_pointer* const motion = ::libinput_event_get_pointer_event(event);

				Position.Emit(
					PointerPosition{
						// Transformed onto a one-unit-wide area, which is libinput's door for a fraction
						// of the device rather than the millimetres it otherwise reports.
						.NormalizedX = ::libinput_event_pointer_get_absolute_x_transformed(motion, 1),
						.NormalizedY = ::libinput_event_pointer_get_absolute_y_transformed(motion, 1),
						.When = At(::libinput_event_pointer_get_time_usec(motion)),
						.Device = Identify(event),
					}
				);

				break;
			}

			case LIBINPUT_EVENT_POINTER_BUTTON:
			{
				libinput_event_pointer* const button = ::libinput_event_get_pointer_event(event);

				Button.Emit(
					PointerButton{
						.Code = ::libinput_event_pointer_get_button(button),
						.Pressed = ::libinput_event_pointer_get_button_state(button) == LIBINPUT_BUTTON_STATE_PRESSED,
						.When = At(::libinput_event_pointer_get_time_usec(button)),
						.Device = Identify(event),
					}
				);

				break;
			}

			case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
			case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
			case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
			{
				libinput_event_pointer* const scroll = ::libinput_event_get_pointer_event(event);
				const libinput_event_type kind = ::libinput_event_get_type(event);

				const ScrollSource source = kind == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL  ? ScrollSource::Wheel :
				                            kind == LIBINPUT_EVENT_POINTER_SCROLL_FINGER ? ScrollSource::Finger :
				                                                                           ScrollSource::Continuous;

				const Instant when = At(::libinput_event_pointer_get_time_usec(scroll));
				const InputDeviceId device = Identify(event);

				// **One event per axis, because that is what the wire carries.** A diagonal two-finger
				// scroll arrives from libinput as one event with two axes on it and leaves here as two,
				// with the same instant — which is the grouping `wl_pointer.frame` puts back together on
				// the far side, and is not this module's to make.
				constexpr std::pair<libinput_pointer_axis, ScrollAxis> Axes[]{
					{ LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, ScrollAxis::Vertical },
					{ LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL, ScrollAxis::Horizontal },
				};

				for (const auto& [axis, which] : Axes)
				{
					if (!::libinput_event_pointer_has_axis(scroll, axis))
					{
						continue;
					}

					const double distance = ::libinput_event_pointer_get_scroll_value(scroll, axis);

					Scroll.Emit(
						PointerScroll{
							.Axis = which,
							.Source = source,
							.Distance = distance,
							.Clicks120 = source == ScrollSource::Wheel ?
					                         ::libinput_event_pointer_get_scroll_value_v120(scroll, axis) :
					                         0.0,
							// **A finger event with no distance is the lift**, which is the only way a
					        // toolkit learns to start decelerating a flick rather than stopping it dead.
					        // libinput says so by reporting zero, and no other source can say it at all.
							.Stop = source == ScrollSource::Finger && distance == 0.0,
							.When = when,
							.Device = device,
						}
					);
				}

				break;
			}

			case LIBINPUT_EVENT_TOUCH_DOWN:
			case LIBINPUT_EVENT_TOUCH_MOTION:
			case LIBINPUT_EVENT_TOUCH_UP:
			case LIBINPUT_EVENT_TOUCH_CANCEL:
			{
				libinput_event_touch* const touch = ::libinput_event_get_touch_event(event);
				const libinput_event_type kind = ::libinput_event_get_type(event);

				const bool positioned = kind == LIBINPUT_EVENT_TOUCH_DOWN || kind == LIBINPUT_EVENT_TOUCH_MOTION;

				const TouchPhase phase = kind == LIBINPUT_EVENT_TOUCH_DOWN   ? TouchPhase::Down :
				                         kind == LIBINPUT_EVENT_TOUCH_MOTION ? TouchPhase::Motion :
				                         kind == LIBINPUT_EVENT_TOUCH_UP     ? TouchPhase::Up :
				                                                               TouchPhase::Cancel;

				Touch.Emit(
					TouchEvent{
						.Point = ::libinput_event_touch_get_slot(touch),
						.Phase = phase,
						// An up and a cancel report no position at all, and asking for one is undefined
				        // rather than merely stale. Zero rather than the last known point, per
				        // Core/Input.h: a lift that repeats a coordinate cannot be told from one that
				        // moved.
						.NormalizedX = positioned ? ::libinput_event_touch_get_x_transformed(touch, 1) : 0.0,
						.NormalizedY = positioned ? ::libinput_event_touch_get_y_transformed(touch, 1) : 0.0,
						.When = At(::libinput_event_touch_get_time_usec(touch)),
						.Device = Identify(event),
					}
				);

				break;
			}

			case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
			{
				libinput_event_tablet_tool* const tablet = ::libinput_event_get_tablet_tool_event(event);

				ToolEvent tool = StateOf(tablet, Identify(event));
				tool.Phase = ::libinput_event_tablet_tool_get_proximity_state(tablet) ==
				                     LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN ?
				                 ToolPhase::ProximityIn :
				                 ToolPhase::ProximityOut;

				Tool.Emit(tool);

				break;
			}

			case LIBINPUT_EVENT_TABLET_TOOL_TIP:
			{
				libinput_event_tablet_tool* const tablet = ::libinput_event_get_tablet_tool_event(event);

				ToolEvent tool = StateOf(tablet, Identify(event));
				tool.Phase = ToolPhase::Tip;

				Tool.Emit(tool);

				break;
			}

			case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
			{
				libinput_event_tablet_tool* const tablet = ::libinput_event_get_tablet_tool_event(event);

				ToolEvent tool = StateOf(tablet, Identify(event));
				tool.Phase = ToolPhase::Button;
				tool.ButtonCode = ::libinput_event_tablet_tool_get_button(tablet);
				tool.ButtonPressed =
					::libinput_event_tablet_tool_get_button_state(tablet) == LIBINPUT_BUTTON_STATE_PRESSED;

				Tool.Emit(tool);

				break;
			}

			case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
			{
				libinput_event_tablet_tool* const tablet = ::libinput_event_get_tablet_tool_event(event);

				ToolEvent tool = StateOf(tablet, Identify(event));
				tool.Phase = ToolPhase::Motion;

				Tool.Emit(tool);

				break;
			}

			default:
				// What is left is the pinch and swipe gestures, the tablet pads, the switches and
				// libinput's own frame markers. Each wants a consumer that does not exist — a recognizer,
				// a pad protocol, the display lifetime that `SW_LID` belongs to — and dropping them is
				// what this module does with an event nothing is asking for. `TOUCH_FRAME` is in here on
				// purpose: the grouping is the far side's, per Seam/Input.h.
				break;
		}

		::libinput_event_destroy(event);
	}

	return {};
}
} // namespace Input
