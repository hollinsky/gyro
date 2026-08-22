cmake_minimum_required(VERSION 3.25)

project(gyro
	VERSION 0.0.0
	DESCRIPTION "Gyro Compositor"
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ISO rather than GNU. Core/Clock.cpp asks for POSIX by defining _POSIX_C_SOURCE where it needs it,
# which is the portable tier's rule spelled out: what is wanted from the platform is named.
set(CMAKE_CXX_EXTENSIONS OFF)
# Lock C standard before any CPM dep can override it (libgit2 sets C90 in cache)
set(CMAKE_C_STANDARD 23 CACHE STRING "The C standard to compile against" FORCE)
set(FETCHCONTENT_QUIET OFF)

set(GYRO_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# Setup CPM
set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm-cache" CACHE PATH "CPM cache directory")
include(CMake/CPM.cmake)

include(CMake/BuildFlags.cmake)
include(CMake/Module.cmake)

enable_testing()

find_package(Git)

add_custom_target(AppVersion
	COMMAND ${CMAKE_COMMAND}
		-D GIT_EXECUTABLE=${GIT_EXECUTABLE}
		-D GIT_WORKING_DIR=${CMAKE_CURRENT_SOURCE_DIR}
		-D INPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/Source/Version.h.in
		-D OUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/Generated/Version.h
		-D PROJECT_NAME=${PROJECT_DESCRIPTION}
		-D VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
		-D VERSION_MINOR=${PROJECT_VERSION_MINOR}
		-D VERSION_PATCH=${PROJECT_VERSION_PATCH}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/Version.cmake
	DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/Source/Version.h.in
	BYPRODUCTS
		${CMAKE_CURRENT_BINARY_DIR}/Generated/Version.h
)

# The timebase has exactly one reader; checked on every build. ALL rather than a dependency of gyro,
# so the rule covers the test binaries and anything else added later without being remembered.
add_custom_target(ClockDiscipline ALL
	COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${GYRO_SOURCE_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckClockDiscipline.cmake
	VERBATIM
)

# The platform stack, consumed through pkg-config; CPM is for C++ libraries. See
# Docs/Architecture.md#dependencies, where this row already sits.
#
# liburing is the first of them, and it arrives with the frame ring rather than with the event loop in
# general: the dispatch ring is a convenience where plain epoll would be defensible, and the frame
# ring is where DEFER_TASKRUN earns its place. Docs/Architecture.md#why-io_uring has the argument, and
# Source/Compositor/Uring.h has the one reap-site constraint that follows from it.
#
# Named REQUIRED with a sentence rather than left to pkg-config's own message, because the machine
# that hits this is a machine somebody is setting up and the fix is one package away.
find_package(PkgConfig REQUIRED)
pkg_check_modules(liburing IMPORTED_TARGET liburing)

if(NOT liburing_FOUND)
	message(FATAL_ERROR
		"liburing was not found. gyro's frame ring is io_uring and carries no epoll fallback, "
		"deliberately; see Docs/Architecture.md#dependencies. Install liburing-devel (Fedora) or "
		"liburing-dev (Debian)."
	)
endif()

# The frame thread is a thread, and the composition root is what starts it. Declared here with the rest
# of the platform stack rather than beside the one target that used to want it, because it is now two:
# Compositor spawns the frame thread and drives its ring from a second one under test, and Integration
# runs the publication soak.
find_package(Threads REQUIRED)

# spdlog, on std::format rather than on its bundled fmt.
#
# This codebase writes std::formatter specializations — Wake, FrameDecision, OutputConfiguration,
# Budget, Plan — and writes them precisely so that a log line can print one. Left on bundled fmt,
# spdlog would accept none of them and every log site would hand-expand the fields, which is how a
# formatter and the line that prints it drift apart. One vocabulary, decided here.
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "spdlog formats with std::format" FORCE)

CPMAddPackage(
	NAME spdlog
	GITHUB_REPOSITORY gabime/spdlog
	GIT_TAG v1.17.0
	SYSTEM YES
	EXCLUDE_FROM_ALL YES
)

# The test harness, declared before the modules whose tests link it.
gyro_add_module(Testing PORTABLE OBJECT
	SOURCES
		Main.cpp
		Test.cpp
)

# OBJECT for Testing's reason, one step less obvious. DebugAllocator.cpp defines the replaceable
# operator new/delete set and nothing references a symbol in it, so as a static archive member it
# would never be extracted and the check would be silently absent from every binary that links Core.
# An object library has no shelf to leave it on.
#
# The consequence to know about: CMake adds an object library's files only to the targets that name
# it directly, so reaching Core through another module's INTERFACE library drops them. That is why
# gyro_add_module names Core on every test executable rather than relying on the graph, and why the
# gyro target below already names it.
gyro_add_module(Core PORTABLE OBJECT
	SOURCES
		Clock.cpp
		DebugAllocator.cpp
		Fd.cpp
		Signal.cpp
	TESTS
		Clock.Test.cpp
		ColorState.Test.cpp
		Fd.Test.cpp
		FrameSection.Test.cpp
		Handle.Test.cpp
		Quiet.Test.cpp
		Result.Test.cpp
		Signal.Test.cpp
		SlotAllocator.Test.cpp
		Time.Test.cpp
		Wake.Test.cpp
)

# Header-only, so the tests are the only thing that compiles it. See Docs/Structure.md, both for why
# Geometry is not part of Core and for what that arrangement makes true.
gyro_add_module(Geometry PORTABLE
	DEPENDS
		Core
	TESTS
		AxisTransform.Test.cpp
		NodeTransform.Test.cpp
		Region.Test.cpp
		Scale.Test.cpp
		Space.Test.cpp
)

# Split by direction rather than by tier: Solve consumes spring coefficients and is what the frame
# thread may call, Author produces them and is dispatch-side.
#
# The Geometry edge is the catalog's and only the catalog's, and it is worth knowing which half of it
# drew the edge. Not the springs: a spring's parameters are scalar whatever its channel's value type
# is, so the bundle table names channels without naming their types. Not the settling thresholds
# either — those arrive as arguments precisely so that neither the geometric ones nor the unresolved
# non-geometric ones have to be known here. It is the gesture mapping: a travel distance is a
# displacement in global space, and typing it is what keeps a device-pixel distance from being
# written where an output-independent one belongs.
#
# FRAME_DEPENDS is what makes that a rule rather than a description. The edge arrived with authoring,
# so it stops at the publication boundary: Solve is scalar throughout and reaches Core only, and a
# geometric type appearing in the half the frame thread calls is a build failure rather than something
# a reader has to notice.
gyro_add_module(Animation PORTABLE
	DISPATCH_HALF
		Author
	DEPENDS
		Core
		Geometry
	FRAME_DEPENDS
		Core
	TESTS
		Solve/Ramp.Test.cpp
		Solve/Spring.Test.cpp
		Author/Animatable.Test.cpp
		Author/Bundle.Test.cpp
		Author/Catalog.Test.cpp
		Author/Drive.Test.cpp
		Author/Motion.Test.cpp
		Author/Retarget.Test.cpp
)

# The data waist: what crosses between the two threads — the snapshot representation and its wait-free
# reader, the newest-wins forward ring, the return channel carrying the consumed-sequence watermark
# and the per-buffer hold, and the deferred reclamation that hangs off it. Split by direction like
# Animation is: Reader is the frame half and is the default, Publisher is the dispatch half that
# serialises, owns the buffers, and allocates.
#
# FRAME_DEPENDS Core narrows the frame half to Core alone. The waist rests on Core and Geometry
# (Docs/Structure.md), but Geometry is authoring-side content — the coordinate spaces a position is
# expressed in before it is serialised — so it stops at the boundary the same way Animation's does. A
# geometric type reaching the reader would be a build failure with the reason attached rather than
# something a reader has to notice.
gyro_add_module(Publication PORTABLE
	DISPATCH_HALF
		Publisher
	DEPENDS
		Core
		Geometry
	FRAME_DEPENDS
		Core
	TESTS
		Return.Test.cpp
		Ring.Test.cpp
		Snapshot.Test.cpp
		Reader/Reader.Test.cpp
		Publisher/Outbox.Test.cpp
		Publisher/Publisher.Test.cpp
)

# The control waist: every interface with more than one implementation, and the plain data that
# crosses them. This is the presentation half — IPresenter and what its two verbs take and report.
# IRenderer, ISession and IInput join it where they are written.
#
# Portable, and it is worth saying why an interface to KMS can be. Nothing here names a Linux header:
# a format is a four-character code and a modifier is a number, both reproduced from stable kernel ABI
# rather than included, and a descriptor is Core/Fd.h's. That is what lets the frame loop's
# schedulability sweep run against a fake presenter on a machine with no GPU, which is the first point
# at which decision 29 becomes falsifiable rather than argued.
#
# No DISPATCH_HALF. Both threads touch these types and neither owns them — the frame thread calls both
# verbs, dispatch authors the configuration one of them takes — so there is no half to name and the
# module is deliberately the same on both sides of the boundary.
gyro_add_module(Seam PORTABLE
	DEPENDS
		Core
		Geometry
	TESTS
		EventSource.Test.cpp
		OutputConfiguration.Test.cpp
		PresentationInfo.Test.cpp
		Presenter.Test.cpp
		RenderTarget.Test.cpp
		Renderer.Test.cpp
		SyncPoint.Test.cpp
)

# The frame side, and for now the one piece of it that has no dependency on the rest: the per-output
# prediction every deadline in the system is derived from. The frame loop, admission control, and the
# timing policy join it here, which is what makes Structure.md's table read wider than this does.
#
# DEPENDS names what the code has rather than where the module is going. Declaring the eventual set
# early would spend the check that matters — CheckLayering denies what is not declared, so a narrower
# declaration is a stronger rule, and the edges arrive with the includes that need them.
#
# Portable, and the clock is where that stops being an aspiration: it is fed by PresentationInfo and
# OutputConfiguration rather than by a drm_event, so decision 29's schedulability sweep can drive it
# at arbitrary mixed rates and phases on a machine with no GPU. No DISPATCH_HALF — the whole module
# is frame-side, which is the default and the reason the default is that way round.
gyro_add_module(Frame PORTABLE
	DEPENDS
		Core
		Geometry
		Publication
		Seam
	TESTS
		Admission.Test.cpp
		Budget.Test.cpp
		FrameClock.Test.cpp
		Loop.Test.cpp
		Timing.Test.cpp
)

# The instrument, and PORTABLE is the load-bearing word.
#
# Docs/Structure.md's table has the three backends as platform code, and this one turns out not to be:
# a simulated panel is arithmetic over Core/Time.h, its images are heap pages rather than dumb buffers,
# and Seam/EventSource.h already settles that a headless source has no descriptor at all — its flips
# are a function of the clock the test drives, so there is no file to poll and nothing to open. What
# the tier buys is that CheckPortability.cmake holds it there: the one place decision 29's
# schedulability claim is falsified cannot acquire a platform dependency without the build saying so,
# and it keeps running on a machine with no GPU, no seat, and no compositor.
#
# No DISPATCH_HALF yet. Scripted input is dispatch-side and arrives with the input seam; presentation
# is all that is here, and Docs/Structure.md's thread table already has that on the frame side.
gyro_add_module(Headless PORTABLE
	DEPENDS
		Core
		Geometry
		Seam
	TESTS
		Device.Test.cpp
		Output.Test.cpp
		Planes.Test.cpp
		Renderer.Test.cpp
		Vblank.Test.cpp
)

# The composition root, and the only module that knows both sides of either waist. Docs/Structure.md
# #orchestration is its whole brief: it constructs the clock, the backend, the rendering device, the
# publication channel, and the threads, and hands each subsystem what it needs.
#
# Not PORTABLE, and it is the first module in the tree that is not. That is the tier working rather
# than eroding — io_uring, SCHED_FIFO, mlockall, and RLIMIT_RTTIME all live here, and they live here
# *because* Frame and Headless may not say any of those words. Decision 80 is what put them on this
# side of the line: the loop is a step and the wait is the root's, so the platform collects at the
# top rather than seeping into the module the schedulability sweep runs.
#
# It depends on everything below it and always will, which is why it is the one module whose DEPENDS
# list is not an argument about layering. No DISPATCH_HALF: the root is what *constructs* both threads
# and runs on neither, so it belongs to the frame side by the default the halves are written around.
gyro_add_module(Compositor
	DEPENDS
		Core
		Geometry
		Publication
		Seam
		Frame
		Headless
	SOURCES
		Compositor.cpp
		RealTime.cpp
		Uring.cpp
	TESTS
		Options.Test.cpp
		Schedule.Test.cpp
		Uring.Test.cpp
)

# PUBLIC so that both the gyro binary and this module's own tests inherit them. gyro_add_module names
# only the module graph, which is the right thing for it to know: an external dependency is a property
# of the module rather than an edge in Docs/Structure.md's graph, and CheckLayering has nothing to say
# about either of these.
target_link_libraries(Compositor
	PUBLIC
		PkgConfig::liburing
		spdlog::spdlog
		Threads::Threads
)

# Declared after the modules, because the module list is what they check. ALL for the same reason as
# the clock check above.
#
# A semicolon inside a custom command's argument is split into separate arguments before the script
# sees it, so both checks are handed a separator of their own and split it themselves. VERBATIM is
# what keeps the graph's `|` from reaching the shell as a pipe.
get_property(GYRO_PORTABLE_MODULES GLOBAL PROPERTY GYRO_PORTABLE_MODULES)
list(JOIN GYRO_PORTABLE_MODULES "," GYRO_PORTABLE_MODULES)
add_custom_target(Portability ALL
	COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${GYRO_SOURCE_DIR}
		-D MODULES=${GYRO_PORTABLE_MODULES}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckPortability.cmake
	VERBATIM
)

# The module graph, flattened to "Name=Dep,Dep|Name=Dep,Dep" because a -P script takes no maps.
get_property(GYRO_MODULES GLOBAL PROPERTY GYRO_MODULES)
set(GYRO_MODULE_GRAPH "")
foreach(MODULE IN LISTS GYRO_MODULES)
	get_property(MODULE_DEPENDS GLOBAL PROPERTY GYRO_MODULE_DEPENDS_${MODULE})
	list(JOIN MODULE_DEPENDS "," MODULE_DEPENDS)
	list(APPEND GYRO_MODULE_GRAPH "${MODULE}=${MODULE_DEPENDS}")
endforeach()
list(JOIN GYRO_MODULE_GRAPH "|" GYRO_MODULE_GRAPH)

# The second partition, which the graph cannot express: the halves of a straddling module, and the
# modules that are dispatch-side entire. Both are empty until a module says otherwise.
get_property(GYRO_DISPATCH_MODULES GLOBAL PROPERTY GYRO_DISPATCH_MODULES)
get_property(GYRO_DISPATCH_HALVES GLOBAL PROPERTY GYRO_DISPATCH_HALVES)
list(JOIN GYRO_DISPATCH_MODULES "," GYRO_DISPATCH_MODULES)
list(JOIN GYRO_DISPATCH_HALVES "," GYRO_DISPATCH_HALVES)

# The same partition applied to the edges rather than to the includes: what a straddling module's
# frame half may reach, for the modules that narrow it. Same flattening as the graph above, because
# it is read the same way and against the same closure.
get_property(GYRO_FRAME_LIMITED_MODULES GLOBAL PROPERTY GYRO_FRAME_LIMITED_MODULES)
set(GYRO_FRAME_GRAPH "")
foreach(MODULE IN LISTS GYRO_FRAME_LIMITED_MODULES)
	get_property(FRAME_DEPENDS GLOBAL PROPERTY GYRO_MODULE_FRAME_DEPENDS_${MODULE})
	list(JOIN FRAME_DEPENDS "," FRAME_DEPENDS)
	list(APPEND GYRO_FRAME_GRAPH "${MODULE}=${FRAME_DEPENDS}")
endforeach()
list(JOIN GYRO_FRAME_GRAPH "|" GYRO_FRAME_GRAPH)

add_custom_target(Layering ALL
	COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${GYRO_SOURCE_DIR}
		-D GRAPH=${GYRO_MODULE_GRAPH}
		-D FRAME_GRAPH=${GYRO_FRAME_GRAPH}
		-D DISPATCH_MODULES=${GYRO_DISPATCH_MODULES}
		-D DISPATCH_HALVES=${GYRO_DISPATCH_HALVES}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckLayering.cmake
	VERBATIM
)

add_executable(gyro
	Source/Main.cpp
)
add_dependencies(gyro AppVersion)
target_include_directories(gyro
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}/Generated
)
# Core is named alongside Compositor for gyro_add_module's reason: it is an OBJECT library, so
# DebugAllocator.cpp's replacement of the global allocator reaches only the targets that name it
# directly. Reached through the graph it would be dropped, and decision 36's check would be silently
# absent from the one binary that has a real frame section in it.
target_link_libraries(gyro
	PRIVATE
		Compositor
		Core
)

# Standalone diagnostics. Not linked into gyro, and deliberately dependency-free — BuildFlags
# included — so they can be built and run on a target machine on their own.
add_executable(UringProbe
	Tools/UringProbe.cpp
)

# The tests that stand on both sides of the publication waist: they publish real spring coefficients
# through Publication and reconstitute them through Animation/Solve, which no single module may do —
# Publication does not depend on Animation, by the graph. So this is a bespoke executable rather than
# a gyro_add_module, outside the per-module layering the crossing is the whole point of, and it is a
# temporary home for the round trip until Scene and Frame own their ends of it.
#
# It runs two threads, which is what makes it the target to point GYRO_SANITIZE=thread at: the
# crossing is the thing whose memory ordering has to be proved, and the soak that proves it has
# nowhere else to live.
#
# Core and Testing are named directly for the reason gyro_add_module names them: they are OBJECT
# libraries, and an object library's members reach only the targets that name it.

add_executable(IntegrationTests
	Source/Integration/PublicationRoundTrip.Test.cpp
	Source/Integration/PublicationSoak.Test.cpp
	Source/Integration/Schedulability.Test.cpp
)
target_link_libraries(IntegrationTests
	PRIVATE
		Frame
		Headless
		Publication
		Animation
		Seam
		Geometry
		Core
		Testing
		Threads::Threads
)
add_test(NAME Integration COMMAND IntegrationTests)
