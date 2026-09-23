#pragma once

#include <algorithm>
#include <array>
#include <string_view>

// Whether a card's vblank is an interrupt from the display or a timer in the kernel.
//
// **The question `wp_presentation_feedback`'s `hw_completion` asks, and one the kernel does not
// answer.** A driver with no vblank at all fails `DRM_IOCTL_CRTC_GET_SEQUENCE` with `EOPNOTSUPP`
// (`drm_crtc_get_sequence_ioctl` checks `drm_dev_has_vblank` first), which `DrmDevice` asks directly.
// But `drm_vblank_helper.c` lets a driver with no interrupt register an `hrtimer` at the mode's refresh
// instead, and from userspace that is indistinguishable from a panel: the counter counts, the events
// arrive, the timestamps are monotonic. Nothing in the uapi says which one is behind a CRTC.
//
// **So this is a list of the drivers that use the timer, and it is a stopgap.** It was taken from every
// caller of `DRM_CRTC_VBLANK_TIMER_FUNCS` and `drm_crtc_vblank_start_timer` in the kernel tree, and
// it rots the moment another driver adopts the helper. The fix is the kernel saying so, which is in
// Docs/KernelWishlist.md. Two known misses: amdgpu's virtual display is `amdgpu` like the real one and
// is claimed as hardware, and vmwgfx is denied on its non-emulated path too. The first is a setup
// nobody runs gyro on; the second is leaving a flag off, which is always allowed.
namespace Drm
{
inline constexpr std::array<std::string_view, 7> TimerVblankDrivers{
	"bochs-drm", "cirrus-qemu", "hyperv_drm", "qxl", "virtio_gpu", "vkms", "vmwgfx",
};

// Whether a driver of this name is known to drive its vblank from a timer. The name is what
// `drmGetVersion` reports, which is the driver's `struct drm_driver::name`.
[[nodiscard]] constexpr bool HasTimerVblank(std::string_view driver) noexcept
{
	return std::ranges::find(TimerVblankDrivers, driver) != TimerVblankDrivers.end();
}
} // namespace Drm
