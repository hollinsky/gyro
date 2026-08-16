# The timebase has exactly one reader. Enforced here rather than by review, because a reachable
# "now" is a now something eventually reaches. See Docs/Decisions.md decision 57.
#
# A grep is the only tool available: C++ cannot make std::chrono::steady_clock unreachable. Line
# comments are stripped so the prose that explains the rule does not trip it; block comments are
# not, which is reason enough to keep writing // here. This belongs in a real lint pass once one
# exists.

if(NOT DEFINED SOURCE_DIR)
	message(FATAL_ERROR "CheckClockDiscipline requires -D SOURCE_DIR=<path>")
endif()

set(READER "${SOURCE_DIR}/Core/Clock.cpp")

set(FORBIDDEN
	"clock_gettime"
	"gettimeofday"
	"steady_clock"
	"system_clock"
	"high_resolution_clock"
)

file(GLOB_RECURSE SOURCES "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.h")

set(VIOLATIONS "")
foreach(SOURCE IN LISTS SOURCES)
	if(SOURCE STREQUAL READER)
		continue()
	endif()

	file(READ "${SOURCE}" CONTENT)
	string(REGEX REPLACE "//[^\n]*" "" CONTENT "${CONTENT}")

	foreach(PATTERN IN LISTS FORBIDDEN)
		if(CONTENT MATCHES "${PATTERN}")
			file(RELATIVE_PATH RELATIVE "${SOURCE_DIR}" "${SOURCE}")
			list(APPEND VIOLATIONS "${RELATIVE}: ${PATTERN}")
		endif()
	endforeach()
endforeach()

if(VIOLATIONS)
	list(JOIN VIOLATIONS "\n    " REPORT)
	message(FATAL_ERROR "The timebase has one reader, Core/Clock.cpp. Reached elsewhere:\n    ${REPORT}\n")
endif()
