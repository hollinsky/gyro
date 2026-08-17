This is going to be a large compositor for Linux / Wayland, which sets itself apart by being an entire system layer, i.e. one compositor per system, not per session. It is run in SCHED_FIFO, with the goal of hitting every single frame. It handles animations and visual effects like blurs. Code quality is paramount so typically you should ask if you think there's some extra effort which should be added to make the code better.

It does not use VTs at all. It has no session dependencies either: device access comes from udev rules,
DRM master from first-open, and session lifecycle from a listener handed to it by a per-session agent.
That makes it a boot service rather than a login one — it subsumes the boot splash, continuing the
firmware's BGRT logo, and provides the recovery console that replaces VTs.

Nothing commercial. Do not add dependencies without asking first and justifying their inclusion.

None of these code or architecture decisions are sacred, there's no "mine" vs "yours," we're building this in its entirety together. We should also treat the world around us as malleable if it helps us. If we need to propose some change to the kernel or create a new standard for userspace, let's talk about it.

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

		Core/                 - Portable tier. Time.h is the timebase (Instant, Duration, the
		                        ingest conversions); Clock.h is IClock, MonotonicClock, and the
		                        ManualClock the headless backend and tests drive; Wake.h is the
		                        contribution the idle fold reduces, and is in Core rather than
		                        Animation because Console contributes to it too (decision 69)
		Geometry/             - Portable tier. Scale.h is the exact rational output scale; Space.h is
		                        the coordinate spaces and the values that live in them, with the
		                        integer grid kept off the world
		Animation/            - Portable tier, split by direction rather than by purity. Solve/ holds
		                        the closed-form spring the frame thread evaluates; Author/ produces
		                        coefficients and is dispatch-side. A frame-side include of Author/ is
		                        a violation; see decisions 11 and 12
		Testing/              - The hand-rolled test harness and every test binary's main().
		                        GYRO_TEST / GYRO_CHECK / GYRO_REQUIRE; see decision 9

		Tests live beside what they test, as <Unit>.Test.cpp, and are listed in the module's
		TESTS rather than compiled into it.

	Tools/
		UringProbe.cpp        - Standalone io_uring capability probe. Raw syscalls, no liburing,
		                        so it runs on a target machine before gyro does. First draft of
		                        gyro's own startup check; see Decisions.md decision 3

	Docs/
		Experience.md   - Tier 1. What a person perceives, stated without mechanism: the six
		                  promises, how the system degrades, what is deliberately not promised
		Architecture.md - Tier 2. Platform seam, backends, boot and display lifetime, rendering
		                  devices, threads and the publication boundary, presentation timing,
		                  geometry and coordinate spaces, effects and quality, colour, sessions and
		                  users, the shell, login agent, event loop, protocol layer
		Animation.md    - Tier 2. Animation system: springs, motion catalog, commits, transforms,
		                  interactive transitions, identity, lifetime, exit pixels
		Structure.md    - Tier 3. Modules and their tiers, the dependency graph and the two waists
		                  it hangs off, which thread each piece runs on, the composition root, and
		                  what the build checks enforce
		Decisions.md    - Cross-cutting. Decision log with rejected alternatives and rationale
		                  (71 decisions). Append-mostly: a revised decision keeps its superseded
		                  position as a rejected alternative, and carries its revision history
		                  inline and dated rather than in any global ledger
		Open.md         - Cross-cutting. The questions not yet settled, roughly in the order they
		                  will bite. Answering one produces a decision; this is the churning half
		                  of what used to be Decisions.md's tail

	Docs are tiered: Experience (what the user perceives) → Architecture and Animation (mechanism
	and invariants) → Structure (where the mechanism lives). Citations point up; dependencies
	point down. A tier-2 document may cite Experience as justification; Experience may never require
	a mechanism document in order to be understood.

	CMakeLists.txt  - Build configuration, dependency management via CPM
	.clang-format   - Code style (tabs, Allman braces, 120 col limit)
	AGENTS.md       - Project context for AI agents (note that CLAUDE.md is a symlink for Claude)

	CMake/
		BuildFlags.cmake           - The warning set, sanitizers (GYRO_SANITIZE), GYRO_WERROR,
		                             GYRO_LTO, and the source-root include path. Everything links it
		Module.cmake               - gyro_add_module(). PORTABLE declares a module part of the tier
		                             decision 6 keeps free of Linux headers; DEPENDS declares its
		                             edges in Structure.md's module graph
		CheckClockDiscipline.cmake - One reader of the timebase, enforced (decision 57)
		CheckPortability.cmake     - No platform headers in a PORTABLE module (decision 6)
		CheckLayering.cmake        - Every #include lies on a declared DEPENDS edge; graph is a DAG

	All three checks are ALL targets, so an ordinary build runs them; a violation fails the build
	rather than waiting for review. Add a discipline here rather than to a style guide nobody greps.

	Build: cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build

Read Docs/ before proposing architectural changes. Decisions.md records what was rejected and why;
re-litigating a settled decision requires engaging with the recorded rationale.

Dependencies are managed via CPM (CMake Package Manager):
	- All dependencies fetched at configure time from GitHub
	- Static linking preferred for deployment simplicity

AI Agent Notes:
	- Agents shall NOT include "Co-Authored-By:" footers or any other attribution in commit messages for this project
