#include "Testing/Test.h"

// Every test binary's entry point. Separate from Test.cpp only so that the runner stays testable
// by something other than itself if it ever needs to be.
int main(int argc, char** argv)
{
	return RunTests(argc, argv);
}
