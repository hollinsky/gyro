#include "Shell/Launch.h"

#include "Testing/Test.h"

// What a person typed becomes an argument vector, and the whole of the rule is whitespace. These are
// the cases a launcher sees on the first day: a bare program, one with a flag, and the leading and
// trailing spaces a person leaves behind while deleting a word.
GYRO_TEST(Launcher, SplitsOnWhitespace)
{
	const std::vector<std::string> bare = Launcher::Words("firefox");

	GYRO_REQUIRE(bare.size() == 1);
	GYRO_REQUIRE(bare[0] == "firefox");

	const std::vector<std::string> flagged = Launcher::Words("  foot   -e   htop ");

	GYRO_REQUIRE(flagged.size() == 3);
	GYRO_REQUIRE(flagged[0] == "foot");
	GYRO_REQUIRE(flagged[1] == "-e");
	GYRO_REQUIRE(flagged[2] == "htop");
}

// Nothing typed is nothing to run, which is what stops a person who summoned the bar and changed
// their mind from having Enter start whatever `execvpe` makes of an empty name.
GYRO_TEST(Launcher, HasNothingToRunForABlankQuery)
{
	GYRO_REQUIRE(Launcher::Words("").empty());
	GYRO_REQUIRE(Launcher::Words("   \t ").empty());

	Launcher launcher;
	launcher.Adopt({});

	GYRO_REQUIRE(!launcher.Run("   ").has_value());
}
