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

# spdlog
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
	TESTS
		Clock.Test.cpp
		FrameSection.Test.cpp
		Handle.Test.cpp
		SlotAllocator.Test.cpp
		Time.Test.cpp
)

# Header-only, so the tests are the only thing that compiles it. See Docs/Structure.md, both for why
# Geometry is not part of Core and for what that arrangement makes true.
gyro_add_module(Geometry PORTABLE
	DEPENDS
		Core
	TESTS
		AxisTransform.Test.cpp
		NodeTransform.Test.cpp
		Scale.Test.cpp
		Space.Test.cpp
)

# Split by direction rather than by tier: Solve consumes spring coefficients and is what the frame
# thread may call, Author produces them and is dispatch-side. Docs/Structure.md's module graph gives
# Animation an edge to Geometry as well, which the solver does not need — settling thresholds arrive
# as arguments precisely so that neither the geometric ones nor the unresolved non-geometric ones
# have to be known here. The edge lands when the catalog does.
gyro_add_module(Animation PORTABLE
	DEPENDS
		Core
	TESTS
		Solve/Spring.Test.cpp
		Author/Retarget.Test.cpp
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

add_custom_target(Layering ALL
	COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${GYRO_SOURCE_DIR}
		-D GRAPH=${GYRO_MODULE_GRAPH}
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
target_link_libraries(gyro
	PRIVATE
		Core
		spdlog::spdlog
)

# Standalone diagnostics. Not linked into gyro, and deliberately dependency-free — BuildFlags
# included — so they can be built and run on a target machine on their own.
add_executable(UringProbe
	Tools/UringProbe.cpp
)
