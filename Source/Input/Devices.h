#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/SlotAllocator.h"
#include "Seam/Input.h"

struct libinput;
struct libinput_device;
struct libinput_event;
struct udev;

// The keyboards, mice and touchpads on this machine, behind libinput.
//
// **One source for every device, which is the granularity libinput already has.** A libinput context
// hands out one descriptor for the whole seat and reports which device an event came from, so this is
// `Seam/EventSource.h`'s *one file, N producers* shape for the same reason the DRM device is: the
// alternative is one poll per keyboard, each reading its siblings' events off a shared queue.
//
// **Devices are opened directly, with no session behind them.** `open_restricted` is an `open`, and
// what makes it succeed is a udev rule rather than a seat manager handing over a descriptor —
// [decision 145](../../Docs/Decisions.md#145-the-drm-backend-takes-master-by-opening-the-node-and-libdrm-stops-at-the-frame-section) settled the same question for DRM master, and this is
// that answer applied to the second device class. So there is no pause, no resume, and no revoke: a
// device gyro can open it keeps until it is unplugged.
//
// **The udev backend rather than the path one**, which costs a link against libudev and buys hotplug.
// libinput links it either way, so nothing new is running on the machine — and a keyboard plugged in
// after boot is not an edge case for a compositor that starts before the login prompt.

namespace Input
{
// **What the udev rules actually admitted, counted as libinput opens the nodes.** This is the only
// place the question can be answered: `libinput_udev_assign_seat` returns success when every device
// on the seat was refused, so the enumeration's own return value says nothing, and the devices that
// did land are not in `m_Devices` until the first drain has run.
//
// **Three numbers rather than one, because the interesting failure is partial.** A rule that admits
// the touchpad and not the keyboard leaves a machine that opens devices, counts more than zero, and
// cannot be typed on — and on a laptop a lid switch, a power button and a row of hotkeys all present
// as keyboards, so *how many* is never the question. `Typists` is the count that can produce a
// letter, which is the same test the exclusive grab is gated on.
struct Admission
{
	std::size_t Opened = 0;
	std::size_t Refused = 0;
	std::size_t Typists = 0;
};

// Every device on one seat, and the keys they produce.
class Devices final : public IInput
{
public:
	~Devices() override;

	Devices(const Devices&) = delete;
	Devices& operator=(const Devices&) = delete;
	Devices(Devices&&) = delete;
	Devices& operator=(Devices&&) = delete;

	// Bring up a context against a seat, which is `seat0` unless something says otherwise.
	//
	// **A seat with no keyboard on it opens successfully**, and so does one where every device node
	// was refused. Devices arrive as events rather than as a list, so the first drain is what
	// discovers them, and refusing here would refuse a machine whose keyboard is plugged in a moment
	// later — a rule reapplies on the hotplug. What a caller can rely on is that failure means the
	// *context* could not be made: no udev, or a seat libinput would not take.
	//
	// **A deployment failure is a log line at error, not a `Failure`.** The refusals are counted as
	// they happen and named individually with the group that owns the node, and what this cannot do
	// is return them — a compositor that would not start because the keyboard is in the wrong group
	// is one that cannot be used to fix the group.
	[[nodiscard]] static Result<std::unique_ptr<Devices>> Open(std::string seat = "seat0");

	[[nodiscard]] RawFd Descriptor() const noexcept override;

	[[nodiscard]] Result<void> Drain() override;

private:
	Devices() = default;

	// Filled in by `open_restricted` as libinput enumerates, so it is complete by the time
	// `libinput_udev_assign_seat` returns and keeps counting across hotplug.
	Admission m_Admission;

	// Owned by the context and released with it, which is why neither is an `Fd`: libinput opened the
	// device nodes through the interface below and closes them itself.
	libinput* m_Context = nullptr;
	udev* m_Udev = nullptr;

	// Which `InputDeviceId` a libinput device is, and where those ids come from.
	//
	// **A map rather than libinput's own user data pointer**, which is what it is for and would hold a
	// generational id only by being punned into a `void*` — 64 bits of handle through a pointer that
	// is not one, and a cast this codebase's warning set is right to object to. Plugging a device in is
	// rare enough that a hash lookup per event is invisible beside the `read` that delivered it.
	//
	// SPEC: the capacity is decision 27's order of magnitude above anything real. A machine with more
	// than 256 input devices on one seat is one gyro would rather refuse than quietly mis-key.
	SlotAllocator<InputDeviceTag> m_Ids{ 256 };
	std::unordered_map<libinput_device*, InputDeviceId> m_Devices;

	// The id an event's device carries, or null for a device that arrived before gyro was watching —
	// which cannot happen through `Drain`, since a device is announced before it reports anything, and
	// is answered rather than asserted because a null id is a value every consumer already has to
	// handle.
	[[nodiscard]] InputDeviceId Identify(libinput_event* event) const;

	std::string m_Seat;
};
} // namespace Input
