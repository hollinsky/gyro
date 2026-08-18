#include "Core/Result.h"

#include <cerrno>
#include <format>
#include <string>

#include "Testing/Test.h"

namespace
{
Result<int> OpenLike(bool succeeds)
{
	if (!succeeds)
	{
		return Failure(EACCES, "opening the render node");
	}

	return 7;
}

Result<void> CloseLike(bool succeeds)
{
	if (!succeeds)
	{
		return Failure(EBADF, "closing the render node");
	}

	return {};
}
} // namespace

GYRO_TEST(Result, CarriesAValue)
{
	const Result<int> result = OpenLike(true);

	GYRO_REQUIRE(result.has_value());
	GYRO_CHECK_EQ(*result, 7);
}

GYRO_TEST(Result, CarriesACodeAndTheOperationThatFailed)
{
	const Result<int> result = OpenLike(false);

	GYRO_REQUIRE(!result.has_value());
	GYRO_CHECK_EQ(result.error().Code(), EACCES);
	GYRO_CHECK_EQ(result.error().Context(), std::string_view{ "opening the render node" });
}

GYRO_TEST(Result, VoidIsTheSameShape)
{
	GYRO_CHECK(CloseLike(true).has_value());

	const Result<void> failed = CloseLike(false);
	GYRO_REQUIRE(!failed.has_value());
	GYRO_CHECK_EQ(failed.error().Code(), EBADF);
}

GYRO_TEST(Result, FromErrnoReadsTheCallSite)
{
	errno = ENODEV;
	const Error error = Error::FromErrno("probing the connector");

	GYRO_CHECK_EQ(error.Code(), ENODEV);
	GYRO_CHECK_EQ(error.Context(), std::string_view{ "probing the connector" });

	errno = ENOENT;
	const std::unexpected<Error> failure = FailFromErrno("opening the device");
	GYRO_CHECK_EQ(failure.error().Code(), ENOENT);

	errno = 0;
}

GYRO_TEST(Result, EqualityIsTheCodeAlone)
{
	// Two sites failing the same way are the same failure to anything that branches on one. A
	// comparison that read the context would make a test assert on prose.
	GYRO_CHECK_EQ(Error(EACCES, "one"), Error(EACCES, "another"));
	GYRO_CHECK(Error(EACCES, "one") != Error(EPERM, "one"));
}

GYRO_TEST(Result, FormatsWithTheOperationFirst)
{
	const std::string report = std::format("{}", Error{ EACCES, "opening the render node" });

	// The code and the operation are asserted; the middle is the C library's message for the
	// platform, which is not gyro's to fix in place.
	GYRO_CHECK(report.starts_with("opening the render node: "));
	GYRO_CHECK(report.ends_with(std::format(" ({})", EACCES)));
}

GYRO_TEST(Result, MonadicOperationsCompose)
{
	// std::expected is the reason this is an alias rather than a class: the shape is already there.
	const Result<int> doubled = OpenLike(true).transform([](int value) { return value * 2; });
	GYRO_REQUIRE(doubled.has_value());
	GYRO_CHECK_EQ(*doubled, 14);

	const Result<int> untouched = OpenLike(false).transform([](int value) { return value * 2; });
	GYRO_REQUIRE(!untouched.has_value());
	GYRO_CHECK_EQ(untouched.error().Code(), EACCES);

	GYRO_CHECK_EQ(OpenLike(false).value_or(-1), -1);
}
