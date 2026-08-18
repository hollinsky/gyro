#pragma once

#include <cerrno>
#include <expected>
#include <format>
#include <string_view>
#include <system_error>
#include <type_traits>

// What a fallible operation returns, and what it says when it fails.
//
// Every current caller is a syscall wrapper — Docs/Architecture.md's ISession::OpenDevice is the
// first — so the error domain is errno and nothing else. A gyro-wide error enum spanning capacity
// refusals, protocol violations, and decision 27's kill-the-client cases is a real thing to want
// later and is deliberately not invented here: it would be categories designed in advance of the
// callers that justify them, which is how a domain ends up with an Other.
//
// **The context is required, and that is the whole reason this is not std::expected<T, std::errc>.**
// EACCES on its own names a permission the reader then has to guess at; "opening /dev/dri/card0"
// beside it names the operation, and the pair is what a log line needs to be actionable. Requiring
// it at the construction site rather than trusting the eventual log statement to add it is the
// difference between the information existing where it was known and existing where someone
// remembered to put it.
//
// It is a string_view over a literal rather than an owned string, which is what keeps an Error
// trivially copyable and allocation-free. The obligation that comes with that: **the text must have
// static storage.** A context built with std::format and passed in dangles, and the type cannot
// stop it — the consteval alternative that could would forbid the one legitimate variable case,
// which is a literal chosen from a small fixed set.

class Error
{
public:
	// No default constructor. A default-constructed error is a failure with nothing to say, and
	// every way of producing one is a bug that this refuses to give a spelling to.
	constexpr Error(int code, std::string_view context) noexcept : m_Context{ context }, m_Code{ code } {}

	// Reads errno at the call site, which is the only place it is still the errno for the call that
	// failed. Anything between the syscall and here — a destructor, a log statement, an allocation —
	// is entitled to overwrite it.
	[[nodiscard]] static Error FromErrno(std::string_view context) noexcept { return Error{ errno, context }; }

	[[nodiscard]] constexpr int Code() const noexcept { return m_Code; }

	[[nodiscard]] constexpr std::string_view Context() const noexcept { return m_Context; }

	// Compares the code alone. Two failures of the same call at two sites are the same failure to
	// anything that branches on one, and a comparison that read the context would make a test assert
	// on prose. The context is for the reader.
	[[nodiscard]] friend constexpr bool operator==(const Error& left, const Error& right) noexcept
	{
		return left.m_Code == right.m_Code;
	}

private:
	std::string_view m_Context;
	int m_Code;
};

// The alias, rather than a class of its own. std::expected already has the shape — the monadic
// operations, the value-or, the void specialisation ISession::CloseDevice's neighbours will want —
// and a wrapper around it would exist to rename things.
//
// Note that it is [[nodiscard]] by virtue of std::expected being so: a Result dropped on the floor
// is a diagnostic, which is the property that makes returning one different from returning a bool.
template<typename T>
using Result = std::expected<T, Error>;

// Spelled here so that a failure return is one name rather than two. `return Failure(EACCES, "...")`
// and `return FailFromErrno("...")` read as what they are at the call site, where
// `std::unexpected{ Error::FromErrno("...") }` reads as a type conversion.
[[nodiscard]] constexpr std::unexpected<Error> Failure(int code, std::string_view context) noexcept
{
	return std::unexpected{ Error{ code, context } };
}

[[nodiscard]] inline std::unexpected<Error> FailFromErrno(std::string_view context) noexcept
{
	return std::unexpected{ Error::FromErrno(context) };
}

// Prints as `opening /dev/dri/card0: Permission denied (13)`. No format spec is accepted.
//
// **Formatting an Error allocates**, because std::system_category().message() returns a std::string,
// and that is correct rather than a defect to work around: this runs on a log path and never on the
// frame path, where a failure is reported by a return value that has not been formatted. The
// alternative is strerror_r, whose GNU and XSI variants differ in return type, in exchange for
// avoiding an allocation nobody was going to make.
//
// The context is a template parameter for the reason recorded at length in Core/Time.h: naming
// std::format_context leaves std::format working and std::formattable false, so the value goes
// missing from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<Error>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const Error& error, Context& context) const
	{
		return std::format_to(
			context.out(), "{}: {} ({})", error.Context(), std::system_category().message(error.Code()), error.Code()
		);
	}
};

// The contract everything downstream assumes.
static_assert(std::is_trivially_copyable_v<Error>, "An error is copied out of a Result, never owned behind one");
static_assert(std::formattable<Error, char>, "A report prints the error rather than <unprintable>");

static_assert(Error{ 13, "opening the render node" }.Code() == 13);
static_assert(
	Error{ 13, "opening the render node" } == Error{ 13, "probing the connector" },
	"Equality is the code alone; the context is for the reader"
);
static_assert(Error{ 13, "" } != Error{ 2, "" });
