#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Bake.h"
#include "Bdf.h"
#include "Core/Result.h"

// The font baker's entry point: BDF files in, one generated translation unit out.
//
//   GyroFonts --output <file.cpp> <font.bdf>...
//
// **All of the fonts in one invocation**, because the output is one ladder and `Text/Font.h`'s
// `Nearest` walks it: a per-file rule would produce one array per size with nothing to order them, and
// a size dropped from the CMake list would leave its table linked in.
//
// **A file whose text has not changed is not rewritten**, for the reason Tools/Bindings/Main.cpp gives
// — a touched output costs a rebuild of everything downstream, and a `find_package` re-run is enough
// to re-run this.

namespace
{
std::optional<std::string> ReadFile(const std::filesystem::path& path)
{
	std::ifstream file{ path, std::ios::binary };

	if (!file)
	{
		return std::nullopt;
	}

	return std::string{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
}

bool WriteIfChanged(const std::filesystem::path& path, std::string_view text)
{
	if (const std::optional<std::string> existing = ReadFile(path); existing.has_value() && *existing == text)
	{
		return true;
	}

	std::error_code failure;
	std::filesystem::create_directories(path.parent_path(), failure);

	if (failure)
	{
		std::fprintf(stderr, "gyro-fonts: %s: %s\n", path.string().c_str(), failure.message().c_str());

		return false;
	}

	std::ofstream file{ path, std::ios::binary | std::ios::trunc };

	file.write(text.data(), static_cast<std::streamsize>(text.size()));

	if (!file)
	{
		std::fprintf(stderr, "gyro-fonts: %s: could not be written\n", path.string().c_str());

		return false;
	}

	return true;
}
} // namespace

int main(int argc, char** argv)
{
	const std::span<char*> arguments{ argv, static_cast<std::size_t>(argc) };

	std::filesystem::path output;
	std::vector<std::filesystem::path> inputs;

	for (std::size_t index = 1; index < arguments.size(); ++index)
	{
		const std::string_view argument = arguments[index];

		if (argument == "--output")
		{
			if (index + 1 == arguments.size())
			{
				std::fprintf(stderr, "gyro-fonts: --output wants a file\n");

				return EXIT_FAILURE;
			}

			output = arguments[++index];

			continue;
		}

		if (argument.starts_with("--"))
		{
			std::fprintf(
				stderr, "gyro-fonts: %.*s is not an option\n", static_cast<int>(argument.size()), argument.data()
			);

			return EXIT_FAILURE;
		}

		inputs.emplace_back(argument);
	}

	if (output.empty() || inputs.empty())
	{
		std::fprintf(stderr, "usage: gyro-fonts --output <file.cpp> <font.bdf>...\n");

		return EXIT_FAILURE;
	}

	std::vector<FaceSource> faces;
	faces.reserve(inputs.size());

	for (const std::filesystem::path& input : inputs)
	{
		const std::optional<std::string> text = ReadFile(input);

		if (!text.has_value())
		{
			std::fprintf(stderr, "gyro-fonts: %s: could not be read\n", input.string().c_str());

			return EXIT_FAILURE;
		}

		std::string diagnostic;
		Result<BdfFont> parsed = ParseBdf(*text, &diagnostic);

		if (!parsed)
		{
			// The file, then the line, then what was wrong — the shape a compiler error has, because
			// this is one and it is read by whoever is looking at the build log.
			std::fprintf(stderr, "%s:%s\n", input.string().c_str(), diagnostic.c_str());

			return EXIT_FAILURE;
		}

		// The stem rather than the path. The path is a property of the machine doing the build, and
		// putting it in a generated file makes two machines produce two different ones.
		faces.push_back(FaceSource{ .Name = input.stem().string(), .Font = std::move(*parsed) });
	}

	std::string diagnostic;
	const Result<std::string> baked = Bake(std::move(faces), &diagnostic);

	if (!baked)
	{
		std::fprintf(stderr, "gyro-fonts: %s\n", diagnostic.c_str());

		return EXIT_FAILURE;
	}

	return WriteIfChanged(output, *baked) ? EXIT_SUCCESS : EXIT_FAILURE;
}
