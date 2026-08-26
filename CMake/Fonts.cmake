# The bitmap font, baked at build time into the build tree.
#
# **Spleen is vendored rather than fetched**, and the reason is where gyro runs: decision 39 leaves
# running from the initramfs open, and every one of decision 38's four callers — the boot splash,
# verbose boot output, the recovery console, and the failure display — is a path that has to work on a
# machine with no network and possibly no root filesystem yet. A font reached over CPM is a font
# missing on the day somebody builds an image offline. It is 1.4 MB of text, once, under BSD 2-Clause,
# and Vendor/Spleen/LICENSE travels with it.
#
# **No generated code is ever written into the source tree**, which is CMake/Bindings.cmake's rule and
# the same one here: the baked tables land under `${CMAKE_BINARY_DIR}/Generated` because a file in
# Source/ that was produced by a build is a file somebody eventually edits, and the edit survives until
# the next clean build.
#
# **The baker is a host tool built by ExternalProject**, for Tools/Fonts/CMakeLists.txt's reason — it
# runs on the machine doing the building and gyro runs on the machine being built for. BUILD_ALWAYS and
# a dependency on the *executable* rather than on the stamp are what keep an edit to the baker from
# leaving a stale table behind, exactly as the bindings rule does.

include(ExternalProject)

set(GYRO_FONTS_BINARY_DIR ${CMAKE_BINARY_DIR}/Tools/Fonts)
set(GYRO_FONTS_TOOL ${GYRO_FONTS_BINARY_DIR}/GyroFonts)

ExternalProject_Add(FontBaker
	SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Tools/Fonts
	BINARY_DIR ${GYRO_FONTS_BINARY_DIR}
	CMAKE_ARGS
		-DCMAKE_BUILD_TYPE=Release
	BUILD_ALWAYS ON
	BUILD_BYPRODUCTS ${GYRO_FONTS_TOOL}
	INSTALL_COMMAND ""
	# It builds its own tests and the main tree runs them, which is where the corpus check lives: a
	# vendored font that arrived truncated should fail `ctest` rather than compile into a console that
	# draws boxes.
	TEST_COMMAND ""
	USES_TERMINAL_BUILD OFF
)

add_test(
	NAME FontBaker
	COMMAND ${GYRO_FONTS_BINARY_DIR}/FontBakerTests
)

# The faces, and the whole ladder in one invocation — Text/Font.h's `Nearest` walks it in order, so
# there is one array and one rule rather than one per size.
#
# Listed rather than globbed. A glob would pick up a size the moment somebody dropped a file in, which
# sounds convenient and means the binary grows by a quarter of a megabyte with nothing in the tree
# saying so; CONFIGURE_DEPENDS on top of that costs a full rebuild every build, which
# CMake/Bindings.cmake already priced and rejected.
set(GYRO_FONT_FILES
	${CMAKE_CURRENT_SOURCE_DIR}/Vendor/Spleen/spleen-5x8.bdf
	${CMAKE_CURRENT_SOURCE_DIR}/Vendor/Spleen/spleen-6x12.bdf
	${CMAKE_CURRENT_SOURCE_DIR}/Vendor/Spleen/spleen-8x16.bdf
	${CMAKE_CURRENT_SOURCE_DIR}/Vendor/Spleen/spleen-12x24.bdf
	${CMAKE_CURRENT_SOURCE_DIR}/Vendor/Spleen/spleen-16x32.bdf
	${CMAKE_CURRENT_SOURCE_DIR}/Vendor/Spleen/spleen-32x64.bdf
)

set(GYRO_FACES_SOURCE ${CMAKE_BINARY_DIR}/Generated/Text/Faces.cpp)
set(GYRO_FACES_STAMP ${CMAKE_BINARY_DIR}/Generated/Text/Faces.stamp)

# The stamp is the output and the source is a byproduct, for the reason CMake/Bindings.cmake spells
# out: the baker does not rewrite a file whose text is unchanged, and a rule whose declared output
# keeps its old timestamp is one ninja considers dirty forever.
add_custom_command(
	OUTPUT ${GYRO_FACES_STAMP}
	COMMAND ${GYRO_FONTS_TOOL} --output ${GYRO_FACES_SOURCE} ${GYRO_FONT_FILES}
	COMMAND ${CMAKE_COMMAND} -E touch ${GYRO_FACES_STAMP}
	BYPRODUCTS ${GYRO_FACES_SOURCE}
	DEPENDS
		${GYRO_FONT_FILES}
		${GYRO_FONTS_TOOL}
		FontBaker
	COMMENT "Baking the Spleen faces"
	VERBATIM
)

add_custom_target(SpleenFacesGenerate DEPENDS ${GYRO_FACES_STAMP})

# A plain library rather than a gyro_add_module, for the reason the generated bindings are one:
# CheckPortability and CheckLayering walk Source/, and neither question is asked of a file the build
# wrote. What holds this in place instead is its one edge — Geometry, for the cell size Text/Font.h
# reports — so a table that reached anywhere else would not link.
set_source_files_properties(${GYRO_FACES_SOURCE} PROPERTIES GENERATED TRUE)

add_library(SpleenFaces STATIC ${GYRO_FACES_SOURCE})
add_dependencies(SpleenFaces SpleenFacesGenerate)
target_link_libraries(SpleenFaces PUBLIC Geometry)
