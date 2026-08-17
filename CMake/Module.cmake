# Modules, and the tier they belong to.
#
# There are two tiers and the split is decision 6's. Core, Scene, and Animation are PORTABLE: ISO
# C++ and POSIX, no Linux-only headers and nothing from the platform stack, so their tests build
# and run on a machine with no GPU, no seat, and no compositor. Everything that touches DRM,
# libinput, Vulkan, io_uring, or the wire protocol is platform code and simply does not say
# PORTABLE.
#
# The keyword is not documentation. Declaring it enrolls the module in CheckPortability.cmake, so
# the tier is a build failure rather than a convention that erodes the first time something is
# convenient — the same standing CheckClockDiscipline.cmake gives the timebase.
#
# DEPENDS has the same standing. It is the module graph in Docs/Structure.md, and
# CheckLayering.cmake holds every #include in the module to it — because CMake enforces a link
# dependency and this codebase is mostly headers, where there is no symbol to link and therefore
# nothing for CMake to notice.

function(gyro_add_module NAME)
	cmake_parse_arguments(PARSE_ARGV 1 MODULE "PORTABLE;OBJECT" "" "SOURCES;TESTS;DEPENDS")

	if(MODULE_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "gyro_add_module(${NAME}): unexpected argument ${MODULE_UNPARSED_ARGUMENTS}")
	endif()
	if(MODULE_OBJECT AND NOT MODULE_SOURCES)
		message(FATAL_ERROR "gyro_add_module(${NAME}): OBJECT needs SOURCES to hold the objects")
	endif()

	# A module with no translation unit of its own is header-only, and it is an INTERFACE library
	# because a static archive needs something to archive. Most of this codebase is headed that way —
	# geometry, Animatable, the snapshot accessors — so this is the ordinary case rather than the
	# exception, and the tests are then the only thing that ever compiles the module. That makes a
	# rule worth having true by construction rather than by habit: a header with no test is a header
	# nothing has compiled. Requiring TESTS here is what stops it being a coincidence.
	if(NOT MODULE_SOURCES AND NOT MODULE_TESTS)
		message(FATAL_ERROR "gyro_add_module(${NAME}): a module with no SOURCES is compiled only by its TESTS")
	endif()

	# Sources are named relative to the module, because a module listing another module's files is
	# a layering mistake worth having to spell out.
	list(TRANSFORM MODULE_SOURCES PREPEND "${GYRO_SOURCE_DIR}/${NAME}/")

	# OBJECT is for a module something links but nothing references by symbol — the test harness,
	# whose main() a static archive would leave on the shelf. The scope keyword travels with the
	# library kind, since an INTERFACE target has no private half to describe.
	if(MODULE_OBJECT)
		add_library(${NAME} OBJECT ${MODULE_SOURCES})
		set(SCOPE PUBLIC)
	elseif(MODULE_SOURCES)
		add_library(${NAME} STATIC ${MODULE_SOURCES})
		set(SCOPE PUBLIC)
	else()
		add_library(${NAME} INTERFACE)
		set(SCOPE INTERFACE)
	endif()

	target_link_libraries(${NAME} ${SCOPE} BuildFlags ${MODULE_DEPENDS})

	set_property(GLOBAL APPEND PROPERTY GYRO_MODULES ${NAME})
	set_property(GLOBAL PROPERTY GYRO_MODULE_DEPENDS_${NAME} "${MODULE_DEPENDS}")

	if(MODULE_PORTABLE)
		set_property(GLOBAL APPEND PROPERTY GYRO_PORTABLE_MODULES ${NAME})
	endif()

	# One test binary per module, so a Core change relinks Core's tests and nothing else, and so the
	# portable tier's tests stay runnable on their own.
	if(MODULE_TESTS)
		list(TRANSFORM MODULE_TESTS PREPEND "${GYRO_SOURCE_DIR}/${NAME}/")

		add_executable(${NAME}Tests ${MODULE_TESTS})
		target_link_libraries(${NAME}Tests PRIVATE ${NAME} Testing)

		# CTest sees one entry per module rather than one per case. The runner prints and filters
		# per case, which is what a human wants; splitting it for CI means teaching CMake to
		# enumerate tests it cannot see, and the report is no better for it.
		add_test(NAME ${NAME} COMMAND ${NAME}Tests)
	endif()
endfunction()
