#pragma once

#include <format>
#include <type_traits>
#include <utility>

// A file descriptor, in the two forms the design actually needs: one that owns and one that names.
//
// POSIX rather than Linux, which is what keeps it in the portable tier — CMake/CheckPortability.cmake
// records the same distinction for Core/Clock.cpp's clock_gettime. The close() itself is in Fd.cpp so
// that including this does not drag <unistd.h> into every translation unit that merely holds one.
//
// **Why there are two types, and it is not a style preference.** A descriptor is opened by one party
// and named by several: Seam/EventSource.h's `Descriptor()` hands the number to whoever does the
// waiting while the source keeps the file, and a `DmabufBuffer` plane and a `SyncPoint` are both a
// descriptor somebody else owns. Those cannot be the same type as the one an open returns, and the
// general rule that settles it is worth stating once, because it will come up again wherever a
// signal is added:
//
//   **A broadcast can never transfer ownership — to anyone.** If N observers each receive a
//   notification, at most one of them could take the resource, and nothing in the signature says
//   which. So what a signal carries is a *fact*, never a resource.
//
// RawFd is that fact. The receiver already holds the Fd it opened, so matching is a comparison with
// no bookkeeping on either side.
//
// **Rejected: one type, with signals and accessors handing out `const Fd&`.** Fewer names, and it
// trades a naming distinction anyone can learn for a lifetime one nobody can see. A broadcast is
// precisely the context where an observer caches what it is handed, and a const reference to
// something the source still owns gives no hint that outliving the call is wrong.
//
// **Rejected: RawFd named WeakFd.** `weak` in C++ means there is an upgrade to attempt and an expiry
// to observe — weak_ptr::lock is the whole content of the word. There is neither here: the number is
// valid or it is not, and only the owner knows which. The name would promise a check that does not
// exist.
//
// **Retired: device pause and resume identified by something that is not a descriptor at all.**
// A logind-shaped session would hand back a *new* descriptor on resume, which would make the old
// number wrong as identity across the cycle — so the concern was real for as long as a session
// interface was expected. Decision 145 removed it: device access is a udev rule and master comes
// from first-open, so nothing revokes a descriptor and there is no cycle to be identified across.

// The value a closed or never-opened descriptor carries. Named rather than spelled -1 at each site,
// since the two places that compare against it are the ones a reader most wants to be sure of.
inline constexpr int InvalidFd = -1;

// A descriptor that names without owning. Trivially copyable, so it crosses a signal; a distinct
// type, so it does not silently become an int and get closed by something that does not own it.
struct RawFd
{
	int Value = InvalidFd;

	[[nodiscard]] constexpr bool IsValid() const noexcept { return Value >= 0; }

	// Deliberately no operator bool. A descriptor of 0 is a perfectly ordinary descriptor, and a
	// conversion whose obvious reading is "nonzero" would be wrong for exactly it.

	friend constexpr bool operator==(RawFd, RawFd) noexcept = default;
};

// A descriptor that owns. Move-only, closes on destruction, and never copies — a copied owner is two
// owners and the second close lands on whatever the number was reused for, which is a bug that
// reproduces as unrelated I/O failing somewhere else entirely.
class Fd
{
public:
	Fd() = default;

	// Explicit, because the whole content of this type is that constructing one is a claim of
	// ownership. An implicit conversion would let a borrowed number become an owner in a function
	// argument, which is the exact accident the type exists to prevent.
	explicit Fd(int descriptor) noexcept : m_Descriptor{ descriptor } {}

	~Fd() { Reset(); }

	Fd(const Fd&) = delete;
	Fd& operator=(const Fd&) = delete;

	Fd(Fd&& other) noexcept : m_Descriptor{ std::exchange(other.m_Descriptor, InvalidFd) } {}

	Fd& operator=(Fd&& other) noexcept
	{
		// Self-assignment first, and it is not paranoia here: without it the Reset() below closes the
		// descriptor that the exchange is about to hand back, and the object ends up owning a number
		// that has already been returned to the kernel.
		if (this != &other)
		{
			Reset(std::exchange(other.m_Descriptor, InvalidFd));
		}

		return *this;
	}

	[[nodiscard]] constexpr bool IsValid() const noexcept { return m_Descriptor >= 0; }

	// The number, for handing to a syscall. Named Get rather than offered as a conversion for the
	// reason the constructor is explicit, and read-only in the sense that matters: the caller may
	// pass it to ioctl, and may not close it.
	[[nodiscard]] constexpr int Get() const noexcept { return m_Descriptor; }

	// The non-owning form, for a signal or anything else that identifies rather than holds. Explicit
	// and named, so that the point at which ownership stops travelling is a token in the source.
	[[nodiscard]] constexpr RawFd Borrow() const noexcept { return RawFd{ m_Descriptor }; }

	// Ownership out, without a close. This is how a descriptor reaches something that takes an int
	// and owns it — the service manager's fd store on the restart path of decision 49 is the caller
	// this exists for.
	[[nodiscard]] constexpr int Release() noexcept { return std::exchange(m_Descriptor, InvalidFd); }

	// Closes what is held and takes what is given. Defined in Fd.cpp; see there for why a failing
	// close is not reported.
	void Reset(int descriptor = InvalidFd) noexcept;

private:
	int m_Descriptor = InvalidFd;
};

// A second owner of the same open file, for the handover that is a copy rather than a move.
//
// **It exists because two APIs in this tree take ownership of what they are handed while the caller
// still needs it.** `Wire::MessageWriter::PutFd` is one — a marshalled request may sit unsent for as
// long as the host is slow, so a caller that closed its copy on return would have handed over a
// number the kernel has already given to something else — and `zwp_linux_buffer_params_v1.add`
// reaches it once per plane per target. Written here rather than at each call site so that the
// `dup` and the `<unistd.h>` it needs live in the one file that already owns both.
//
// An invalid descriptor duplicates to an invalid one rather than to an error, because the caller's
// next question is `IsValid` either way. Defined in Fd.cpp.
[[nodiscard]] Fd Duplicate(RawFd descriptor) noexcept;

// Prints as fd 7, or fd none. No format spec is accepted.
//
// The context is a template parameter for the reason recorded at length in Core/Time.h: naming
// std::format_context leaves std::format working and std::formattable false, so the value goes
// missing from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<RawFd>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(RawFd descriptor, Context& context) const
	{
		if (!descriptor.IsValid())
		{
			return std::format_to(context.out(), "fd none");
		}

		return std::format_to(context.out(), "fd {}", descriptor.Value);
	}
};

// An owner prints as what it names. Two formatters rather than one on a common base, because there is
// no common base and inventing one to share four lines would put a conversion between them.
template<>
struct std::formatter<Fd>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const Fd& descriptor, Context& context) const
	{
		return std::formatter<RawFd>{}.format(descriptor.Borrow(), context);
	}
};

// The contract everything downstream assumes.
static_assert(
	std::is_trivially_copyable_v<RawFd> && std::is_standard_layout_v<RawFd>,
	"A borrowed descriptor is broadcast by value"
);
static_assert(!std::is_copy_constructible_v<Fd>, "An owner that copied would be two owners");
static_assert(std::is_nothrow_move_constructible_v<Fd> && std::is_nothrow_move_assignable_v<Fd>);
static_assert(!std::is_convertible_v<int, Fd>, "Ownership is claimed explicitly or not at all");

static_assert(std::formattable<RawFd, char> && std::formattable<Fd, char>);

static_assert(!RawFd{}.IsValid(), "A default-constructed descriptor names nothing");
static_assert(RawFd{ 0 }.IsValid(), "Descriptor 0 is a descriptor");
static_assert(RawFd{ 7 } == RawFd{ 7 } && RawFd{ 7 } != RawFd{ 8 });
