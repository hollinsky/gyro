#include "Virtual/Buffer.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "Core/Fd.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The ownership rules, and the description the seam sees. Nothing here allocates a real dmabuf —
// Udmabuf.Test.cpp is where the kernel is involved — so these are about the type rather than the
// mechanism, which is the split that keeps a machine with no `/dev/udmabuf` able to check the part
// that is pure C++.

GYRO_TEST(Buffer, DefaultDescribesNothing)
{
	const DmabufBuffer buffer;

	GYRO_CHECK(!buffer.IsValid());
	GYRO_CHECK(!buffer.IsMapped());
	GYRO_CHECK(buffer.Pixels().empty());

	// The seam's own validity check is the one that matters, because it is what a presenter's target
	// set is filtered by and what a renderer refuses on.
	GYRO_CHECK(!buffer.Describe().IsValid());
}

GYRO_TEST(Buffer, DescribeCarriesOnePlane)
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

GYRO_TEST(Buffer, MoveLeavesTheSourceEmpty)
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

GYRO_TEST(Buffer, MappingUnmapsAndIsMoveOnly)
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

GYRO_TEST(Buffer, CpuReadOnAnUnmappedBufferIsEmptyRatherThanUnsafe)
{
	const DmabufBuffer buffer;
	const DmabufBuffer::CpuRead read{ buffer };

	// An allocator that could not map hands back a buffer with no mapping, and a consumer that asks
	// to look must get nothing rather than a null span with a length on it.
	GYRO_CHECK(read.Bytes().empty());
}

GYRO_TEST(Buffer, FormatsAsWhatItIs)
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
