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
		Quiet.Test.cpp
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
		Solve/Spring.Test.cpp
		Author/Animatable.Test.cpp
		Author/Bundle.Test.cpp
		Author/Catalog.Test.cpp
		Author/Drive.Test.cpp
		Author/Motion.Test.cpp
		Author/Retarget.Test.cpp
)

# The data waist: what crosses between the two threads. This cut is the snapshot representation and
# its wait-free reader; the SPSC ring, the return channel, the watermark, and the per-buffer hold are
# later parts of the same module. Split by direction like Animation is: Reader is the frame half and
# is the default, Publisher is the dispatch half that serialises and allocates.
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
		Snapshot.Test.cpp
		Reader/Reader.Test.cpp
		Publisher/Publisher.Test.cpp
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

# The one test that stands on both sides of the publication waist: it publishes real spring
# coefficients through Publication and reconstitutes them through Animation/Solve, which no single
# module may do — Publication does not depend on Animation, by the graph. So it is a bespoke
# executable rather than a gyro_add_module, outside the per-module layering the crossing is the whole
# point of, and it is a temporary home for the round trip until Scene and Frame own their ends of it.
# Core and Testing are named directly for the reason gyro_add_module names them: they are OBJECT
# libraries, and an object library's members reach only the targets that name it.
add_executable(IntegrationTests
	Source/Integration/PublicationRoundTrip.Test.cpp
)
target_link_libraries(IntegrationTests
	PRIVATE
		Publication
		Animation
		Geometry
		Core
		Testing
)
add_test(NAME Integration COMMAND IntegrationTests)
