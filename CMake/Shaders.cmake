# GLSL compiled to SPIR-V at build time, embedded in the binary as an array.
#
# **The compiler is a build tool and is not linked into gyro**, which is the whole point of the rule
# below. Decision 62 fuses effect chains into pipeline variants and compiles a missing one off the
# frame path, and the variant set is enumerable — decisions 103 and 110 count it — so every module
# gyro can ever need exists before it boots. What happens at runtime is `vkCreateGraphicsPipelines` against a
# module that is already there, not a front end turning text into SPIR-V. A compositor that linked
# glslang would carry it in a process that calls `mlockall(MCL_CURRENT | MCL_FUTURE)`, which is a
# compiler pinned into RAM for the life of the machine on every machine.
#
# **Embedded rather than loaded from a file**, because gyro is a boot service. Decision 41 has it
# running before the real driver has loaded, from an initramfs that may hold nothing but the binary,
# and a shader that has to be found on a filesystem is a black screen with a path in a log nobody can
# read yet. The array is in the text the loader already mapped.
#
# **glslang rather than shaderc**, which is the incumbent name in Docs/Architecture.md's dependency
# table and in decision 9. shaderc is a *library* wrapper whose value is the runtime compilation this
# file's first paragraph declines, and taking it costs SPIRV-Tools, SPIRV-Headers and glslang
# underneath it. Asked for the one thing wanted here — a `.vert` in and a header out — `glslang -V`
# is the same SPIR-V from one package, with `ENABLE_OPT` off so the optimizer's dependency never
# arrives. It is pinned to the SDK Vulkan-Headers and volk are pinned to, for their reason: a front
# end that generates for one header revision and a dispatch table built from another disagree about
# what a structure contains.

# The stage a file extension names, spelled the way the generated symbol should read. A file whose
# extension is not here is a fatal error rather than a stage glslang would have inferred, because
# the inference is what would silently compile a compute shader as a fragment one.
set(GYRO_SHADER_STAGE_vert Vertex)
set(GYRO_SHADER_STAGE_frag Fragment)
set(GYRO_SHADER_STAGE_comp Compute)

# One module's shaders, compiled into `<Module>/Shaders/<Name>.<stage>.h` under the generated root
# and reachable as `#include "<Module>/Shaders/<Name>.<stage>.h"`. `Quad.vert` declares
# `QuadVertexSpirv`; the symbol is derived rather than named so that a header and the array in it
# cannot come apart.
function(gyro_add_shaders MODULE)
	cmake_parse_arguments(PARSE_ARGV 1 SHADER "" "" "SOURCES")

	if(SHADER_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "gyro_add_shaders(${MODULE}): unexpected argument ${SHADER_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT SHADER_SOURCES)
		message(FATAL_ERROR "gyro_add_shaders(${MODULE}): no SOURCES")
	endif()

	set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/Generated")
	set(OUTPUTS "")

	foreach(SOURCE IN LISTS SHADER_SOURCES)
		get_filename_component(NAME "${SOURCE}" NAME_WE)
		get_filename_component(STAGE "${SOURCE}" LAST_EXT)
		string(SUBSTRING "${STAGE}" 1 -1 STAGE)

		if(NOT DEFINED GYRO_SHADER_STAGE_${STAGE})
			message(FATAL_ERROR "gyro_add_shaders(${MODULE}): ${SOURCE} names no stage this file knows")
		endif()

		set(INPUT "${GYRO_SOURCE_DIR}/${MODULE}/Shaders/${SOURCE}")
		set(OUTPUT "${GENERATED}/${MODULE}/Shaders/${SOURCE}.h")

		add_custom_command(
			OUTPUT "${OUTPUT}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED}/${MODULE}/Shaders"
			# vulkan1.3 because Render/Device.cpp refuses anything older, and the target environment
			# is what decides whether a construct the device requires is even legal to emit.
			COMMAND $<TARGET_FILE:glslang-standalone>
				-V
				--target-env vulkan1.3
				--quiet
				# The module's own shader directory, which is what makes a shared `.glsl` header
				# reachable from a `#include` — see DEPFILE below for the half of that which is not
				# optional. Joined to the flag rather than a separate argument because glslang says
				# *include path must immediately follow option* and exits, which under `VERBATIM` is
				# the difference between a build and a one-line error nobody reads twice.
				-I${GYRO_SOURCE_DIR}/${MODULE}/Shaders
				--depfile "${OUTPUT}.d"
				--vn "${NAME}${GYRO_SHADER_STAGE_${STAGE}}Spirv"
				-o "${OUTPUT}"
				"${INPUT}"
			DEPENDS "${INPUT}" glslang-standalone
			# **Without this an edit to an included header rebuilds nothing.** `DEPENDS` above names
			# the one file on the command line, so a shared header changes and every shader that
			# includes it keeps its stale SPIR-V — a picture that is wrong until somebody deletes the
			# build directory, and wrong differently for each person depending on when they last did.
			# glslang writes the list it actually opened, so the rule is generated rather than
			# maintained, which is the only version of it that cannot drift from the `#include`s.
			DEPFILE "${OUTPUT}.d"
			COMMENT "Compiling ${MODULE}/Shaders/${SOURCE}"
			VERBATIM
		)

		list(APPEND OUTPUTS "${OUTPUT}")
	endforeach()

	# A target rather than adding the outputs to the module's sources: a generated header is not a
	# translation unit, and CMake orders it against a compile only if something depends on it.
	add_custom_target(${MODULE}Shaders DEPENDS ${OUTPUTS})
	add_dependencies(${MODULE} ${MODULE}Shaders)

	# PUBLIC because the module's own tests compile against the same headers, which is the reason
	# Render already links volk that way.
	target_include_directories(${MODULE} PUBLIC "${GENERATED}")
endfunction()
