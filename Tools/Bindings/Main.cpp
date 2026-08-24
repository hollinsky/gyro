#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "Core/Result.h"
#include "Emit.h"
#include "Protocol.h"
#include "Xml.h"

// The generator's entry point: XML paths in, a generated tree out.
//
//   GyroBindings [--server] --output <directory> <protocol.xml>...
//
// **The direction is a flag rather than a second program**, and the two runs share nothing but the
// parser: `--server` emits resources and handlers over libwayland into `Wayland/Server/`, and the
// default emits proxies and listeners over gyro's own codec into `Wayland/`. They are separate
// invocations with separate protocol lists, because what gyro speaks to its host and what it speaks
// to its clients are different sets of protocols that happen to be described by the same files.
//
// **Every protocol is one invocation**, because Tools/Bindings/Emit.h needs the whole set to resolve
// a reference that leaves its own document — and because one invocation is one build rule, so a
// protocol added to the CMake list regenerates all of them rather than leaving the file that names it
// stale until somebody clears the build directory.
//
// **A file whose text has not changed is not rewritten.** The generated headers are included by the
// nested backend, and touching them costs a rebuild of everything downstream — which would be every
// build, since a `find_package` re-run is enough to re-run the generator. Comparing before writing is
// four lines and turns a protocol-suite update into a rebuild of what actually changed.
//
// **A file this run did not write is deleted.** Dropping a protocol from the CMake list otherwise
// leaves its header sitting in the generated tree, on the include path, describing an interface
// nothing generates a definition for any more — so the build keeps working until somebody includes it
// and gets a link error naming a symbol they never asked for.
//
// Scoped to the directories this run wrote into, which is what makes the two directions coexist: the
// client run owns `Wayland/` and the server run owns `Wayland/Server/`, and neither prunes the
// other's tree. A sweep of `Wayland/` that recursed would have the client run delete every server
// header on every build.

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
		std::fprintf(stderr, "gyro-bindings: %s: %s\n", path.string().c_str(), failure.message().c_str());

		return false;
	}

	std::ofstream file{ path, std::ios::binary | std::ios::trunc };

	file.write(text.data(), static_cast<std::streamsize>(text.size()));

	if (!file)
	{
		std::fprintf(stderr, "gyro-bindings: %s: could not be written\n", path.string().c_str());

		return false;
	}

	return true;
}
// Everything in the directories this run wrote into that this run did not produce. A protocol removed
// from the build's list leaves a header behind otherwise, and a header on the include path that
// nothing generates a `.cpp` for is worse than a missing one: it compiles.
//
// **The directories come from what was written rather than from a fixed name**, and that is what
// keeps the two directions out of each other's way. `Wayland/` and `Wayland/Server/` are written by
// separate runs of this program; a sweep that recursed from the first would delete the second's
// output every build, and one that named a single directory would leave a dropped server protocol's
// header behind forever. Each run sweeps exactly the directories it filled, one level deep.
bool Prune(const std::vector<std::filesystem::path>& written)
{
	std::set<std::filesystem::path> directories;

	for (const std::filesystem::path& path : written)
	{
		directories.insert(path.parent_path());
	}

	for (const std::filesystem::path& directory : directories)
	{
		std::error_code failure;

		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{ directory, failure })
		{
			if (!entry.is_regular_file())
			{
				continue;
			}

			const std::filesystem::path path = entry.path().lexically_normal();

			if (std::ranges::find(written, path) != written.end())
			{
				continue;
			}

			std::filesystem::remove(path, failure);

			if (failure)
			{
				std::fprintf(stderr, "gyro-bindings: %s: could not be removed\n", path.string().c_str());

				return false;
			}
		}

		if (failure)
		{
			return false;
		}
	}

	return true;
}
} // namespace

int main(int argc, char** argv)
{
	const std::span<char*> arguments{ argv, static_cast<std::size_t>(argc) };

	std::filesystem::path output;
	std::vector<std::filesystem::path> inputs;
	Direction direction = Direction::Client;

	for (std::size_t index = 1; index < arguments.size(); ++index)
	{
		const std::string_view argument = arguments[index];

		if (argument == "--output")
		{
			if (index + 1 == arguments.size())
			{
				std::fprintf(stderr, "gyro-bindings: --output wants a directory\n");

				return EXIT_FAILURE;
			}

			output = arguments[++index];

			continue;
		}

		if (argument == "--server")
		{
			direction = Direction::Server;

			continue;
		}

		if (argument.starts_with("--"))
		{
			std::fprintf(
				stderr, "gyro-bindings: %.*s is not an option\n", static_cast<int>(argument.size()), argument.data()
			);

			return EXIT_FAILURE;
		}

		inputs.emplace_back(argument);
	}

	if (output.empty() || inputs.empty())
	{
		std::fprintf(stderr, "usage: gyro-bindings [--server] --output <directory> <protocol.xml>...\n");

		return EXIT_FAILURE;
	}

	std::vector<ProtocolSource> protocols;
	protocols.reserve(inputs.size());

	for (const std::filesystem::path& input : inputs)
	{
		const std::optional<std::string> text = ReadFile(input);

		if (!text.has_value())
		{
			std::fprintf(stderr, "gyro-bindings: %s: could not be read\n", input.string().c_str());

			return EXIT_FAILURE;
		}

		Diagnostic diagnostic;
		Result<Protocol> parsed = ParseProtocol(*text, &diagnostic);

		if (!parsed)
		{
			// The file, then the position, then what was wrong — the shape a compiler error has,
			// because this is one and it is read by whoever is looking at the build log.
			std::fprintf(stderr, "%s:%s\n", input.string().c_str(), std::format("{}", diagnostic).c_str());

			return EXIT_FAILURE;
		}

		// The file name rather than the path it was found at. The path is a property of the machine
		// doing the build — a distribution's pkgdatadir, a container's mount point — and putting it in
		// a generated header makes two machines produce two different files from one protocol.
		protocols.push_back(ProtocolSource{ .Model = std::move(*parsed), .Path = input.filename().string() });
	}

	EmitDiagnostic diagnostic;
	const Result<std::vector<EmittedFile>> files = Emit(protocols, direction, &diagnostic);

	if (!files)
	{
		std::fprintf(stderr, "gyro-bindings: %s\n", diagnostic.Message.c_str());

		return EXIT_FAILURE;
	}

	std::vector<std::filesystem::path> written;

	for (const EmittedFile& file : *files)
	{
		const std::filesystem::path path = output / file.Path;

		if (!WriteIfChanged(path, file.Text))
		{
			return EXIT_FAILURE;
		}

		written.push_back(path.lexically_normal());
	}

	return Prune(written) ? EXIT_SUCCESS : EXIT_FAILURE;
}
