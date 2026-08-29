#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

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
// It is a pointer to a literal rather than an owned string, which is what keeps an Error trivially
// copyable and allocation-free. The obligation that comes with that — **the sentence must have static
// storage** — is carried by the type rather than by a paragraph, because it went unmet for as long as
// it was only written down: `Failure(errno, std::format("opening {}", path))` compiled, dangled, and
// printed freed heap on the one log line standing between a black screen and a diagnosis.
//
// **A `const char*` rather than a `string_view`, which is the second obligation the first one hid.**
// A view over a literal is NUL-terminated in fact and not by type, so `Failure(code, sentence.substr(
// 4))` was a spelling the compiler allowed — and the reader that would find out is Trace/Recorder.h,
// on the writer thread, `strlen`ing a pointer a frame put in the ring some seconds ago. Taking the
// pointer makes both obligations one thing the caller cannot get wrong, and it retires the deleted
// std::string overloads along with them: a std::string does not convert to a `const char*` at all, so
// the rule no longer needs a trick to state it.
//
// **The varying part is a Subject instead**, which owns its bytes. The sentence stays a literal and
// the value beside it is copied, so there is nothing left to outlive: a device path, a connector
// name, a fourcc and its modifier. That costs a fixed buffer on every Error, which lands on failure
// returns and never in a loop that runs per frame.

// The short token a sentence is about. Thirty-one characters because the longest real subject is a
// format and its modifier — `XR24 mod 0x100000000000002` — and a site naming two values wants room
// for both. Truncation is marked rather than silent: a subject that did not fit is still a subject
// the reader can recognise, and the trailing `~` says what happened to the rest.
class Subject
{
public:
	static constexpr std::size_t Capacity = 31;

	constexpr Subject() noexcept = default;

	// Copies, so a std::string temporary is safe here in exactly the way it is not for the sentence.
	constexpr Subject(std::string_view text) noexcept
	{
		const std::size_t taken = text.size() > Capacity ? Capacity - 1 : text.size();

		for (std::size_t index = 0; index < taken; ++index)
		{
			m_Text[index] = text[index];
		}

		if (taken < text.size())
		{
			m_Text[taken] = '~';
			m_Length = static_cast<std::uint8_t>(taken + 1);

			return;
		}

		m_Length = static_cast<std::uint8_t>(taken);
	}

	// For a subject that is two values or a formattable one. std::format_to_n writes into the buffer
	// and allocates nothing, which is the whole point of spelling it here rather than at the call site.
	template<typename... Args>
	[[nodiscard]] static Subject Of(std::format_string<Args...> pattern, Args&&... arguments)
	{
		// One larger than the buffer keeps, so that a subject which did not fit arrives here as a view
		// longer than Capacity and picks up the truncation marker the copying constructor writes.
		char scratch[Capacity + 1];

		const auto written = std::format_to_n(scratch, Capacity + 1, pattern, std::forward<Args>(arguments)...);

		return Subject{ std::string_view{ scratch, static_cast<std::size_t>(written.out - scratch) } };
	}

	[[nodiscard]] constexpr std::string_view View() const noexcept { return { m_Text, m_Length }; }

	[[nodiscard]] constexpr bool IsEmpty() const noexcept { return m_Length == 0; }

private:
	char m_Text[Capacity]{};
	std::uint8_t m_Length = 0;
};

class Error
{
public:
	// No default constructor. A default-constructed error is a failure with nothing to say, and
	// every way of producing one is a bug that this refuses to give a spelling to.
	constexpr Error(int code, const char* context) noexcept : m_Context{ context }, m_Code{ code } {}

	constexpr Error(int code, const char* context, Subject subject) noexcept
		: m_Context{ context }, m_Subject{ subject }, m_Code{ code }
	{}

	// Reads errno at the call site, which is the only place it is still the errno for the call that
	// failed. Anything between the syscall and here — a destructor, a log statement, an allocation —
	// is entitled to overwrite it.
	[[nodiscard]] static Error FromErrno(const char* context) noexcept { return Error{ errno, context }; }

	[[nodiscard]] static Error FromErrno(const char* context, Subject subject) noexcept
	{
		return Error{ errno, context, subject };
	}

	[[nodiscard]] constexpr int Code() const noexcept { return m_Code; }

	[[nodiscard]] constexpr std::string_view Context() const noexcept { return m_Context; }

	// The same sentence for a caller that needs the pointer rather than the view, which today is the
	// trace ring: Core/Trace.h holds a record's name as a `const char*` and the writer thread reads it
	// long after the frame that recorded it. That is exactly what the storage rule above promises, so
	// it is handed over rather than copied — a refusal names itself on the timeline for the cost of a
	// pointer already in .rodata.
	[[nodiscard]] constexpr const char* Sentence() const noexcept { return m_Context; }

	[[nodiscard]] constexpr Subject About() const noexcept { return m_Subject; }

	// Compares the code alone. Two failures of the same call at two sites are the same failure to
	// anything that branches on one, and a comparison that read the context would make a test assert
	// on prose. The context is for the reader.
	[[nodiscard]] friend constexpr bool operator==(const Error& left, const Error& right) noexcept
	{
		return left.m_Code == right.m_Code;
	}

private:
	const char* m_Context;
	Subject m_Subject;
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
[[nodiscard]] constexpr std::unexpected<Error> Failure(int code, const char* context) noexcept
{
	return std::unexpected{ Error{ code, context } };
}

[[nodiscard]] constexpr std::unexpected<Error> Failure(int code, const char* context, Subject subject) noexcept
{
	return std::unexpected{ Error{ code, context, subject } };
}

[[nodiscard]] inline std::unexpected<Error> FailFromErrno(const char* context) noexcept
{
	return std::unexpected{ Error::FromErrno(context) };
}

[[nodiscard]] inline std::unexpected<Error> FailFromErrno(const char* context, Subject subject) noexcept
{
	return std::unexpected{ Error::FromErrno(context, subject) };
}

// Prints as `opening a DRM node /dev/dri/card0: Permission denied (13)`, and without the subject
// where there is none. No format spec is accepted.
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
		auto out = error.About().IsEmpty() ?
		               std::format_to(context.out(), "{}", error.Context()) :
		               std::format_to(context.out(), "{} {}", error.Context(), error.About().View());

		return std::format_to(out, ": {} ({})", std::system_category().message(error.Code()), error.Code());
	}
};

// The contract everything downstream assumes.
static_assert(std::is_trivially_copyable_v<Error>, "An error is copied out of a Result, never owned behind one");
static_assert(std::is_trivially_copyable_v<Subject>, "A subject rides in an Error and owns no storage of its own");
static_assert(std::formattable<Error, char>, "A report prints the error rather than <unprintable>");

static_assert(Error{ 13, "opening the render node" }.Code() == 13);
static_assert(
	Error{ 13, "opening the render node" } == Error{ 13, "probing the connector" },
	"Equality is the code alone; the context is for the reader"
);
static_assert(Error{ 13, "" } != Error{ 2, "" });
