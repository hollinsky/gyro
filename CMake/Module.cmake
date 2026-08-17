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

function(gyro_add_module NAME)
	cmake_parse_arguments(PARSE_ARGV 1 MODULE "PORTABLE" "" "SOURCES;TESTS;DEPENDS")

	if(MODULE_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "gyro_add_module(${NAME}): unexpected argument ${MODULE_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT MODULE_SOURCES)
		message(FATAL_ERROR "gyro_add_module(${NAME}): SOURCES is required")
	endif()

	# Sources are named relative to the module, because a module listing another module's files is
	# a layering mistake worth having to spell out.
	list(TRANSFORM MODULE_SOURCES PREPEND "${GYRO_SOURCE_DIR}/${NAME}/")

	add_library(${NAME} STATIC ${MODULE_SOURCES})
	target_link_libraries(${NAME} PUBLIC BuildFlags ${MODULE_DEPENDS})

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
