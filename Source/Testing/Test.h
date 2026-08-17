#pragma once

#include <format>
#include <source_location>
#include <string>
#include <string_view>

// A test harness, not a test framework.
//
// Decision 9 keeps a framework out of the dependency list, and what is actually wanted is small: a
// registry that costs nothing before main, assertions that say where they failed, and a runner
// CTest can invoke. Fixtures, mocks, tags, and parameterized suites are machinery this codebase has
// no use for, and the cost of adding one of them later is one file.
//
// Three properties shaped what is here rather than being incidental to it:
//
//   The registry is an intrusive list with a zero-initialized head, so registration cannot depend
//   on another translation unit's dynamic initialization having run, and no test costs an
//   allocation before main.
//
//   Assertions are callable from a thread a test spawned. The publication boundary is the thing
//   this codebase most needs to test and it has two threads by definition, so a harness that only
//   worked on the thread it was called from would be useless at exactly the interesting part.
//
//   Nothing here measures duration. CheckClockDiscipline.cmake forbids the harness a clock, which
//   is the right answer independently: CTest already reports wall time per entry, and a test that
//   asserts on its own runtime is a flaky test with extra steps.
//
// The macros do only what a macro must — stringify an expression, capture a source location, and
// return early. Everything else is an ordinary function, which is what keeps them steppable and
// keeps template instantiation off every assertion in the codebase.

class TestCase
{
public:
	using Body = void (*)();

	// Constructed only by GYRO_TEST, at static-init time, linking itself into the registry. Not
	// thread safe and not required to be: static initialization is single-threaded here.
	TestCase(std::string_view suite, std::string_view name, Body body) noexcept;

	TestCase(const TestCase&) = delete;
	TestCase& operator=(const TestCase&) = delete;

	[[nodiscard]] std::string_view Suite() const noexcept { return m_Suite; }
	[[nodiscard]] std::string_view Name() const noexcept { return m_Name; }
	[[nodiscard]] const TestCase* Next() const noexcept { return m_Next; }

	void Run() const { m_Body(); }

	// Registration order is link order, which is not something to depend on. The runner sorts.
	[[nodiscard]] static const TestCase* First() noexcept;

private:
	std::string_view m_Suite;
	std::string_view m_Name;
	Body m_Body;
	const TestCase* m_Next;
};

// Records a failure against the run. `detail` is the values behind the expression, where they can
// be printed, and empty where the expression is its own explanation.
void ReportFailure(std::string_view expression, std::string_view detail, const std::source_location& where);

[[nodiscard]] int RunTests(int argc, char** argv);

namespace Detail
{
// A type without a formatter is not a reason to refuse to compile a comparison. The expression text
// is already in the report, so requiring a formatter for everything ever compared would put
// boilerplate in front of writing a test in order to improve a message nobody has read yet.
template<typename T>
[[nodiscard]] std::string Describe(const T& value)
{
	if constexpr (std::formattable<T, char>)
	{
		return std::format("{}", value);
	}
	else
	{
		return "<unprintable>";
	}
}

// Templated so that the conversion to bool happens in a contextual position, which admits an
// explicit operator bool — an optional, a handle, a unique_ptr — without the caller spelling out a
// cast that -Wuseless-cast would then object to on the ordinary comparison case.
template<typename Value>
bool CheckTrue(const Value& value, std::string_view expression, const std::source_location& where)
{
	if (value)
	{
		return true;
	}

	ReportFailure(expression, {}, where);
	return false;
}

template<typename Left, typename Right>
bool CheckEqual(const Left& left, const Right& right, std::string_view expression, const std::source_location& where)
{
	if (left == right)
	{
		return true;
	}

	ReportFailure(expression, std::format("{} vs {}", Describe(left), Describe(right)), where);
	return false;
}
} // namespace Detail

// Defines a test. Suite and Name are identifiers rather than strings so that the pair also names
// the function, which is what makes a stack trace legible.
#define GYRO_TEST(Suite, Name)                                                                                         \
	static void Suite##_##Name();                                                                                      \
	[[maybe_unused]] static const TestCase Suite##_##Name##_Case{ #Suite, #Name, &Suite##_##Name };                    \
	static void Suite##_##Name()

// Variadic so that an expression containing a comma at paren depth zero — a braced initializer, a
// template with two arguments — does not have to be wrapped by the caller.
#define GYRO_CHECK(...) Detail::CheckTrue((__VA_ARGS__), #__VA_ARGS__, std::source_location::current())

#define GYRO_REQUIRE(...)                                                                                              \
	do                                                                                                                 \
	{                                                                                                                  \
		if (!GYRO_CHECK(__VA_ARGS__))                                                                                  \
		{                                                                                                              \
			return;                                                                                                    \
		}                                                                                                              \
	} while (false)

// Variadic for the same reason, and it is less obvious that it can be. The preprocessor knows only
// parentheses, so GYRO_CHECK_EQ(handles.front(), EntityId{ 0, 2 }) reaches a two-parameter macro as
// three arguments and does not expand at all — reported as the macro being undeclared, which names
// everything except the comma responsible. Substituting __VA_ARGS__ re-emits the caller's tokens
// unchanged, and the C++ parser that then splits them does understand braces and template argument
// lists, so it recovers the two operands the preprocessor could not.
//
// The cost is that the report carries the operands as written rather than joined by ==, which the
// two values printed beneath it already say. The operands are no longer individually parenthesized
// either, and nothing is lost with them: the only operator that could bind looser than a function
// argument is the comma, which the two-parameter form could not accept in the first place.
#define GYRO_CHECK_EQ(...) Detail::CheckEqual(__VA_ARGS__, #__VA_ARGS__, std::source_location::current())

#define GYRO_REQUIRE_EQ(...)                                                                                           \
	do                                                                                                                 \
	{                                                                                                                  \
		if (!GYRO_CHECK_EQ(__VA_ARGS__))                                                                               \
		{                                                                                                              \
			return;                                                                                                    \
		}                                                                                                              \
	} while (false)

#define GYRO_FAIL(Message) ReportFailure((Message), {}, std::source_location::current())
