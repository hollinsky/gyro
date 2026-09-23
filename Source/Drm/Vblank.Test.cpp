#include "Drm/Vblank.h"

#include "Testing/Test.h"

// Which drivers are denied `hw_completion` by name. Exact matches only, because the name is the kernel's
// and a prefix match would be a guess about drivers nobody has read.

GYRO_TEST(Vblank, TimerDriversAreKnown)
{
	GYRO_CHECK(Drm::HasTimerVblank("virtio_gpu"));
	GYRO_CHECK(Drm::HasTimerVblank("vkms"));
	GYRO_CHECK(Drm::HasTimerVblank("bochs-drm"));
}

GYRO_TEST(Vblank, PanelDriversAreNot)
{
	GYRO_CHECK(!Drm::HasTimerVblank("i915"));
	GYRO_CHECK(!Drm::HasTimerVblank("xe"));
	GYRO_CHECK(!Drm::HasTimerVblank("amdgpu"));
	GYRO_CHECK(!Drm::HasTimerVblank("msm"));
	GYRO_CHECK(!Drm::HasTimerVblank("virtio"));
	GYRO_CHECK(!Drm::HasTimerVblank(""));
}
