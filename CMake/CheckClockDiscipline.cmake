# The timebase has exactly one reader, and its arithmetic has exactly one home. Two rules, both
# enforced here rather than by review, because a reachable "now" is a now something eventually
# reaches and a reachable raw count is arithmetic somebody eventually hand-rolls. See
# Docs/Decisions.md decision 57.
#
# A grep is the only tool available: C++ cannot make std::chrono::steady_clock unreachable, and
# Instant is an alias whose operators cannot be replaced. Line comments are stripped so the prose
# that explains the rules does not trip them; block comments are not, which is reason enough to keep
# writing // here. This belongs in a real lint pass once one exists.

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

# The second rule. Reaching through an Instant to the integer underneath is how the timebase's
# arithmetic ends up hand-rolled beside whichever caller needed it first: an unguarded subtraction or
# addition on a raw count, in a file whose subject is something else, each one guarding whichever
# direction that caller happened to think about. Core/Time.h owns the total forms — Elapsed and
# Advanced — so it owns the escape hatch they are built from.
#
# The operators themselves are deliberately absent from this list. Instant - Instant is the ordinary
# spelling for a pair the caller bounds, which is most of them; it is the count that is not ordinary.
set(ARITHMETIC "${SOURCE_DIR}/Core/Time.h")

set(RESERVED
	"time_since_epoch"
)

file(GLOB_RECURSE SOURCES "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.h")

set(READERS "")
set(REACHES "")
foreach(SOURCE IN LISTS SOURCES)
	file(READ "${SOURCE}" CONTENT)
	string(REGEX REPLACE "//[^\n]*" "" CONTENT "${CONTENT}")
	file(RELATIVE_PATH RELATIVE "${SOURCE_DIR}" "${SOURCE}")

	if(NOT SOURCE STREQUAL READER)
		foreach(PATTERN IN LISTS FORBIDDEN)
			if(CONTENT MATCHES "${PATTERN}")
				list(APPEND READERS "${RELATIVE}: ${PATTERN}")
			endif()
		endforeach()
	endif()

	if(NOT SOURCE STREQUAL ARITHMETIC)
		foreach(PATTERN IN LISTS RESERVED)
			if(CONTENT MATCHES "${PATTERN}")
				list(APPEND REACHES "${RELATIVE}: ${PATTERN}")
			endif()
		endforeach()
	endif()
endforeach()

if(READERS)
	list(JOIN READERS "\n    " REPORT)
	message(FATAL_ERROR "The timebase has one reader, Core/Clock.cpp. Reached elsewhere:\n    ${REPORT}\n")
endif()

if(REACHES)
	list(JOIN REACHES "\n    " REPORT)
	message(FATAL_ERROR
		"The timebase's arithmetic lives in Core/Time.h — Elapsed and Advanced are the total forms. "
		"Reached through elsewhere:\n    ${REPORT}\n"
	)
endif()
