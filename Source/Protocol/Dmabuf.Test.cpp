#include "Protocol/Dmabuf.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"

// What a buffer made of descriptors owes, which is one thing and it is the release.
//
// **A buffer here has no `wl_resource` behind it**, per Shm.Test.cpp's reason: `Release` on an absent
// object does nothing, so what is asserted is the *count* of ids still naming the memory rather than
// the event a client received. The wire half — a client sending `add`, `create` and a `commit`, and
// reading `wl_buffer.release` back — is Integration's, where there is a real connection to read it on.
//
// The cases worth having are the ones where the count is not one. A buffer committed twice before the
// first frame left the screen, and a buffer the client destroyed while a frame was still drawn from
// it, are the two arrangements that turn a wrong answer here into a window tearing into itself.

namespace
{
// A registry that mints ids and remembers who to tell, which is the whole of what this file's
// contract with `Scene/Textures.h` is.
class FakeTextures final : public ITextures
{
public:
	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId>
	Adopt(PixelSize<BufferSpace>, std::uint32_t, std::span<const std::byte>, TextureAlpha) override
	{
		return Failure(ENODEV, "this fake takes descriptors only");
	}

	[[nodiscard]] Result<TextureId> Adopt(
		PixelSize<BufferSpace> size,
		TextureFormat format,
		std::span<const TexturePlane> planes,
		ITextureRelease* release
	) override
	{
		if (Refuse)
		{
			return Failure(EINVAL, "refused on purpose");
		}

		Sizes.push_back(size);
		Formats_.push_back(format);
		PlaneCounts.push_back(planes.size());

		// The descriptors are the caller's and this borrows them for the call, which is the contract
		// being asserted: every one has to still be open here.
		for (const TexturePlane& plane : planes)
		{
			Open.push_back(::fcntl(plane.Descriptor.Value, F_GETFD) >= 0);
		}

		++m_Minted;

		const TextureId id{ m_Minted, 1 };

		m_Live.emplace_back(id, release);

		return id;
	}

	void Retire(TextureId) noexcept override {}

	void Abandon(const ITextureRelease& release) noexcept override
	{
		for (std::pair<TextureId, ITextureRelease*>& held : m_Live)
		{
			if (held.second == &release)
			{
				held.second = nullptr;

				++Abandoned;
			}
		}
	}

	[[nodiscard]] std::span<const TextureFormat> Formats() const noexcept override { return Advertised; }

	// The watermark passed the oldest id, which is what `TextureRegistry::Reclaim` does for real.
	void ReclaimOldest() noexcept
	{
		if (m_Live.empty())
		{
			return;
		}

		ITextureRelease* const release = m_Live.front().second;

		m_Live.erase(m_Live.begin());

		if (release != nullptr)
		{
			release->OnTextureReleased();
		}
	}

	std::vector<TextureFormat> Advertised;
	std::vector<PixelSize<BufferSpace>> Sizes;
	std::vector<TextureFormat> Formats_;
	std::vector<std::size_t> PlaneCounts;
	std::vector<bool> Open;
	std::size_t Abandoned = 0;
	bool Refuse = false;

private:
	std::uint32_t m_Minted = 0;
	std::vector<std::pair<TextureId, ITextureRelease*>> m_Live;
};

[[nodiscard]] Fd MakeBuffer(std::size_t size)
{
	const int descriptor = ::memfd_create("gyro-dmabuf-test", MFD_CLOEXEC);

	if (descriptor < 0)
	{
		return {};
	}

	Fd fd{ descriptor };

	return ::ftruncate(descriptor, static_cast<off_t>(size)) == 0 ? std::move(fd) : Fd{};
}

[[nodiscard]] std::vector<ClientDmabufBuffer::Plane> OnePlane(Fd fd, std::uint32_t stride)
{
	std::vector<ClientDmabufBuffer::Plane> planes;

	planes.push_back(ClientDmabufBuffer::Plane{ .Descriptor = std::move(fd), .Offset = 0, .Stride = stride });

	return planes;
}
} // namespace

GYRO_TEST(Dmabuf, DescriptorsAreBorrowedRatherThanCopiedSoTheReleaseIsOwed)
{
	HostContext context;
	FakeTextures textures;

	Fd fd = MakeBuffer(64U * 32U * 4U);

	GYRO_REQUIRE(fd.IsValid());

	ClientDmabufBuffer buffer{ context,
		                       PixelSize<BufferSpace>{ 64, 32 },
		                       TextureFormat{ .Code = 0x34325241, .Modifier = 0 },
		                       OnePlane(std::move(fd), 64U * 4U) };

	// The one field the whole file turns on: a client told it may redraw would be drawing into the
	// buffer a panel is scanning out.
	GYRO_CHECK(!buffer.ReleasesImmediately());

	const Result<TextureId> adopted = buffer.Adopt(textures);

	GYRO_REQUIRE(adopted.has_value());
	GYRO_CHECK(buffer.OutstandingCount() == 1);
	GYRO_CHECK(textures.PlaneCounts.size() == 1 && textures.PlaneCounts[0] == 1);
	GYRO_CHECK(textures.Open.size() == 1 && textures.Open[0]);
	GYRO_CHECK(textures.Sizes[0] == PixelSize<BufferSpace>{ 64, 32 });
}

GYRO_TEST(Dmabuf, ABufferCommittedTwiceIsNotReleasedUntilBothFramesAreDone)
{
	HostContext context;
	FakeTextures textures;

	Fd fd = MakeBuffer(64U * 32U * 4U);

	GYRO_REQUIRE(fd.IsValid());

	ClientDmabufBuffer buffer{ context,
		                       PixelSize<BufferSpace>{ 64, 32 },
		                       TextureFormat{ .Code = 0x34325241, .Modifier = 0 },
		                       OnePlane(std::move(fd), 64U * 4U) };

	GYRO_REQUIRE(buffer.Adopt(textures).has_value());
	GYRO_REQUIRE(buffer.Adopt(textures).has_value());
	GYRO_CHECK(buffer.OutstandingCount() == 2);

	// The first frame's id retires and the second is still on screen. A release here is the tearing
	// this file exists to prevent.
	textures.ReclaimOldest();

	GYRO_CHECK(buffer.OutstandingCount() == 1);

	textures.ReclaimOldest();

	GYRO_CHECK(buffer.OutstandingCount() == 0);
}

GYRO_TEST(Dmabuf, ABufferTheClientDestroyedStopsBeingCalledBackAndTheIdStaysLive)
{
	HostContext context;
	FakeTextures textures;

	Fd fd = MakeBuffer(64U * 32U * 4U);

	GYRO_REQUIRE(fd.IsValid());

	// Constructed and destroyed with the texture space reachable, which is what a client destroying a
	// `wl_buffer` inside an `Advance` looks like.
	{
		ManualClock clock;
		SceneStore store{ clock };
		const HostContext::Dispatching dispatching{ context, store, textures };

		auto* const buffer = new ClientDmabufBuffer{ context,
			                                         PixelSize<BufferSpace>{ 64, 32 },
			                                         TextureFormat{ .Code = 0x34325241, .Modifier = 0 },
			                                         OnePlane(std::move(fd), 64U * 4U) };

		GYRO_REQUIRE(buffer->Adopt(textures).has_value());

		delete buffer;
	}

	GYRO_CHECK(textures.Abandoned == 1);

	// And the id is still live: nothing here says the frame thread has stopped drawing it, so the
	// reclaim that follows finds nobody to tell rather than a freed object.
	textures.ReclaimOldest();
}

GYRO_TEST(Dmabuf, AnInertBufferRefusesRatherThanAdoptingNothing)
{
	HostContext context;
	FakeTextures textures;

	ClientDmabufBuffer buffer{ context, {}, {}, std::vector<ClientDmabufBuffer::Plane>{} };

	GYRO_CHECK(buffer.IsInert());
	GYRO_CHECK(!buffer.Adopt(textures).has_value());
}
