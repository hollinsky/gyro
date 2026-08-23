#include "Seam/Buffer.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include "Core/Fd.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The ownership rules, and the description the seam sees. Nothing here allocates a real dmabuf —
// Virtual/Udmabuf.Test.cpp is where the kernel is involved — so these are about the type rather than
// the mechanism, which is the split that keeps a machine with no `/dev/udmabuf` able to check the
// part that is pure C++.

GYRO_TEST(SeamBuffer, DefaultDescribesNothing)
{
	const DmabufBuffer buffer;

	GYRO_CHECK(!buffer.IsValid());
	GYRO_CHECK(!buffer.IsMapped());
	GYRO_CHECK(buffer.Pixels().empty());

	// The seam's own validity check is the one that matters, because it is what a presenter's target
	// set is filtered by and what a renderer refuses on.
	GYRO_CHECK(!buffer.Describe().IsValid());
}

GYRO_TEST(SeamBuffer, DescribeCarriesOnePlane)
{
	// A descriptor that is a real open file, so that the `Fd` closing it is closing something. `dup`
	// of the test binary's own stderr is the cheapest such thing that needs no filesystem.
	Fd descriptor{ ::dup(2) };
	GYRO_REQUIRE(descriptor.IsValid());

	const int number = descriptor.Get();
	const DmabufBuffer buffer{ std::move(descriptor),
		                       PixelSize<DeviceSpace>{ 1920, 1080 },
		                       PixelFormat{ FormatXrgb8888, 0, ModifierLinear },
		                       7680,
		                       Mapping{} };

	GYRO_CHECK(buffer.IsValid());

	const RenderTarget target = buffer.Describe();

	GYRO_CHECK(target.IsValid());
	GYRO_CHECK_EQ(target.Size, PixelSize<DeviceSpace>{ 1920, 1080 });
	GYRO_CHECK_EQ(target.Format, PixelFormat{ FormatXrgb8888, 0, ModifierLinear });

	const DmabufImage* image = target.AsDmabuf();
	GYRO_REQUIRE(image != nullptr);
	GYRO_CHECK_EQ(image->PlaneCount, 1U);
	GYRO_CHECK_EQ(image->Planes[0].Stride, 7680U);
	GYRO_CHECK_EQ(image->Planes[0].Offset, 0U);

	// Borrowed, per Seam/RenderTarget.h. The description names the buffer's descriptor and does not
	// own it, which is the whole reason a target can be copied per frame.
	GYRO_CHECK_EQ(image->Planes[0].Descriptor, RawFd{ number });
}

GYRO_TEST(SeamBuffer, MoveLeavesTheSourceEmpty)
{
	Fd descriptor{ ::dup(2) };
	GYRO_REQUIRE(descriptor.IsValid());

	DmabufBuffer source{ std::move(descriptor),
		                 PixelSize<DeviceSpace>{ 64, 32 },
		                 PixelFormat{ FormatXrgb8888, 0, ModifierLinear },
		                 256,
		                 Mapping{} };

	const DmabufBuffer moved = std::move(source);

	GYRO_CHECK(moved.IsValid());
	// The moved-from buffer must describe nothing rather than describe the same descriptor: two
	// owners is a double close, and the second one lands on whatever number was reused.
	GYRO_CHECK(!source.IsValid());
	GYRO_CHECK(!source.Describe().IsValid());
}

GYRO_TEST(SeamBuffer, MappingUnmapsAndIsMoveOnly)
{
	// A mapping over pages this test owns, so that `Reset` has something real to unmap and a leak
	// checker has something to notice if it does not.
	constexpr std::size_t length = 4096;
	void* pages = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	GYRO_REQUIRE(pages != MAP_FAILED);

	Mapping mapping{ static_cast<std::byte*>(pages), length };
	GYRO_CHECK(mapping.IsValid());
	GYRO_CHECK_EQ(mapping.Bytes().size(), length);

	Mapping moved = std::move(mapping);
	GYRO_CHECK(moved.IsValid());
	GYRO_CHECK(!mapping.IsValid());
	GYRO_CHECK(mapping.Bytes().empty());

	moved.Reset();
	GYRO_CHECK(!moved.IsValid());

	// Idempotent, which is what makes the destructor safe after an explicit reset.
	moved.Reset();
	GYRO_CHECK(!moved.IsValid());
}

GYRO_TEST(SeamBuffer, FormatsAsWhatItIs)
{
	Fd descriptor{ ::dup(2) };
	GYRO_REQUIRE(descriptor.IsValid());

	const DmabufBuffer buffer{ std::move(descriptor),
		                       PixelSize<DeviceSpace>{ 64, 32 },
		                       PixelFormat{ FormatXrgb8888, 0, ModifierLinear },
		                       256,
		                       Mapping{} };

	const std::string text = std::format("{}", buffer);

	GYRO_CHECK(text.contains("64x32"));
	GYRO_CHECK(text.contains("XR24"));
	GYRO_CHECK(text.contains("stride 256"));
	GYRO_CHECK(text.contains("unmapped"));
}

GYRO_TEST(SeamBuffer, ADescribedBufferCarriesTheLayoutTheDeviceChose)
{
	// The form decision 120 added, and the reason it exists: a compressed modifier lays its metadata
	// out as a second plane at an offset into the same allocation, so a buffer that could only say
	// *one stride, offset zero* would describe it wrongly. What the device laid out is what is
	// carried.
	Fd descriptor{ ::dup(2) };
	GYRO_REQUIRE(descriptor.IsValid());

	const RawFd raw = descriptor.Borrow();

	// Something that has to outlive the description, standing in for the `VkImage` and its memory. The
	// flag is what the test reads afterwards: a backing that is not destroyed with the buffer is the
	// leak this type exists to prevent.
	class Owner final : public IDmabufBacking
	{
	public:
		explicit Owner(Fd descriptor, bool& destroyed) noexcept
			: m_Descriptor{ std::move(descriptor) }, m_Destroyed{ &destroyed }
		{}

		~Owner() override { *m_Destroyed = true; }

	private:
		Fd m_Descriptor;
		bool* m_Destroyed;
	};

	bool destroyed = false;

	DmabufImage image{};
	image.PlaneCount = 2;
	image.Planes[0] = DmabufPlane{ .Descriptor = raw, .Offset = 0, .Stride = 512 };
	image.Planes[1] = DmabufPlane{ .Descriptor = raw, .Offset = 65536, .Stride = 128 };

	{
		const DmabufBuffer buffer{ RenderTarget{ .Size = PixelSize<DeviceSpace>{ 128, 128 },
			                                     .Format = PixelFormat{ FormatXrgb8888, 0, 0x0100000000000001ULL },
			                                     .Memory = image },
			                       std::make_unique<Owner>(std::move(descriptor), destroyed) };

		GYRO_REQUIRE(buffer.IsValid());

		const RenderTarget described = buffer.Describe();
		const DmabufImage* const planes = described.AsDmabuf();

		GYRO_REQUIRE(planes != nullptr);
		GYRO_CHECK_EQ(planes->PlaneCount, std::uint32_t{ 2 });
		GYRO_CHECK_EQ(planes->Planes[1].Offset, std::uint32_t{ 65536 });

		// The convenience accessors mean the *pixels*, which is the first plane and not an average of
		// them.
		GYRO_CHECK_EQ(buffer.Stride(), std::uint32_t{ 512 });
		GYRO_CHECK_EQ(buffer.Descriptor().Value, raw.Value);

		// No mapping came with it, so the blitter's face of it describes nothing rather than a null
		// pointer with a length on it.
		GYRO_CHECK(!buffer.DescribeMapped().IsValid());

		GYRO_CHECK(!destroyed);
	}

	GYRO_CHECK(destroyed);
}
