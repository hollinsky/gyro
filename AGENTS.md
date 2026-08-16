This is going to be a large compositor for Linux / Wayland, which sets itself apart by being an entire system layer, i.e. one compositor per system, not per session. It is run in SCHED_FIFO, with the goal of hitting every single frame. It handles animations and visual effects like blurs. Code quality is paramount so typically you should ask if you think there's some extra effort which should be added to make the code better.

It does not use VTs at all. It has no session dependencies either: device access comes from udev rules,
DRM master from first-open, and session lifecycle from a listener handed to it by a per-session agent.
That makes it a boot service rather than a login one — it subsumes the boot splash, continuing the
firmware's BGRT logo, and provides the recovery console that replaces VTs.

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

	Tools/
		UringProbe.cpp        - Standalone io_uring capability probe. Raw syscalls, no liburing,
		                        so it runs on a target machine before gyro does. First draft of
		                        gyro's own startup check; see Decisions.md decision 3

	Docs/
		Architecture.md - Platform seam, backends, boot and display lifetime, rendering devices,
		                  threads and the publication boundary, presentation timing, geometry and
		                  coordinate spaces, effects and quality, colour, sessions and users, the
		                  shell, login agent, event loop, protocol layer
		Animation.md    - Animation system: springs, motion catalog, commits, transforms, identity,
		                  lifetime, exit pixels
		Decisions.md    - Decision log with rejected alternatives and rationale (56 decisions)

	CMakeLists.txt  - Build configuration, dependency management via CPM
	.clang-format   - Code style (tabs, Allman braces, 120 col limit)
	AGENTS.md       - Project context for AI agents (note that CLAUDE.md is a symlink for Claude)

Read Docs/ before proposing architectural changes. Decisions.md records what was rejected and why;
re-litigating a settled decision requires engaging with the recorded rationale.

Dependencies are managed via CPM (CMake Package Manager):
	- All dependencies fetched at configure time from GitHub
	- Static linking preferred for deployment simplicity

AI Agent Notes:
	- Agents shall NOT include "Co-Authored-By:" footers or any other attribution in commit messages for this project
