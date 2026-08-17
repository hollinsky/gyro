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
#
# DISPATCH and DISPATCH_HALF are the second partition, and they exist because the graph cannot see
# it. Docs/Structure.md#threads-are-a-second-partition names four modules that straddle the
# publication boundary, and for those the dangerous include is one the graph calls legal: Frame
# depends on Animation, so Frame reaching Animation::Author crosses the boundary without crossing an
# edge. DISPATCH_HALF names the subdirectories that are dispatch-side, DISPATCH says the whole module
# is, and everything else is denied — the frame side is the default because it is the side being
# protected, and a new module has to say it is dispatch-side rather than say it is not.
#
# FRAME_DEPENDS is that partition applied to the module's own edges, and it is what makes "a
# dependency that arrives with authoring stops at the publication boundary" a build failure rather
# than a paragraph. Animation depends on Geometry for one member of the gesture mapping, which is
# authoring; the solver the frame thread calls is scalar throughout and must not acquire it. Naming
# the subset the frame half may reach is what holds that. Optional, and silence means both halves get
# DEPENDS: a module whose halves want the same edges should not have to say so twice, and the check
# it turns on is worth having exactly where somebody has a reason to claim it.

function(gyro_add_module NAME)
	cmake_parse_arguments(
		PARSE_ARGV 1 MODULE "PORTABLE;OBJECT;DISPATCH" "" "SOURCES;TESTS;DEPENDS;DISPATCH_HALF;FRAME_DEPENDS"
	)

	if(MODULE_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "gyro_add_module(${NAME}): unexpected argument ${MODULE_UNPARSED_ARGUMENTS}")
	endif()
	if(MODULE_OBJECT AND NOT MODULE_SOURCES)
		message(FATAL_ERROR "gyro_add_module(${NAME}): OBJECT needs SOURCES to hold the objects")
	endif()
	if(MODULE_DISPATCH AND MODULE_DISPATCH_HALF)
		message(FATAL_ERROR "gyro_add_module(${NAME}): DISPATCH is the whole module; it has no half to name")
	endif()

	# A module with no dispatch half has no frame half to narrow, and one whose frame half may reach
	# something the module itself does not is declaring an edge in the wrong place. Both are caught
	# here rather than in the check, since a graph that never had the edge cannot report losing it.
	if(MODULE_FRAME_DEPENDS AND NOT MODULE_DISPATCH_HALF)
		message(FATAL_ERROR "gyro_add_module(${NAME}): FRAME_DEPENDS narrows a DISPATCH_HALF, and there is none")
	endif()
	foreach(FRAME_DEPENDENCY IN LISTS MODULE_FRAME_DEPENDS)
		if(NOT FRAME_DEPENDENCY IN_LIST MODULE_DEPENDS)
			message(FATAL_ERROR "gyro_add_module(${NAME}): FRAME_DEPENDS ${FRAME_DEPENDENCY} is not in DEPENDS")
		endif()
	endforeach()

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

	if(MODULE_FRAME_DEPENDS)
		set_property(GLOBAL APPEND PROPERTY GYRO_FRAME_LIMITED_MODULES ${NAME})
		set_property(GLOBAL PROPERTY GYRO_MODULE_FRAME_DEPENDS_${NAME} "${MODULE_FRAME_DEPENDS}")
	endif()

	if(MODULE_PORTABLE)
		set_property(GLOBAL APPEND PROPERTY GYRO_PORTABLE_MODULES ${NAME})
	endif()

	if(MODULE_DISPATCH)
		set_property(GLOBAL APPEND PROPERTY GYRO_DISPATCH_MODULES ${NAME})
	endif()

	# Halves are recorded module-relative, since that is the form an #include is written in and the
	# check should not have to reconstruct it. A misspelled half is the failure worth catching here:
	# it names no directory, matches no include, and silently turns the rule off for the one module
	# that asked for it.
	foreach(HALF IN LISTS MODULE_DISPATCH_HALF)
		if(NOT IS_DIRECTORY "${GYRO_SOURCE_DIR}/${NAME}/${HALF}")
			message(FATAL_ERROR "gyro_add_module(${NAME}): DISPATCH_HALF ${HALF} is not a directory")
		endif()
		set_property(GLOBAL APPEND PROPERTY GYRO_DISPATCH_HALVES "${NAME}/${HALF}")
	endforeach()

	# One test binary per module, so a Core change relinks Core's tests and nothing else, and so the
	# portable tier's tests stay runnable on their own.
	if(MODULE_TESTS)
		list(TRANSFORM MODULE_TESTS PREPEND "${GYRO_SOURCE_DIR}/${NAME}/")

		add_executable(${NAME}Tests ${MODULE_TESTS})
		# Core is named even though every module already depends on it, and the redundancy is
		# load-bearing. Core is an OBJECT library so that Core/DebugAllocator.cpp's replacement of
		# the global allocator reaches the binaries that link it, and CMake adds an object
		# library's files only to the targets that name it *directly* — reached through the
		# INTERFACE library a header-only module becomes, the objects are dropped and decision 36's
		# check is silently absent from that test binary. Naming Core here makes it uniform across
		# every test executable rather than an accident of which modules happen to have sources.
		target_link_libraries(${NAME}Tests PRIVATE ${NAME} Core Testing)

		# CTest sees one entry per module rather than one per case. The runner prints and filters
		# per case, which is what a human wants; splitting it for CI means teaching CMake to
		# enumerate tests it cannot see, and the report is no better for it.
		add_test(NAME ${NAME} COMMAND ${NAME}Tests)
	endif()
endfunction()
