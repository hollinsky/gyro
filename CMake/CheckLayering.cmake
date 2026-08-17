# The module graph is the one in Docs/Structure.md, checked on every build.
#
# CMake already enforces a dependency it can see a symbol for. Most of this codebase is headers —
# the timebase, handles, geometry, Animatable, the snapshot accessors — so most of the graph has no
# symbol to link and CMake has nothing to notice. An `#include` is the dependency here, and this is
# what holds one to what its module declared.
#
# What it is really guarding is a single edge that must not exist: the frame side reaching into the
# world. Decisions 45 and 50 give the frame thread one channel, and a stray include of Scene or
# Protocol from Frame would compile, link, and run — and would surface months later as jitter with
# no obvious cause. Every other edge here is bookkeeping; that one is the reason to have the file.
#
# Dependencies are transitive, matching PUBLIC linkage: a module may include from anything its
# declared dependencies reach. Declaring Core in every module would be noise, and the edge above is
# caught either way, because nothing on the frame side's closure leads to the world.

if(NOT DEFINED SOURCE_DIR)
	message(FATAL_ERROR "CheckLayering requires -D SOURCE_DIR=<path>")
endif()
if(NOT DEFINED GRAPH)
	message(FATAL_ERROR "CheckLayering requires -D GRAPH=<Name=Dep,Dep|Name=Dep,Dep>")
endif()

# "Name=Dep,Dep|Name=Dep,Dep" because a -P script takes no maps, and because a semicolon list handed
# to a custom command is split into separate arguments before the script ever sees it.
string(REPLACE "|" ";" GRAPH "${GRAPH}")

set(MODULES "")
foreach(ENTRY IN LISTS GRAPH)
	string(FIND "${ENTRY}" "=" SPLIT)
	if(SPLIT EQUAL -1)
		message(FATAL_ERROR "CheckLayering: malformed graph entry '${ENTRY}'")
	endif()

	string(SUBSTRING "${ENTRY}" 0 ${SPLIT} NAME)
	math(EXPR AFTER "${SPLIT} + 1")
	string(SUBSTRING "${ENTRY}" ${AFTER} -1 DEPENDS)
	string(REPLACE "," ";" DEPENDS "${DEPENDS}")

	list(APPEND MODULES ${NAME})
	set(DIRECT_${NAME} "${DEPENDS}")
endforeach()

# Transitive closure per module, by worklist. A module reaching itself is a cycle, which is worth
# failing on separately: the graph in Structure.md is a DAG, and a cycle means it stopped being one.
set(CYCLES "")
foreach(MODULE IN LISTS MODULES)
	set(REACHED "")
	set(PENDING "${DIRECT_${MODULE}}")

	while(PENDING)
		list(POP_FRONT PENDING NEXT)
		if(NEXT STREQUAL "" OR NEXT IN_LIST REACHED)
			continue()
		endif()

		list(APPEND REACHED ${NEXT})
		if(DEFINED DIRECT_${NEXT})
			list(APPEND PENDING ${DIRECT_${NEXT}})
		endif()
	endwhile()

	set(CLOSURE_${MODULE} "${REACHED}")
	if(MODULE IN_LIST REACHED)
		list(APPEND CYCLES "${MODULE}")
	endif()
endforeach()

if(CYCLES)
	list(JOIN CYCLES ", " REPORT)
	message(FATAL_ERROR "The module graph is a DAG. These modules reach themselves: ${REPORT}\n")
endif()

set(VIOLATIONS "")

foreach(MODULE IN LISTS MODULES)
	file(GLOB_RECURSE SOURCES "${SOURCE_DIR}/${MODULE}/*.cpp" "${SOURCE_DIR}/${MODULE}/*.h")

	foreach(SOURCE IN LISTS SOURCES)
		file(READ "${SOURCE}" CONTENT)
		string(REGEX REPLACE "//[^\n]*" "" CONTENT "${CONTENT}")
		string(REGEX MATCHALL "#[ \t]*include[ \t]*\"[^\"]+\"" INCLUDES "${CONTENT}")

		# A test is not part of its module's public shape, so it may reach the harness that runs it
		# without every module declaring a dependency on Testing that only its tests have.
		set(ALLOWED "${MODULE}" ${CLOSURE_${MODULE}})
		if(SOURCE MATCHES "\\.Test\\.cpp$")
			list(APPEND ALLOWED "Testing")
		endif()

		foreach(INCLUDE IN LISTS INCLUDES)
			string(REGEX REPLACE "^.*\"([^\"]+)\"$" "\\1" HEADER "${INCLUDE}")

			# Only a path with a leading component names a module. A bare header is a sibling, and
			# a component that is not a registered module is not this file's business.
			if(NOT HEADER MATCHES "/")
				continue()
			endif()

			string(REGEX REPLACE "/.*$" "" OWNER "${HEADER}")
			if(NOT OWNER IN_LIST MODULES OR OWNER IN_LIST ALLOWED)
				continue()
			endif()

			file(RELATIVE_PATH RELATIVE "${SOURCE_DIR}" "${SOURCE}")
			list(APPEND VIOLATIONS "${RELATIVE}: \"${HEADER}\" — ${MODULE} does not depend on ${OWNER}")
		endforeach()
	endforeach()
endforeach()

if(VIOLATIONS)
	list(REMOVE_DUPLICATES VIOLATIONS)
	list(JOIN VIOLATIONS "\n    " REPORT)
	message(FATAL_ERROR
		"An include crosses an edge the module graph does not have:\n    ${REPORT}\n\n"
		"Either the include is wrong, or gyro_add_module's DEPENDS is. Docs/Structure.md is the graph.\n"
	)
endif()
