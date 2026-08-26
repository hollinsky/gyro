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
#include <string_view>

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

				spdlog::info("input: {} ({})", ::libinput_device_get_name(device), keys ? "keys" : "no keys");

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
						.When = Monotonic::FromMicroseconds(
							static_cast<std::int64_t>(::libinput_event_keyboard_get_time_usec(key))
						),
					}
				);

				break;
			}

			default:
				// Everything else is a pointer, a touch or a gesture, and there is nothing on the far end
				// of any of them yet. Dropped rather than queued: a seat that cannot route a click gains
				// nothing from remembering the clicks it could not route.
				break;
		}

		::libinput_event_destroy(event);
	}

	return {};
}
} // namespace Input
