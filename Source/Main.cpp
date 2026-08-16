#include <cstring>
#include <iostream>

#include "Version.h"

static void PrintVersion()
{
	std::cout << AppName << " " << AppVersion << std::endl;
}

static void PrintUsage()
{
	std::cout << "Usage: " << AppName << " [options]\n"
			  << "\n"
			  << "Options:\n"
			  << "  --help       Show this help message\n"
			  << "  --version    Show version information\n"
			  << "\n"
			  << "Running with no options launches the desktop application.\n";
}

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0)
		{
			PrintVersion();
			return 0;
		}
		if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
		{
			PrintUsage();
			return 0;
		}

		std::cerr << "Unknown option: " << argv[i] << "\n";
		PrintUsage();
		return 1;
	}

	std::cout << "All the time in the world, but nothing to do yet..." << std::endl;
	return 0;
}
