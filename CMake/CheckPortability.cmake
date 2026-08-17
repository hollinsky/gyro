# The portable tier is portable, checked on every build.
#
# Decision 6 keeps Core, Scene, and Animation free of Linux-only and platform-stack headers so that
# their tests build and run anywhere — which is worth having because it is the tier where the
# springs, the differ, the geometry, and the timebase live, and none of those needs a display to be
# wrong. The property is easy to state and easy to lose to one convenient include.
#
# POSIX is allowed. Core/Clock.cpp reads clock_gettime, which is POSIX rather than Linux, and the
# distinction is the whole point: "builds anywhere" means anywhere with a C library, not anywhere
# with a kernel we like.
#
# A grep is the only tool available short of a second toolchain. Line comments are stripped so the
# prose explaining a rule does not trip it, which is the same convention CheckClockDiscipline.cmake
# established and the same reason to keep writing // rather than /* */.

if(NOT DEFINED SOURCE_DIR)
	message(FATAL_ERROR "CheckPortability requires -D SOURCE_DIR=<path>")
endif()
if(NOT DEFINED MODULES)
	message(FATAL_ERROR "CheckPortability requires -D MODULES=<list>")
endif()

# Matched against the text between the angle brackets.
set(FORBIDDEN
	"^linux/"
	"^asm/"
	"^asm-generic/"
	"^sys/(epoll|eventfd|timerfd|signalfd|inotify|prctl|syscall|sysinfo|memfd|personality|sendfile|vfs|sysmacros)\\.h$"
	"^liburing"
	"^xf86drm"
	"^(drm|libdrm)/"
	"^libinput\\.h$"
	"^libudev\\.h$"
	"^xkbcommon/"
	"^wayland-"
	"^vulkan/"
	"^volk"
	"^shaderc/"
	"^GL/"
)

set(VIOLATIONS "")

foreach(MODULE IN LISTS MODULES)
	file(GLOB_RECURSE SOURCES "${SOURCE_DIR}/${MODULE}/*.cpp" "${SOURCE_DIR}/${MODULE}/*.h")

	foreach(SOURCE IN LISTS SOURCES)
		file(READ "${SOURCE}" CONTENT)
		string(REGEX REPLACE "//[^\n]*" "" CONTENT "${CONTENT}")
		string(REGEX MATCHALL "#[ \t]*include[ \t]*<[^>]+>" INCLUDES "${CONTENT}")

		foreach(INCLUDE IN LISTS INCLUDES)
			string(REGEX REPLACE "^.*<([^>]+)>$" "\\1" HEADER "${INCLUDE}")

			foreach(PATTERN IN LISTS FORBIDDEN)
				if(HEADER MATCHES "${PATTERN}")
					file(RELATIVE_PATH RELATIVE "${SOURCE_DIR}" "${SOURCE}")
					list(APPEND VIOLATIONS "${RELATIVE}: <${HEADER}>")
				endif()
			endforeach()
		endforeach()
	endforeach()
endforeach()

if(VIOLATIONS)
	list(REMOVE_DUPLICATES VIOLATIONS)
	list(JOIN VIOLATIONS "\n    " REPORT)
	message(FATAL_ERROR
		"The portable tier builds anywhere: ${MODULES}. Platform headers reached:\n    ${REPORT}\n"
	)
endif()
