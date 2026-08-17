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
)

# spdlog
CPMAddPackage(
	NAME spdlog
	GITHUB_REPOSITORY gabime/spdlog
	GIT_TAG v1.17.0
	SYSTEM YES
	EXCLUDE_FROM_ALL YES
)

gyro_add_module(Core PORTABLE
	SOURCES
		Clock.cpp
)

# Declared after the modules, because the module list is what it checks. ALL for the same reason as
# the clock check above.
get_property(GYRO_PORTABLE_MODULES GLOBAL PROPERTY GYRO_PORTABLE_MODULES)
add_custom_target(Portability ALL
	COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${GYRO_SOURCE_DIR}
		-D "MODULES=${GYRO_PORTABLE_MODULES}"
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckPortability.cmake
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
