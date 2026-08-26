// open() with O_CLOEXEC, which is POSIX, and libudev, which is Linux's. Named here for
// Compositor/Compositor.cpp's reason: what is wanted from the platform is stated rather than
// inherited from a build flag.
#define _POSIX_C_SOURCE 200809L

#include "Input/Devices.h"

#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <string_view>

#include "Core/Time.h"

namespace Input
{
namespace
{
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

	return descriptor >= 0 ? descriptor : -errno;
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

				spdlog::info(
					"input: {} ({})",
					::libinput_device_get_name(device),
					::libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD) ? "keyboard" : "no keys"
				);

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
