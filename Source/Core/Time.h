#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <ratio>
#include <type_traits>

// The timebase.
//
// Every timestamp in the process is monotonic nanoseconds, converted once at the boundary that
// produced it and never observed in any other domain. The type split is what enforces that: a
// Duration is domain-free, but an Instant is a point in *this* timebase, and there is no way to
// spell one from another epoch without going through a named conversion below.
//
// See Docs/Decisions.md decision 57 and Docs/Architecture.md#the-timebase.

// A clock tag, not a Cpp17Clock. It deliberately has no now(): the only ways to obtain an Instant
// are IClock::Now() and the ingest conversions below, which is what keeps a global "now" from
// being reachable at all rather than merely discouraged. The lowercase names are <chrono>'s
// requirement, not ours.
struct Timebase
{
	using rep = std::int64_t;
	using period = std::nano;
	using duration = std::chrono::duration<rep, period>;
	using time_point = std::chrono::time_point<Timebase>;

	static constexpr bool is_steady = true;
};

// Signed, because a deadline subtraction legitimately goes negative. An alias rather than a strong
// type because a duration has no epoch to be wrong about, and because that keeps <chrono>'s
// literals (16ms, 500us) and duration_cast working unchanged.
using Duration = Timebase::duration;

// Instant + Instant is ill-formed, Instant - Instant is a Duration, and construction from a
// Duration is explicit — so every entry into the domain is a visible conversion.
using Instant = Timebase::time_point;

// The ingest surface, and the only one. Everything gyro reads timestamps from chose
// CLOCK_MONOTONIC already: libinput reports microseconds, DRM page flips report nanoseconds under
// DRM_CAP_TIMESTAMP_MONOTONIC, io_uring times out against it by default. Each function names the
// domain the caller is asserting, which is the assertion worth being able to find later — a
// foreign domain reaching one of these is a bug at the call site, and `Monotonic::` is where you
// grep for every place that judgement was made.
struct Monotonic
{
	[[nodiscard]] static constexpr Instant FromNanoseconds(std::int64_t nanoseconds) noexcept
	{
		return Instant{ Duration{ nanoseconds } };
	}

	[[nodiscard]] static constexpr Instant FromMicroseconds(std::int64_t microseconds) noexcept
	{
		return Instant{ std::chrono::microseconds{ microseconds } };
	}

	// The one way back out, and it exists for two callers: arming an absolute kernel timeout, and
	// stamping a record in a trace file.
	//
	// io_uring's IORING_TIMEOUT_ABS wants a timespec in the same domain an Instant already counts, so
	// this is FromNanoseconds run backwards rather than a new conversion — which is why it is named
	// here beside its inverse rather than with the ToSeconds egress below. The distinction matters:
	// ToSeconds is for a human and may lose precision, and this may not, because a rounded deadline is
	// a frame served at the wrong vblank.
	//
	// **It is not the escape hatch for arithmetic.** Elapsed and Advanced are the total forms and this
	// is not a cheaper way to reach them; a caller doing sums on the result is the hand-rolled timebase
	// CheckClockDiscipline.cmake exists to stop, one indirection further out. What is sanctioned is
	// handing the count straight to something outside the process that takes one — io_uring's
	// IORING_TIMEOUT_ABS, and Trace/Perfetto.cpp's packet timestamps, which are the same nanoseconds in
	// the same domain being read by somebody else's tool rather than by gyro.
	[[nodiscard]] static constexpr std::int64_t ToNanoseconds(Instant instant) noexcept
	{
		return instant.time_since_epoch().count();
	}
};

// Configuration and the motion catalog author in seconds, so this is the one sanctioned
// double -> Duration conversion and it is total. NaN and out-of-range clamp rather than invoking
// the undefined behaviour a bare cast would: a system-layer compositor does not fail to start, or
// schedule a frame into the deep past, because someone fat-fingered a response time. Callers still
// validate; this decides what happens when they do not.
[[nodiscard]] constexpr Duration DurationFromSeconds(double seconds) noexcept
{
	// Self-comparison rather than std::isnan, which is not dependably constexpr across libraries.
	if (seconds != seconds)
	{
		return Duration::zero();
	}

	// The int64 bounds are not exactly representable as doubles, so the guard is the nearest power
	// of ten strictly inside the range. Infinities land here too.
	constexpr double Limit = 9.0e18;
	const double nanoseconds = seconds * 1'000'000'000.0;

	if (nanoseconds >= Limit)
	{
		return Duration::max();
	}
	if (nanoseconds <= -Limit)
	{
		return Duration::min();
	}

	return Duration{ static_cast<std::int64_t>(nanoseconds) };
}

// Mode tables and VRR ranges quote hertz; everything downstream wants the period. A non-positive
// or NaN rate yields Duration::max() rather than zero, because an output that never owes a frame
// is recoverable and one that owes infinitely many is not.
[[nodiscard]] constexpr Duration PeriodFromHertz(double hertz) noexcept
{
	if (!(hertz > 0.0))
	{
		return Duration::max();
	}

	return DurationFromSeconds(1.0 / hertz);
}

// Egress, for logs and human-facing values only. Scheduling arithmetic stays in integer
// nanoseconds — decision 29's processor-demand test is exact because its floors are integer
// floors, and a double anywhere in that path turns an exact answer into a discretized one.
[[nodiscard]] constexpr double ToSeconds(Duration duration) noexcept
{
	return static_cast<double>(duration.count()) / 1'000'000'000.0;
}

[[nodiscard]] constexpr double ToMilliseconds(Duration duration) noexcept
{
	return static_cast<double>(duration.count()) / 1'000'000.0;
}

// The timebase's own arithmetic, for the pairs whose separation the caller does not bound.
//
// std::chrono's operators are the half of this domain that is not total: Instant - Instant and
// Instant + Duration are undefined on overflow, and an alias cannot replace an operator. So the total
// forms are named here, in the file that owns the domain, rather than hand-rolled beside whichever
// caller needed one first.
//
// This is the same split the conversions above already draw, one level down. ToSeconds is egress and
// trusts what it is handed; DurationFromSeconds is ingress and is total. The bare operators are the
// egress half of the arithmetic and stay the ordinary spelling — a frame loop subtracting a deadline
// from a now it produced microseconds ago is bounded by construction, and Time.Test.cpp's
// DeadlineArithmeticGoesNegative is that case. These two are the ingress half, for a pair that
// arrived from somewhere: an event timestamp against a predicted presentation time, or an authored
// duration added to either.
//
// Neither bound is reachable in this process. An Instant counts nanoseconds from boot and a Duration
// spans some 292 years, so every pair gyro constructs sits far inside — which is exactly the argument
// DurationFromSeconds declines to rest on, for the reason it gives: callers still validate, and these
// decide what happens when they do not. A saturated answer is one the caller can compare and clamp;
// an overflowed one is a value the optimizer is entitled to assume cannot exist.
[[nodiscard]] constexpr Duration Elapsed(Instant origin, Instant now) noexcept
{
	const std::int64_t from = origin.time_since_epoch().count();
	const std::int64_t to = now.time_since_epoch().count();

	// Unsigned, so the difference of any two counts is exact before it is clamped rather than
	// undefined on the way to being rejected. The ordering test is what picks the branch, which is why
	// it comes first and why neither branch can wrap: each subtracts the smaller from the larger.
	if (to >= from)
	{
		const std::uint64_t forward = static_cast<std::uint64_t>(to) - static_cast<std::uint64_t>(from);

		if (forward > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
		{
			return Duration::max();
		}

		return Duration{ static_cast<std::int64_t>(forward) };
	}

	const std::uint64_t backward = static_cast<std::uint64_t>(from) - static_cast<std::uint64_t>(to);

	// The negative range holds one more value than the positive one, and Duration::min() is that
	// value — so the boundary is the magnitude itself rather than one past it, and negating anything
	// smaller stays in range.
	if (backward >= std::uint64_t{ 1 } << 63)
	{
		return Duration::min();
	}

	return Duration{ -static_cast<std::int64_t>(backward) };
}

// Instants saturate rather than wrapping. A spring that never settles reports an instant no frame
// will reach, and the alternative is a settle time in the deep past — the same failure the frame
// clock's Invalidate() exists to prevent, arriving from the animation side instead.
//
// Both directions, because a horizon or a response time is authored and an authored number has no
// sign discipline. Guarding only the direction the first caller happened to need is how the other one
// becomes somebody's afternoon.
[[nodiscard]] constexpr Instant Advanced(Instant origin, Duration after) noexcept
{
	const std::int64_t base = origin.time_since_epoch().count();
	const std::int64_t offset = after.count();

	if (offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset)
	{
		return Instant{ Duration::max() };
	}

	if (offset < 0 && base < std::numeric_limits<std::int64_t>::min() - offset)
	{
		return Instant{ Duration::min() };
	}

	return origin + after;
}

// The standard supplies formatters only for the calendar clocks, and a time_point over a private
// tag is not one of them. Prints time since the timebase's epoch, which is boot, and which is the
// only reading of an Instant that means anything on its own.
//
// The context is a template parameter rather than std::format_context, and it has to be. The
// std::formattable concept instantiates a formatter against an unspecified context type that is not
// required to be, and in libstdc++ is not, the one std::format uses — so naming the concrete type
// leaves std::format working while the concept reports false. Nothing here would notice; the test
// harness would, by printing <unprintable> in place of the two instants that failed to match.
template<>
struct std::formatter<Instant> : std::formatter<Duration>
{
	template<typename Context>
	auto format(Instant instant, Context& context) const
	{
		return std::formatter<Duration>::format(instant.time_since_epoch(), context);
	}
};

namespace Detail
{
// Only to give the assertion below a dependent context: a requires-expression outside a template
// is checked eagerly, so the ill-formed case is a hard error rather than a false result.
template<typename T>
concept Addable = requires(T a, T b) { a + b; };
} // namespace Detail

// The contract everything downstream assumes. The runtime half waits on the test harness.
static_assert(sizeof(Instant) == sizeof(std::int64_t) && sizeof(Duration) == sizeof(std::int64_t));
static_assert(std::is_trivially_copyable_v<Instant> && std::is_trivially_copyable_v<Duration>);
static_assert(std::formattable<Instant, char>, "A report prints the instant rather than <unprintable>");
static_assert(std::formattable<Duration, char>);
static_assert(!Detail::Addable<Instant>, "Two points in time do not add");
static_assert(Detail::Addable<Duration>);
static_assert(!std::is_convertible_v<Duration, Instant>, "Entering the domain must be explicit");
static_assert(std::is_same_v<decltype(Instant{} - Instant{}), Duration>);
static_assert(std::is_same_v<decltype(Instant{} + Duration{}), Instant>);
static_assert(Instant{} + Duration{ 5 } - Instant{} == Duration{ 5 });
static_assert(Monotonic::FromMicroseconds(1) == Monotonic::FromNanoseconds(1'000));
static_assert(Monotonic::ToNanoseconds(Monotonic::FromNanoseconds(1'234)) == 1'234);
static_assert(Monotonic::ToNanoseconds(Monotonic::FromMicroseconds(2)) == 2'000);
static_assert(Monotonic::ToNanoseconds(Instant{}) == 0);

static_assert(DurationFromSeconds(1.0) == std::chrono::seconds{ 1 });
static_assert(DurationFromSeconds(-0.5) == std::chrono::milliseconds{ -500 });
static_assert(DurationFromSeconds(std::numeric_limits<double>::quiet_NaN()) == Duration::zero());
static_assert(DurationFromSeconds(std::numeric_limits<double>::infinity()) == Duration::max());
static_assert(DurationFromSeconds(-std::numeric_limits<double>::infinity()) == Duration::min());
static_assert(PeriodFromHertz(2.0) == std::chrono::milliseconds{ 500 });
static_assert(PeriodFromHertz(0.0) == Duration::max());
static_assert(PeriodFromHertz(std::numeric_limits<double>::quiet_NaN()) == Duration::max());

static_assert(Elapsed(Monotonic::FromNanoseconds(100), Monotonic::FromNanoseconds(400)) == Duration{ 300 });
static_assert(Elapsed(Monotonic::FromNanoseconds(400), Monotonic::FromNanoseconds(100)) == Duration{ -300 });
static_assert(
	Elapsed(Monotonic::FromNanoseconds(std::numeric_limits<std::int64_t>::min()), Instant{ Duration::max() }) ==
		Duration::max(),
	"The widest pair the type can hold saturates rather than overflowing"
);
static_assert(
	Elapsed(Instant{ Duration::max() }, Monotonic::FromNanoseconds(std::numeric_limits<std::int64_t>::min())) ==
		Duration::min(),
	"and does so in both directions"
);
static_assert(Elapsed(Instant{ Duration::max() }, Instant{ Duration::max() }) == Duration::zero());

static_assert(Advanced(Monotonic::FromNanoseconds(100), Duration{ 300 }) == Monotonic::FromNanoseconds(400));
static_assert(Advanced(Monotonic::FromNanoseconds(400), Duration{ -300 }) == Monotonic::FromNanoseconds(100));
static_assert(
	Advanced(Monotonic::FromNanoseconds(1'000), Duration::max()) == Instant{ Duration::max() },
	"An unreachable offset lands on an unreachable instant, not in the deep past"
);
static_assert(
	Advanced(Monotonic::FromNanoseconds(-1'000), Duration::min()) == Instant{ Duration::min() },
	"and the sign the first caller did not need is guarded too"
);
static_assert(Advanced(Instant{}, Duration::zero()) == Instant{});
