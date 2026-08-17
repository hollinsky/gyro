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
#
# The second half of the file guards the same edge where the graph cannot see it. Four modules
# straddle the publication boundary (Docs/Structure.md#threads-are-a-second-partition), and inside
# one of those the crossing include is one the graph calls legal — Frame depends on Animation, so
# Frame reaching Animation::Author is a boundary crossing along a declared edge. What makes that
# mechanical rather than a matter of reading the contents is decision 50: producing spring
# coefficients is dispatch-side and consuming them is not, so the halves split by *direction*, the
# split is a directory, and the rule is a path prefix.
#
# Deny by default, and only the dispatch half is named. The frame side is what is being protected,
# so it is the default and the exception is what gets declared — a module that has said nothing
# cannot reach authoring, which is the polarity that survives someone adding a module and not
# reading this file.
#
# The third half of the file is the same edge seen from the other end. A straddling module's two
# halves do not need the same dependencies, and the one that matters is the frame half: Animation
# depends on Geometry for a single member of the gesture mapping, which is authoring, so the solver
# the frame thread calls has no business with it. That is Docs/Structure.md's general shape rather
# than one module's quirk — a dependency that arrives with authoring stops at the publication
# boundary — and without FRAME_DEPENDS it is a sentence in a document while the build permits the
# opposite. Declaring it is optional and silence means the old behaviour, because a module whose
# halves genuinely want the same edges should not have to say so twice.

if(NOT DEFINED SOURCE_DIR)
	message(FATAL_ERROR "CheckLayering requires -D SOURCE_DIR=<path>")
endif()
if(NOT DEFINED GRAPH)
	message(FATAL_ERROR "CheckLayering requires -D GRAPH=<Name=Dep,Dep|Name=Dep,Dep>")
endif()

# Both are empty until a straddler declares itself, which is the ordinary state of this file until
# Publication and Render exist.
foreach(OPTIONAL IN ITEMS DISPATCH_MODULES DISPATCH_HALVES)
	if(NOT DEFINED ${OPTIONAL})
		set(${OPTIONAL} "")
	endif()
	string(REPLACE "," ";" ${OPTIONAL} "${${OPTIONAL}}")
endforeach()

# "Name=Dep,Dep|Name=Dep,Dep" because a -P script takes no maps, and because a semicolon list handed
# to a custom command is split into separate arguments before the script ever sees it. FRAME_GRAPH is
# the same shape and is empty until a straddling module narrows its frame half.
string(REPLACE "|" ";" GRAPH "${GRAPH}")

if(NOT DEFINED FRAME_GRAPH)
	set(FRAME_GRAPH "")
endif()
string(REPLACE "|" ";" FRAME_GRAPH "${FRAME_GRAPH}")

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

# What each narrowed frame half may reach, closed over the same graph. A frame dependency has to be
# one the module already declares, so this is a subset of a closure that is already computed rather
# than a second graph that could disagree with the first.
foreach(ENTRY IN LISTS FRAME_GRAPH)
	string(FIND "${ENTRY}" "=" SPLIT)
	if(SPLIT EQUAL -1)
		message(FATAL_ERROR "CheckLayering: malformed frame graph entry '${ENTRY}'")
	endif()

	string(SUBSTRING "${ENTRY}" 0 ${SPLIT} NAME)
	math(EXPR AFTER "${SPLIT} + 1")
	string(SUBSTRING "${ENTRY}" ${AFTER} -1 DEPENDS)
	string(REPLACE "," ";" DEPENDS "${DEPENDS}")

	set(REACHED "")
	foreach(DEPENDENCY IN LISTS DEPENDS)
		if(DEPENDENCY STREQUAL "")
			continue()
		endif()

		list(APPEND REACHED ${DEPENDENCY} ${CLOSURE_${DEPENDENCY}})
	endforeach()

	list(REMOVE_DUPLICATES REACHED)
	set(FRAME_CLOSURE_${NAME} "${REACHED}")
	set(FRAME_LIMITED_${NAME} TRUE)
endforeach()

set(VIOLATIONS "")

foreach(MODULE IN LISTS MODULES)
	file(GLOB_RECURSE SOURCES "${SOURCE_DIR}/${MODULE}/*.cpp" "${SOURCE_DIR}/${MODULE}/*.h")

	foreach(SOURCE IN LISTS SOURCES)
		file(READ "${SOURCE}" CONTENT)
		string(REGEX REPLACE "//[^\n]*" "" CONTENT "${CONTENT}")
		string(REGEX MATCHALL "#[ \t]*include[ \t]*\"[^\"]+\"" INCLUDES "${CONTENT}")

		file(RELATIVE_PATH RELATIVE "${SOURCE_DIR}" "${SOURCE}")

		# A file is dispatch-side by where it sits, tests included. That is deliberate: a test for
		# something in a dispatch half belongs in that half, and one written at the module root
		# fails here rather than quietly establishing that the root may reach authoring.
		set(DISPATCH_SIDE FALSE)
		if(MODULE IN_LIST DISPATCH_MODULES)
			set(DISPATCH_SIDE TRUE)
		endif()
		foreach(HALF IN LISTS DISPATCH_HALVES)
			if(RELATIVE MATCHES "^${HALF}/")
				set(DISPATCH_SIDE TRUE)
			endif()
		endforeach()

		# A test is not part of its module's public shape, so it may reach the harness that runs it
		# without every module declaring a dependency on Testing that only its tests have.
		#
		# The frame half of a narrowed module gets the narrower allowance, and its tests get it too:
		# a test that may include what the code beside it may not is a test that stops being able to
		# fail on the thing this rule exists for.
		set(ALLOWED "${MODULE}" ${CLOSURE_${MODULE}})
		if(NOT DISPATCH_SIDE AND FRAME_LIMITED_${MODULE})
			set(ALLOWED "${MODULE}" ${FRAME_CLOSURE_${MODULE}})
		endif()
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

			if(NOT DISPATCH_SIDE)
				foreach(HALF IN LISTS DISPATCH_HALVES)
					if(HEADER MATCHES "^${HALF}/")
						list(APPEND VIOLATIONS
							"${RELATIVE}: \"${HEADER}\" — ${HALF} is dispatch-side and this is not")
					endif()
				endforeach()
			endif()

			string(REGEX REPLACE "/.*$" "" OWNER "${HEADER}")
			if(NOT OWNER IN_LIST MODULES OR OWNER IN_LIST ALLOWED)
				continue()
			endif()

			# Two different mistakes, and the report says which. An edge the module does not have at
			# all is a graph question; an edge it has but only for its dispatch half is a boundary
			# question, and telling someone to add a DEPENDS it already declared is how a check
			# teaches the wrong lesson.
			if(OWNER IN_LIST CLOSURE_${MODULE})
				list(APPEND VIOLATIONS
					"${RELATIVE}: \"${HEADER}\" — ${OWNER} arrives with ${MODULE}'s dispatch half and stops there")
			else()
				list(APPEND VIOLATIONS "${RELATIVE}: \"${HEADER}\" — ${MODULE} does not depend on ${OWNER}")
			endif()
		endforeach()
	endforeach()
endforeach()

if(VIOLATIONS)
	list(REMOVE_DUPLICATES VIOLATIONS)
	list(JOIN VIOLATIONS "\n    " REPORT)
	message(FATAL_ERROR
		"An include crosses a line the module graph draws:\n    ${REPORT}\n\n"
		"Either the include is wrong, or gyro_add_module's DEPENDS, DISPATCH, DISPATCH_HALF, or\n"
		"FRAME_DEPENDS is.\n"
		"Docs/Structure.md is the graph, and its thread table is the second partition.\n"
	)
endif()
