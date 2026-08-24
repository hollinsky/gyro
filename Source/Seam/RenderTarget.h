#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "Core/Fd.h"
#include "Geometry/Space.h"

// An image the presenter owns and something else writes into.
//
// **The backend owns the images, and that is the rule the whole seam hangs off.** Both KMS and
// Wayland have modifier and device constraints the renderer cannot know, so neither can accept a
// foreign buffer; the presenter allocates or imports, and hands out descriptions. A RenderTarget is
// that description — everything a writer needs to bind memory it did not allocate, and nothing about
// how it will be scanned out.
//
// **Backend-allocated and imported are one concept**, per Docs/Architecture.md#what-to-build-before-
// it-is-needed. A virtual output renders into dmabufs a client owns and a local output renders into
// dmabufs gyro allocated; only the source of the constraint differs, and building the second as a
// special case bolted onto the first is what makes remote desktop a fork instead of an output.
//
// **Two memory kinds, because one presenter yields both across its own life.** The console writes
// dumb buffers with CPU blits before Vulkan exists; the same DRM presenter allocates through GBM once
// it does. That is the same output, the same frame clock, the same damage path and the same
// presentation feedback — what changes is the writer, not the seam. So the console is a *renderer*
// rather than a second presentation path, and this type carries the discriminated memory that makes
// the two writers substitutable. See Docs/Decisions.md decision 79.
//
// **The renderer refuses what it cannot import; it never assumes.** A Vulkan device handed a mapped
// image and a blitter handed a dmabuf are both configuration errors from the composition root, and
// the variant below is what makes them a branch somebody had to write rather than a cast that
// happens to work on the machine it was written on.
//
// **Identity is the index into `Targets()`**, valid until `TargetsInvalidated`. A generational handle
// was considered and is not worth it here: the span is the presenter's, the invalidation is
// announced, and the frame loop re-reads the whole set when it fires — so there is no window in which
// a stale index resolves to the wrong image, which is the only thing a handle would buy.

// DRM's numbers, reproduced rather than included. The portable tier may not reach `drm_fourcc.h`
// (CMake/CheckPortability.cmake), and these are stable kernel ABI rather than an implementation
// detail of a header — a format code that changed would break every existing client. The backends
// that do include the real header are where a mismatch would show, and they compare against these.
[[nodiscard]] constexpr std::uint32_t FourCc(char a, char b, char c, char d) noexcept
{
	return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) |
	       (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
}

// `DRM_FORMAT_MOD_LINEAR` and `DRM_FORMAT_MOD_INVALID`. The second is not "no modifier" — it is the
// value that says the modifier is unknown, which is what an implicitly-tiled legacy allocation
// carries and what lavapipe's linear-only import must be distinguished from.
inline constexpr std::uint64_t ModifierLinear = 0;
inline constexpr std::uint64_t ModifierInvalid = 0x00ffffffffffffffULL;

struct PixelFormat
{
	std::uint32_t Code = 0;
	std::uint32_t Reserved = 0;
	std::uint64_t Modifier = ModifierInvalid;

	[[nodiscard]] constexpr bool IsValid() const noexcept { return Code != 0; }

	friend constexpr bool operator==(PixelFormat, PixelFormat) noexcept = default;
};

// The handful the floor tier and the console actually name. A catalog belongs to the backends, which
// negotiate against what the device reports; these exist so that a test, a dump, and the boot splash
// are not each spelling a magic number.
inline constexpr std::uint32_t FormatXrgb8888 = FourCc('X', 'R', '2', '4');
inline constexpr std::uint32_t FormatArgb8888 = FourCc('A', 'R', '2', '4');
inline constexpr std::uint32_t FormatXrgb2101010 = FourCc('X', 'R', '3', '0');
inline constexpr std::uint32_t FormatArgb2101010 = FourCc('A', 'R', '3', '0');
inline constexpr std::uint32_t FormatNv12 = FourCc('N', 'V', '1', '2');

// Four, which is what `DRM_FORMAT_MOD` planar formats reach and what `AddFB2` takes.
inline constexpr std::size_t MaxImagePlanes = 4;

struct DmabufPlane
{
	// Borrowed, for Core/Fd.h's reason: a target description is copied by value and read per frame,
	// and the descriptor belongs to whoever allocated the image — the presenter, or a client that
	// registered a virtual output. It is valid until TargetsInvalidated.
	RawFd Descriptor;
	std::uint32_t Offset = 0;
	std::uint32_t Stride = 0;

	friend constexpr bool operator==(DmabufPlane, DmabufPlane) noexcept = default;
};

// Memory the renderer imports. The only kind gyro renders into: `wl_shm` is refused for virtual
// outputs because a dmabuf's size is fixed at allocation and shm's is not, which is the whole of the
// truncate-and-fault hazard that would otherwise force a SIGBUS handler onto the frame thread.
struct DmabufImage
{
	std::array<DmabufPlane, MaxImagePlanes> Planes{};
	std::uint32_t PlaneCount = 0;

	[[nodiscard]] constexpr std::span<const DmabufPlane> Used() const noexcept { return { Planes.data(), PlaneCount }; }

	friend constexpr bool operator==(DmabufImage, DmabufImage) noexcept = default;
};

// Memory a CPU writes: a KMS dumb buffer, mapped. This is the console's target and the boot splash's,
// and it exists at the seam for the reason above — the presenter that yields it is the same presenter
// that will yield dmabufs a moment later.
struct MappedImage
{
	std::byte* Pixels = nullptr;
	std::uint32_t Stride = 0;
	std::uint32_t Reserved = 0;

	// The mapping's own length, which is not `Stride * Height`: a dumb buffer is page-rounded, and a
	// blitter that computed the bound itself would be computing it from the wrong number.
	std::size_t Length = 0;

	[[nodiscard]] constexpr std::span<std::byte> Bytes() const noexcept { return { Pixels, Length }; }

	friend constexpr bool operator==(MappedImage, MappedImage) noexcept = default;
};

// SPEC: how deep a presenter's ring may be, and therefore how many targets anything holding fixed
// per-target storage has to size for. *(Hoisted to the waist 2026-08-23; see decision 134.)*
//
// **Four, and every party to it had already written the same argument down separately.** It is what a
// KMS driver hands out for a flip queue and what a nested host keeps in flight; it is a ring three deep
// plus the one being scanned. The interesting configurations are two and three, and four is headroom.
//
// **At the waist because it is an agreement rather than a capacity.** A presenter chooses its ring
// depth, a renderer sizes a bind table for whatever it is handed, and the frame loop carries state per
// target — three modules deciding independently, all of them wrong together the moment one picks a
// different number. `Headless`, `Nested` and `Virtual` each capped themselves here, `Blit` sized its
// bind table to "what a DRM presenter binds at its widest", and `Frame` needed one more of the same;
// five copies of one number, one of which described the others by hand.
//
// A renderer may deliberately bind *more* than this — `Render` allows a doubling so that a presenter
// growing a cursor plane's target does not meet its table first — which is why this is the presenter's
// ceiling and not a renderer's.
inline constexpr std::uint32_t MaxTargets = 4;

struct RenderTarget
{
	PixelSize<DeviceSpace> Size{};
	PixelFormat Format{};
	std::variant<DmabufImage, MappedImage> Memory{};

	// No color state here, deliberately. What the output is scanned out as is stated once, in
	// OutputConfiguration, and a copy on every target is a second place for it to be wrong across a
	// reconfiguration that changed one and not the other.

	[[nodiscard]] const DmabufImage* AsDmabuf() const noexcept { return std::get_if<DmabufImage>(&Memory); }

	[[nodiscard]] const MappedImage* AsMapped() const noexcept { return std::get_if<MappedImage>(&Memory); }

	[[nodiscard]] bool IsMapped() const noexcept { return std::holds_alternative<MappedImage>(Memory); }

	// A target is describable but not usable until it has an extent and a format. Backends build these
	// in place and a half-filled one is worth catching where it is consumed rather than where a plane
	// count of zero eventually produces a black screen.
	[[nodiscard]] bool IsValid() const noexcept
	{
		if (Size.IsEmpty() || !Format.IsValid())
		{
			return false;
		}

		if (const DmabufImage* image = AsDmabuf())
		{
			return image->PlaneCount > 0 && image->PlaneCount <= MaxImagePlanes &&
			       image->Planes[0].Descriptor.IsValid();
		}

		if (const MappedImage* image = AsMapped())
		{
			return image->Pixels != nullptr && image->Length > 0;
		}

		return false;
	}

	friend bool operator==(const RenderTarget&, const RenderTarget&) = default;
};

// Prints as XR24 mod 0x0 and XR24 mod invalid, which is how both appear in a modifier negotiation log.
template<>
struct std::formatter<PixelFormat>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(PixelFormat format, Context& context) const
	{
		const char code[4] = {
			static_cast<char>(format.Code & 0xff),
			static_cast<char>((format.Code >> 8) & 0xff),
			static_cast<char>((format.Code >> 16) & 0xff),
			static_cast<char>((format.Code >> 24) & 0xff),
		};

		auto out = std::format_to(context.out(), "{}", std::string_view{ code, format.IsValid() ? 4U : 0U });

		if (!format.IsValid())
		{
			out = std::format_to(out, "none");
		}

		if (format.Modifier == ModifierInvalid)
		{
			return std::format_to(out, " mod invalid");
		}

		return std::format_to(out, " mod {:#x}", format.Modifier);
	}
};

// Prints as device(1920x1080) XR24 mod 0x0 dmabuf x1, or ... mapped 8294400B.
template<>
struct std::formatter<RenderTarget>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const RenderTarget& target, Context& context) const
	{
		auto out = std::format_to(context.out(), "{} {}", target.Size, target.Format);

		if (const DmabufImage* image = target.AsDmabuf())
		{
			return std::format_to(out, " dmabuf x{}", image->PlaneCount);
		}

		if (const MappedImage* image = target.AsMapped())
		{
			return std::format_to(out, " mapped {}B", image->Length);
		}

		return out;
	}
};

// The contract everything downstream assumes.
static_assert(std::is_trivially_copyable_v<PixelFormat> && std::is_standard_layout_v<PixelFormat>);
static_assert(sizeof(PixelFormat) == 16, "No padding to leave uninitialised");
static_assert(std::is_trivially_copyable_v<DmabufImage> && std::is_trivially_copyable_v<MappedImage>);
static_assert(std::is_trivially_copyable_v<RenderTarget>, "A target description is copied, never owned");
static_assert(std::formattable<PixelFormat, char> && std::formattable<RenderTarget, char>);

// The four-character codes, against the kernel's own values. Written as the hex a driver log prints,
// so that a mismatch shows up here rather than as a rejected AddFB2 on a machine with a monitor.
static_assert(FormatXrgb8888 == 0x34325258);
static_assert(FormatArgb8888 == 0x34325241);
static_assert(FormatXrgb2101010 == 0x30335258);
static_assert(FormatNv12 == 0x3231564e);
static_assert(ModifierInvalid != ModifierLinear, "Unknown is not linear, and importing one as the other is a defect");

// The default is unusable and says so, which is what keeps a half-built description out of a commit.
static_assert(!PixelFormat{}.IsValid());
static_assert(PixelFormat{ FormatXrgb8888, 0, ModifierLinear }.IsValid());
static_assert(
	PixelFormat{ FormatXrgb8888, 0, ModifierLinear } != PixelFormat{ FormatXrgb8888, 0, ModifierInvalid },
	"Two modifiers are two formats: a buffer imports under one and not the other"
);
