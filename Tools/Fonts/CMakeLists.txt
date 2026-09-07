# The font baker is a *host* tool, so it is a project of its own rather than a target in gyro's build —
# Tools/Bindings/CMakeLists.txt has the argument, and it is the same one: it runs on the machine doing
# the building and gyro runs on the machine being built for, and one build tree holds one toolchain.
#
# What it does is turn the BDF files in Vendor/Spleen into one C++ translation unit of glyph tables.
# Decision 38 wants an embedded bitmap font, and embedding one is either this or a blob checked into
# the tree beside a script nobody runs again.
#
#   cmake -S Tools/Fonts -B build-fonts -G Ninja && ninja -C build-fonts && ctest --test-dir build-fonts

cmake_minimum_required(VERSION 3.25)

project(GyroFonts
	DESCRIPTION "Gyro bitmap font baker"
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The same warning set and the same -Werror as the rest of the tree, for Tools/Bindings' reason: a
# build tool held to a looser standard than the code it emits is one nobody trusts the output of.
set(GYRO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(GYRO_SOURCE_DIR ${GYRO_ROOT}/Source)
include(${GYRO_ROOT}/CMake/BuildFlags.cmake)

enable_testing()

add_library(FontBaker STATIC
	Bake.cpp
	Bdf.cpp
)
target_link_libraries(FontBaker PUBLIC BuildFlags)
target_include_directories(FontBaker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(GyroFonts Main.cpp)
target_link_libraries(GyroFonts PRIVATE FontBaker)

# Installed for Tools/Bindings' reason and by the same arrangement: a cross build takes its baker from
# a host build of this project through `GYRO_HOST_FONTS`, because a baker compiled for the target is
# one the build machine cannot run.
include(GNUInstallDirs)
install(TARGETS GyroFonts RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# **The tests are optional, and the one caller that turns them off is a
# distribution.** They compile Source/Testing, which is written against the same
# C++23 the compositor is, while this project is built by whatever compiler the
# *build host* has -- and a build host is routinely several releases behind the
# cross toolchain the image is built with. Yocto's Ubuntu 24.04 host is GCC 13,
# which has no <print>; the tool itself compiles there and only the tests do not.
#
# **Rejected: lowering what the tests may use to the oldest host worth
# supporting.** That is a standard-library subset nothing enforces, so it would
# be rediscovered one header at a time by whoever next builds an image.
option(GYRO_TOOL_TESTS "Build this tool's own tests; off for a build host older than the tree" ON)

if(GYRO_TOOL_TESTS)

# The corpus test reads Vendor/Spleen as this checkout has it rather than a fixture beside it, which is
# the rule Tools/Bindings' parser tests already follow: a copy is a snapshot that stops testing the
# thing that matters.
add_executable(FontBakerTests
	Bake.Test.cpp
	Bdf.Test.cpp
	${GYRO_SOURCE_DIR}/Testing/Main.cpp
	${GYRO_SOURCE_DIR}/Testing/Test.cpp
)
target_link_libraries(FontBakerTests PRIVATE FontBaker)
target_compile_definitions(FontBakerTests PRIVATE GYRO_FONT_DIR="${GYRO_ROOT}/Vendor/Spleen")

add_test(NAME FontBaker COMMAND FontBakerTests)

endif()
