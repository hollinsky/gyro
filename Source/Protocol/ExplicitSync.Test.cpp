// `eventfd` and the DRM ioctls, which is what the subject of this file is made of.
#define _GNU_SOURCE 1

#include "Protocol/ExplicitSync.h"

#include <drm/drm.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Protocol/Dmabuf.h"
#include "Protocol/Sync.h"
#include "Testing/Test.h"

// What gyro does with a client's timeline, asserted against the kernel rather than against a fake.
//
// **There is no fake worth writing here.** Everything this file is about is `drm_syncobj` semantics —
// that a query reports what has signalled rather than what has been promised, that a point may be
// watched before it exists, and that signalling a point signals every point below it — and a stand-in
// that agreed with gyro's reading of those would prove only that the reading is self-consistent. So
// the test *is* a client: it creates a syncobj with the same ioctls a toolkit's driver would, exports
// it, and hands the descriptor over exactly as `import_timeline` does.
//
// **A machine with no DRM node runs these as passes rather than as failures**, which is the same
// answer gyro itself gives: the global is not advertised there, so there is no behaviour to assert.
// The guard is checked rather than silent, so a machine that *does* have a node cannot quietly skip.
//
// The wire half — a client binding the global, naming points, and its commit being held — is
// Integration's, where there is a real connection to drive it over.

namespace
{
// The device number of a node that exists, or zero on a machine with none — which is exactly what
// `ITextures::MainDevice()` hands `ExplicitSync::Open`, and the number it now refuses to guess past.
[[nodiscard]] std::uint64_t AnyNode() noexcept
{
	for (const char* const path : { "/dev/dri/renderD128", "/dev/dri/card0", "/dev/dri/card1" })
	{
		struct ::stat status = {};

		if (::stat(path, &status) == 0)
		{
			return static_cast<std::uint64_t>(status.st_rdev);
		}
	}

	return 0;
}

// A `wl_event_loop` with nothing on it, since `ExplicitSync::Open` wants one to hand out and these
// tests never dispatch.
class Loop
{
public:
	Loop() noexcept : m_Loop{ wl_event_loop_create() } {}

	~Loop()
	{
		if (m_Loop != nullptr)
		{
			wl_event_loop_destroy(m_Loop);
		}
	}

	Loop(const Loop&) = delete;
	Loop& operator=(const Loop&) = delete;
	Loop(Loop&&) = delete;
	Loop& operator=(Loop&&) = delete;

	[[nodiscard]] wl_event_loop* Get() const noexcept { return m_Loop; }

private:
	wl_event_loop* m_Loop = nullptr;
};

// The client's half: a counter it owns and a descriptor it hands over.
struct ClientTimeline
{
	Fd Device;
	Fd Descriptor;
	std::uint32_t Handle = 0;

	[[nodiscard]] bool IsValid() const noexcept { return Descriptor.IsValid(); }

	// The client finishing its frame.
	[[nodiscard]] bool Signal(std::uint64_t point) const noexcept
	{
		std::uint64_t value = point;
		std::uint32_t handle = Handle;

		drm_syncobj_timeline_array signal = {};
		signal.handles = reinterpret_cast<std::uintptr_t>(&handle);
		signal.points = reinterpret_cast<std::uintptr_t>(&value);
		signal.count_handles = 1;

		return ::ioctl(Device.Get(), DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal) == 0;
	}
};

// Just enough of the texture space to mint an id and reclaim it, which is the whole of what a release
// point hangs off. `Dmabuf.Test.cpp` has the fuller one; what is needed here is the watermark passing.
class OneIdAtATime final : public ITextures
{
public:
	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId>
	Adopt(PixelSize<BufferSpace>, std::uint32_t, std::span<const std::byte>, TextureAlpha) override
	{
		return Failure(ENODEV, "this fake takes descriptors only");
	}

	[[nodiscard]] Result<TextureId>
	Adopt(PixelSize<BufferSpace>, TextureFormat, std::span<const TexturePlane>, ITextureRelease* release) override
	{
		++m_Minted;

		m_Live.push_back(release);

		return TextureId{ m_Minted, 1 };
	}

	void Retire(TextureId) noexcept override {}

	// The watermark passed the oldest id, which is what `TextureRegistry::Reclaim` does for real.
	void ReclaimOldest() noexcept
	{
		if (m_Live.empty())
		{
			return;
		}

		ITextureRelease* const release = m_Live.front();

		m_Live.erase(m_Live.begin());

		if (release != nullptr)
		{
			release->OnTextureReleased();
		}
	}

private:
	std::uint32_t m_Minted = 0;
	std::vector<ITextureRelease*> m_Live;
};

// A syncobj made the way a client's driver makes one, on whatever node opens. Invalid where there is
// none, which is the whole of the guard every test below takes.
[[nodiscard]] ClientTimeline MakeClientTimeline(const ExplicitSync& sync)
{
	ClientTimeline made;

	// The same node gyro opened, reached the only way a test can: `ExplicitSync` names its path.
	const int opened = ::open(sync.Path().c_str(), O_RDWR | O_CLOEXEC);

	if (opened < 0)
	{
		return made;
	}

	made.Device = Fd{ opened };

	drm_syncobj_create create = {};

	if (::ioctl(made.Device.Get(), DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0)
	{
		return ClientTimeline{};
	}

	made.Handle = create.handle;

	drm_syncobj_handle handle = {};
	handle.handle = create.handle;
	handle.fd = InvalidFd;

	if (::ioctl(made.Device.Get(), DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle) != 0)
	{
		return ClientTimeline{};
	}

	made.Descriptor = Fd{ handle.fd };

	return made;
}
} // namespace

GYRO_TEST(ExplicitSync, AQueryReportsWhatHasSignalledRatherThanWhatWasPromised)
{
	const Loop loop;

	GYRO_REQUIRE(loop.Get() != nullptr);

	ExplicitSync sync;

	if (!sync.Open(AnyNode(), *loop.Get()))
	{
		// No DRM node, or a kernel with no eventfd ioctl. gyro advertises nothing there and so does this.
		GYRO_CHECK(!sync.IsAvailable());

		return;
	}

	ClientTimeline client = MakeClientTimeline(sync);

	GYRO_REQUIRE(client.IsValid());

	const Result<std::shared_ptr<SyncTimeline>> imported = sync.Import(std::move(client.Descriptor));

	GYRO_REQUIRE(imported.has_value());

	const SyncTimeline& timeline = **imported;

	// Nothing has happened yet, which is exactly the state a client is in when it commits before its
	// GPU work lands — and the state the whole hold exists for.
	GYRO_CHECK(!timeline.HasSignalled(1));

	GYRO_REQUIRE(client.Signal(4));

	// **Every point below it too**, which is the protocol's own rule and what makes a release point
	// safe to answer out of order.
	GYRO_CHECK(timeline.HasSignalled(1));
	GYRO_CHECK(timeline.HasSignalled(4));
	GYRO_CHECK(!timeline.HasSignalled(5));
}

GYRO_TEST(ExplicitSync, APointCanBeWatchedBeforeTheWorkBehindItExists)
{
	const Loop loop;

	GYRO_REQUIRE(loop.Get() != nullptr);

	ExplicitSync sync;

	if (!sync.Open(AnyNode(), *loop.Get()))
	{
		GYRO_CHECK(!sync.IsAvailable());

		return;
	}

	ClientTimeline client = MakeClientTimeline(sync);

	GYRO_REQUIRE(client.IsValid());

	const Result<std::shared_ptr<SyncTimeline>> imported = sync.Import(std::move(client.Descriptor));

	GYRO_REQUIRE(imported.has_value());

	const Fd wake{ ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) };

	GYRO_REQUIRE(wake.IsValid());

	// **Armed on a point the client has not submitted anything for**, which is the property that makes
	// this the ioctl rather than a poll on an exported `sync_file`: exporting a fence from a point with
	// no work behind it fails, and a compositor that had to wait for the client to submit before it
	// could wait for the client to finish would be back to polling.
	GYRO_REQUIRE((*imported)->Watch(9, wake.Borrow()).has_value());

	std::uint64_t count = 0;

	GYRO_CHECK(::read(wake.Get(), &count, sizeof(count)) < 0 && errno == EAGAIN);

	GYRO_REQUIRE(client.Signal(9));

	GYRO_CHECK(::read(wake.Get(), &count, sizeof(count)) == static_cast<ssize_t>(sizeof(count)));
	GYRO_CHECK(count > 0);
}

GYRO_TEST(ExplicitSync, AReleasePointIsSignalledOnlyWhenNothingIsReadingTheBufferAnyMore)
{
	const Loop loop;

	GYRO_REQUIRE(loop.Get() != nullptr);

	ExplicitSync sync;

	if (!sync.Open(AnyNode(), *loop.Get()))
	{
		GYRO_CHECK(!sync.IsAvailable());

		return;
	}

	ClientTimeline client = MakeClientTimeline(sync);

	GYRO_REQUIRE(client.IsValid());

	Result<std::shared_ptr<SyncTimeline>> imported = sync.Import(std::move(client.Descriptor));

	GYRO_REQUIRE(imported.has_value());

	HostContext context;
	OneIdAtATime textures;

	Fd memory{ ::memfd_create("gyro-syncobj-test", MFD_CLOEXEC) };

	GYRO_REQUIRE(memory.IsValid());

	std::vector<ClientDmabufBuffer::Plane> planes;
	planes.push_back(ClientDmabufBuffer::Plane{ .Descriptor = std::move(memory), .Offset = 0, .Stride = 32 });

	ClientDmabufBuffer buffer{
		context, PixelSize<BufferSpace>{ 8, 8 }, TextureFormat{ .Code = 0x34325241, .Modifier = 0 }, std::move(planes)
	};

	// **Committed twice before the first frame left the screen**, which is the arrangement that makes
	// the rule visible: two ids name the memory, and the buffer is still being read until both retire.
	GYRO_REQUIRE(buffer.Adopt(textures, SyncTimelinePoint{ *imported, 3 }).has_value());
	GYRO_REQUIRE(buffer.Adopt(textures, SyncTimelinePoint{ *imported, 7 }).has_value());
	GYRO_CHECK(buffer.OutstandingCount() == 2);

	textures.ReclaimOldest();

	// Still one id reading it. A client told otherwise here would be drawing into the buffer a panel is
	// scanning out, which reaches a person as a window tearing into itself and reaches a log as nothing.
	GYRO_CHECK(buffer.OutstandingCount() == 1);
	GYRO_CHECK(!(*imported)->HasSignalled(3));

	textures.ReclaimOldest();

	GYRO_CHECK(buffer.OutstandingCount() == 0);

	// Both points, because both commits are answered at the one moment nothing is reading.
	GYRO_CHECK((*imported)->HasSignalled(3));
	GYRO_CHECK((*imported)->HasSignalled(7));
}
