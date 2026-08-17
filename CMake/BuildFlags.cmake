# Compiler configuration, carried by an interface target everything links.
#
# The warning set is chosen rather than inherited. gyro's two most expensive bug classes are both
# silent under a default build: an integer that narrowed or changed sign somewhere between a
# rational scale, a nanosecond count, and a device pixel; and a float that was promoted to double
# on a path that was supposed to stay single. -Wconversion and -Wdouble-promotion are here for
# exactly those, and they are the two flags most likely to be argued with — see the note on each.

add_library(BuildFlags INTERFACE)

# Every module spells its includes from the source root, so "Core/Time.h" reads the same from any
# depth and no header ever needs a relative path.
target_include_directories(BuildFlags INTERFACE ${GYRO_SOURCE_DIR})

target_compile_features(BuildFlags INTERFACE cxx_std_23)

target_compile_options(BuildFlags INTERFACE
	-Wall
	-Wextra
	-Wpedantic

	# Narrowing and sign changes. Decisions 52-57 exist because a value that rounds or wraps once
	# and is then stored is the least diagnosable defect this project can ship, and the compiler
	# already knows where every one of them is. Noisy on code that was not written for it; this
	# codebase is being written for it.
	-Wconversion
	-Wsign-conversion

	# Geometry crosses the publication boundary at double and lives at single everywhere else
	# (Architecture.md#the-spaces). An accidental promotion is a silent doubling of a hot structure.
	-Wdouble-promotion

	-Wshadow
	-Wnon-virtual-dtor
	-Woverloaded-virtual
	-Wsuggest-override
	-Wcast-qual
	-Wcast-align
	-Wuseless-cast
	-Wold-style-cast
	-Wzero-as-null-pointer-constant
	-Wextra-semi
	-Wimplicit-fallthrough
	-Wformat=2
	-Wundef

	# There are no namespaces here, so internal linkage is the only thing keeping a helper out of
	# the global name pool. This makes forgetting `static` an error rather than a collision later.
	-Wmissing-declarations

	# perf against a SCHED_FIFO process is going to be a daily tool, and a frame thread without a
	# frame pointer is a flame graph with the interesting half missing. The register is not the
	# bottleneck; the GPU is.
	-fno-omit-frame-pointer
)

# Bounds-checked libstdc++ containers and iterators. Cheap where it is on, and off in Release so
# the flag never becomes an argument about the frame budget.
target_compile_definitions(BuildFlags INTERFACE
	$<$<CONFIG:Debug,RelWithDebInfo>:_GLIBCXX_ASSERTIONS>
	$<$<CONFIG:Release,RelWithDebInfo>:_FORTIFY_SOURCE=3>
)

# Decision 36's runtime check: Core/DebugAllocator.cpp replaces the global allocator and aborts on
# an allocation made inside a FrameSection. The three checks in this directory are build-time and
# this is its counterpart, so it is configured beside them rather than inside a module.
#
# The same two configurations as the assertions above, for the same reason — Release is where a
# diagnostic must never become an argument about the frame budget — and RelWithDebInfo is the half
# that matters. The value is concentrated in the headless tests, which is what CI runs; on in local
# debug only would be on nowhere that counts. The option is here to turn it off, not on, since the
# only build entitled to skip it already does.
option(GYRO_FRAME_PATH_CHECK "Abort on a heap allocation inside a FrameSection" ON)
if(GYRO_FRAME_PATH_CHECK)
	target_compile_definitions(BuildFlags INTERFACE $<$<CONFIG:Debug,RelWithDebInfo>:GYRO_FRAME_PATH_CHECK>)
endif()

# Macro definitions in the debug info, so the test macros can be stepped into.
target_compile_options(BuildFlags INTERFACE $<$<CONFIG:Debug>:-ggdb3>)

option(GYRO_WERROR "Treat warnings as errors" ON)
if(GYRO_WERROR)
	target_compile_options(BuildFlags INTERFACE -Werror)
endif()

# Nested is what makes these usable at all — a compositor holding DRM master is not something you
# attach a sanitizer to (Architecture.md#nested-wayland). Thread is the one that matters most here:
# the publication boundary is a two-thread invariant that no test can observe directly.
set(GYRO_SANITIZE "none" CACHE STRING "Sanitizers: none, address, thread, undefined, or a ; list")
set_property(CACHE GYRO_SANITIZE PROPERTY STRINGS none address thread undefined "address;undefined" "thread;undefined")

if(NOT GYRO_SANITIZE STREQUAL "none")
	if("address" IN_LIST GYRO_SANITIZE AND "thread" IN_LIST GYRO_SANITIZE)
		message(FATAL_ERROR "GYRO_SANITIZE: address and thread cannot be combined")
	endif()

	list(JOIN GYRO_SANITIZE "," SANITIZERS)
	message(STATUS "Sanitizers: ${SANITIZERS}")

	# GCC accepts -fsanitize= whether or not the matching runtime is installed, and the failure
	# lands at link as "cannot find libtsan.so" — which reads as a broken toolchain rather than as a
	# missing package. Caught here instead, where the message can say what to install.
	include(CheckCXXSourceCompiles)
	string(MAKE_C_IDENTIFIER "${SANITIZERS}" SANITIZER_KEY)
	set(CMAKE_REQUIRED_FLAGS -fsanitize=${SANITIZERS})
	set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=${SANITIZERS})
	check_cxx_source_compiles("int main() { return 0; }" GYRO_SANITIZER_RUNTIME_${SANITIZER_KEY})

	if(NOT GYRO_SANITIZER_RUNTIME_${SANITIZER_KEY})
		message(FATAL_ERROR
			"GYRO_SANITIZE=${SANITIZERS} but its runtime does not link. "
			"On Fedora: sudo dnf install libasan libubsan libtsan"
		)
	endif()

	# Both halves, and the link line too — a sanitizer configured only for compilation fails at link
	# in a way that reads as a missing library.
	target_compile_options(BuildFlags INTERFACE -fsanitize=${SANITIZERS} -fno-sanitize-recover=all)
	target_link_options(BuildFlags INTERFACE -fsanitize=${SANITIZERS})
endif()

option(GYRO_LTO "Link-time optimization" OFF)
if(GYRO_LTO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
	if(IPO_SUPPORTED)
		set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON PARENT_SCOPE)
	else()
		message(WARNING "GYRO_LTO requested but unavailable: ${IPO_ERROR}")
	endif()
endif()
