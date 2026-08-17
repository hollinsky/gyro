#include "Testing/Test.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <print>
#include <utility>
#include <vector>

namespace
{
// Zero-initialized before any dynamic initialization runs, which is what lets a test register
// without knowing whether another translation unit's statics have been constructed. constinit is
// the assertion that it stays that way — give this an initializer that is not constant and the
// static-init order fiasco arrives silently, as a test that is simply missing from the run.
constinit const TestCase* s_Head = nullptr;

// A test may spawn threads, so a check can arrive from somewhere other than the runner. Relaxed
// because this counts rather than orders: the runner reads it between tests, with the test's own
// joins standing between the write and the read.
constinit std::atomic<std::size_t> s_Failures{ 0 };

// Reports name files the way the repository does. source_location carries whatever path reached
// the compiler, which is absolute here and is noise in anything that gets pasted somewhere.
[[nodiscard]] std::string_view TrimToSource(std::string_view path) noexcept
{
	constexpr std::string_view Root = "Source/";

	const std::size_t at = path.rfind(Root);
	return at == std::string_view::npos ? path : path.substr(at);
}

[[nodiscard]] constexpr std::string_view Plural(std::size_t count) noexcept
{
	return count == 1 ? "" : "s";
}
} // namespace

TestCase::TestCase(std::string_view suite, std::string_view name, Body body) noexcept
	: m_Suite{ suite }, m_Name{ name }, m_Body{ body }, m_Next{ s_Head }
{
	s_Head = this;
}

const TestCase* TestCase::First() noexcept
{
	return s_Head;
}

void ReportFailure(std::string_view expression, std::string_view detail, const std::source_location& where)
{
	s_Failures.fetch_add(1, std::memory_order_relaxed);

	// Formatted whole and written once, so a report from a spawned thread interleaves between
	// reports rather than inside one.
	const std::string report =
		detail.empty() ?
			std::format("    {}:{}: {}\n", TrimToSource(where.file_name()), where.line(), expression) :
			std::format("    {}:{}: {}\n      {}\n", TrimToSource(where.file_name()), where.line(), expression, detail);

	std::print("{}", report);
}

int RunTests(int argc, char** argv)
{
	// Line buffered, so the name of a test that segfaults has already reached the terminal. A
	// compositor's test suite crashing is an ordinary Tuesday and the name is the whole diagnosis.
	std::setvbuf(stdout, nullptr, _IOLBF, 0);

	std::vector<std::string_view> filters;
	bool listOnly = false;

	for (int i = 1; i < argc; ++i)
	{
		const std::string_view argument{ argv[i] };

		if (argument == "--list")
		{
			listOnly = true;
		}
		else if (argument.starts_with('-'))
		{
			std::println("Usage: {} [--list] [name filter...]", argv[0]);
			return 2;
		}
		else
		{
			filters.push_back(argument);
		}
	}

	std::vector<const TestCase*> cases;
	for (const TestCase* test = TestCase::First(); test != nullptr; test = test->Next())
	{
		cases.push_back(test);
	}

	// Registration order is link order. Sorting makes a run reproducible, which matters more here
	// than it looks: a harness whose output reorders between builds cannot be diffed.
	std::ranges::sort(cases, {}, [](const TestCase* test) { return std::pair{ test->Suite(), test->Name() }; });

	std::size_t ran = 0;
	std::size_t failed = 0;

	for (const TestCase* test : cases)
	{
		const std::string name = std::format("{}.{}", test->Suite(), test->Name());

		if (!filters.empty() &&
		    std::ranges::none_of(filters, [&](std::string_view filter) { return name.contains(filter); }))
		{
			continue;
		}

		if (listOnly)
		{
			std::println("{}", name);
			continue;
		}

		std::println("{}", name);

		const std::size_t before = s_Failures.load(std::memory_order_relaxed);
		test->Run();
		const std::size_t after = s_Failures.load(std::memory_order_relaxed);

		++ran;
		if (after != before)
		{
			++failed;
		}
	}

	if (listOnly)
	{
		return 0;
	}

	if (failed == 0)
	{
		std::println("{} test{}, none failed", ran, Plural(ran));
		return 0;
	}

	const std::size_t checks = s_Failures.load(std::memory_order_relaxed);
	std::println("{} test{}, {} failed, {} failed check{}", ran, Plural(ran), failed, checks, Plural(checks));
	return 1;
}
