This is going to be a large compositor for Linux / Wayland, which sets itself apart by being an entire system layer, i.e. one compositor per system, not per session. It is run in SCHED_FIFO, with the goal of hitting every single frame. It handles animations and visual effects like blurs. Code quality is paramount so typically you should ask if you think there's some extra effort which should be added to make the code better.

It does not use VTs at all, though it expects a logind style session claiming interface.

Nothing commercial. Do not add dependencies without asking first and justifying their inclusion.

Ask questions before implementing where it helps you to implement more effectively.

Always format C++ code using the project's `.clang-format` configuration before finalizing changes.

Summary of the Stack:
	Language: C++23 (CMake, CPM)
	Graphics API: Vulkan
	Logging: spdlog

Codebase Structure:
	Source/
		Main.cpp              - Thin entry point: CLI arg parsing (--version, --help)
		Version.h.in          - CMake-configured version string template

	CMakeLists.txt  - Build configuration, dependency management via CPM
	.clang-format   - Code style (tabs, Allman braces, 120 col limit)
	AGENTS.md       - Project context for AI agents (note that CLAUDE.md is a symlink for Claude)

Dependencies are managed via CPM (CMake Package Manager):
	- All dependencies fetched at configure time from GitHub
	- Static linking preferred for deployment simplicity

AI Agent Notes:
	- Agents shall NOT include "Co-Authored-By:" footers or any other attribution in commit messages for this project
