# The bindings generator is a *host* tool, so it is a project of its own rather than a target in
# gyro's build.
#
# The reason is the one that always makes a code generator a separate project: it runs on the machine
# doing the building and gyro runs on the machine being built for, and a single build tree can only
# hold one toolchain. Keeping it separate means the day gyro is cross-compiled for an ARM panel is a
# day this keeps working, rather than the day somebody discovers the generator was built for the
# target and cannot be executed.
#
# It is also why Tools/ sits outside Source/ and outside CheckPortability and CheckLayering: those
# police what runs on the frame thread and what a module may name, and neither question is asked of
# a program whose whole life is between `cmake` and `ninja`.
#
#   cmake -S Tools/Bindings -B build-bindings -G Ninja && ninja -C build-bindings \
#       && ctest --test-dir build-bindings

cmake_minimum_required(VERSION 3.25)

project(GyroBindings
	DESCRIPTION "Gyro Wayland protocol bindings generator"
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The same warning set and the same -Werror as the rest of the tree. A build tool held to a looser
# standard than the code it emits is a build tool nobody trusts the output of, and BuildFlags.cmake
# is standalone apart from this one variable.
set(GYRO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(GYRO_SOURCE_DIR ${GYRO_ROOT}/Source)
include(${GYRO_ROOT}/CMake/BuildFlags.cmake)

enable_testing()

# The parser and the emitter, in one library because the tests drive both and the executable is a
# `main()` over them. Split into two targets it would be two archives with one consumer each.
add_library(BindingsParser STATIC
	Emit.Client.cpp
	Emit.Server.cpp
	Emit.Shared.cpp
	Emit.cpp
	Naming.cpp
	Xml.cpp
)
target_link_libraries(BindingsParser PUBLIC BuildFlags)
target_include_directories(BindingsParser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(GyroBindings Main.cpp)
target_link_libraries(GyroBindings PRIVATE BindingsParser)

# The published protocols, located rather than hardcoded. The generator needs them at build time
# regardless, so a machine that cannot find them cannot build gyro's bindings either — which makes
# this the right place for the message to appear.
find_package(PkgConfig REQUIRED)
pkg_get_variable(WAYLAND_DATA_DIR wayland-scanner pkgdatadir)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

if(NOT WAYLAND_DATA_DIR OR NOT WAYLAND_PROTOCOLS_DIR)
	message(FATAL_ERROR
		"wayland.xml and the protocol suite are what this parses. "
		"On Fedora: sudo dnf install wayland-devel wayland-protocols-devel"
	)
endif()

# The corpus tests read the real files rather than a copy checked in beside them, because a copy is
# a snapshot that stops testing the thing that matters: that the protocols *as this machine has
# them* parse. A protocol suite update that introduces something outside the subset should break
# this build on the day it lands, not on the day somebody remembers to refresh a fixture.
add_executable(BindingsParserTests
	Emit.Test.cpp
	Naming.Test.cpp
	Xml.Test.cpp
	${GYRO_SOURCE_DIR}/Testing/Main.cpp
	${GYRO_SOURCE_DIR}/Testing/Test.cpp
)
target_link_libraries(BindingsParserTests PRIVATE BindingsParser)
target_compile_definitions(BindingsParserTests
	PRIVATE
		GYRO_WAYLAND_DATA_DIR="${WAYLAND_DATA_DIR}"
		GYRO_WAYLAND_PROTOCOLS_DIR="${WAYLAND_PROTOCOLS_DIR}"
)

add_test(NAME BindingsParser COMMAND BindingsParserTests)
