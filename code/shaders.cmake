
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/graphics/shaders")
# This is the legacy location of shader code. To avoid duplicating included files, this is added as an include directory
set(LEGACY_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/def_files/data/effects")

set(VULKAN_SHADERS
	${SHADER_DIR}/vulkan.frag
	${SHADER_DIR}/vulkan.vert
)

set(SHADERS
	${SHADER_DIR}/default-material.frag
	${SHADER_DIR}/default-material.vert
)

if (FSO_BUILD_WITH_VULKAN)
	list(APPEND SHADERS ${VULKAN_SHADERS})
endif()

target_sources(code PRIVATE ${SHADERS})
source_group("Graphics\\Shaders" FILES ${SHADERS})

set(_structHeaderList)

# Vulkan-only shaders can use latest Vulkan features
set(GLSLC_TARGET_ENV_VULKAN "vulkan1.4")
# Cross-backend shaders need vulkan1.2 to avoid features that can't be translated to OpenGL
# (e.g., vulkan1.3+ converts discard to OpDemoteToHelperInvocation which has no OpenGL equivalent)
set(GLSLC_TARGET_ENV_COMPAT "vulkan1.2")

set(GLSLC_COMMON_FLAGS
	-O
	-g
	"-I${SHADER_DIR}"
	"-I${LEGACY_SHADER_DIR}"
	-Werror
	-x
	glsl
)

foreach (_shader ${SHADERS})
	if ("${_shader}" MATCHES "\\.glsl$")
		# Ignore include files since they will only be used but not compiled
		continue()
	endif ()

	get_filename_component(_fileName "${_shader}" NAME)

	# We write the compiled/generated shader files to the source directory so that they can be included in the VCS
	# This way, it is not necessary to have the tools for compiling shaders when doing non-shader related work
	set(_shaderOutputDir "${CMAKE_CURRENT_SOURCE_DIR}/graphics/shaders/compiled")
	set(_spirvFile "${_shaderOutputDir}/${_fileName}.spv")

	get_filename_component(_baseShaderName "${_shader}" NAME_WE)
	get_filename_component(_shaderExt "${_shader}" EXT)

	if (TARGET glslc)
		set(_depFileDir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
		set(_depFile "${_depFileDir}/${_fileName}.spv.d")
		file(RELATIVE_PATH _relativeSpirvPath "${CMAKE_BINARY_DIR}" "${_spirvFile}")

		set(_glslc_defines)
		if (_shader IN_LIST VULKAN_SHADERS)
			list(APPEND _glslc_defines "-DFSO_VULKAN=1")
			set(_glslc_target_env "${GLSLC_TARGET_ENV_VULKAN}")
		else()
			set(_glslc_target_env "${GLSLC_TARGET_ENV_COMPAT}")
		endif()

		set(DEPFILE_PARAM)
		if (CMAKE_GENERATOR STREQUAL "Ninja")
			set(DEPFILE_PARAM DEPFILE "${_depFile}")
		endif ()

		add_custom_command(OUTPUT "${_spirvFile}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${_depFileDir}"
			COMMAND glslc "${_shader}" -o "${_spirvFile}" --target-env=${_glslc_target_env} ${GLSLC_COMMON_FLAGS} ${_glslc_defines}
				-MD -MF "${_depFile}" -MT "${_relativeSpirvPath}"
			MAIN_DEPENDENCY "${_shader}"
			COMMENT "Compiling shader ${_fileName} (${_glslc_target_env})"
			${DEPFILE_PARAM}
			)

		target_embed_files(code FILES "${_spirvFile}" RELATIVE_TO "${_shaderOutputDir}" PATH_TYPE_PREFIX "data/effects")

		set(_glslOutput "${_spirvFile}.glsl")
		set(_structOutput "${_shaderOutputDir}/${_baseShaderName}_structs${_shaderExt}.h")

		list(APPEND _structHeaderList "${_structOutput}")

		# Vulkan-only shaders: generate struct headers but skip GLSL output
		# (Vulkan-specific features like demote_to_helper_invocation can't be translated to OpenGL GLSL)
		if (_shader IN_LIST VULKAN_SHADERS)
			add_custom_command(OUTPUT "${_structOutput}"
				COMMAND shadertool --structs "--structs-output=${_structOutput}" "${_spirvFile}"
				MAIN_DEPENDENCY "${_spirvFile}"
				COMMENT "Processing Vulkan shader ${_spirvFile} (structs only)"
				)
		else()
			add_custom_command(OUTPUT "${_glslOutput}" "${_structOutput}"
				COMMAND shadertool --glsl "--glsl-output=${_glslOutput}" --structs "--structs-output=${_structOutput}" "${_spirvFile}"
				MAIN_DEPENDENCY "${_spirvFile}"
				COMMENT "Processing shader ${_spirvFile}"
				)

			target_embed_files(code FILES "${_glslOutput}" RELATIVE_TO "${_shaderOutputDir}" PATH_TYPE_PREFIX "data/effects")
		endif()
	else()
		# Validate pre-compiled shader exists when shader compilation is disabled
		if (NOT EXISTS "${_spirvFile}")
			message(WARNING "Pre-compiled shader '${_spirvFile}' not found and shader compilation is disabled. "
				"Enable SHADERS_ENABLE_COMPILATION and install glslc, or ensure pre-compiled shaders are present.")
		endif()

		target_embed_files(code FILES "${_spirvFile}" RELATIVE_TO "${_shaderOutputDir}" PATH_TYPE_PREFIX "data/effects")

		set(_glslOutput "${_spirvFile}.glsl")
		set(_structOutput "${_shaderOutputDir}/${_baseShaderName}_structs${_shaderExt}.h")

		list(APPEND _structHeaderList "${_structOutput}")

		# Vulkan-only shaders don't have GLSL output
		if (NOT _shader IN_LIST VULKAN_SHADERS)
			target_embed_files(code FILES "${_glslOutput}" RELATIVE_TO "${_shaderOutputDir}" PATH_TYPE_PREFIX "data/effects")
		endif()
	endif()
endforeach ()

set(_shaderHeaderPath "${CMAKE_CURRENT_BINARY_DIR}/shader_structs.h")
set(_headerContent "#pragma once")
foreach(_headerPath ${_structHeaderList})
	set(_headerContent "${_headerContent}\n#include \"${_headerPath}\"")
endforeach()
# Use the generate command to avoid rewriting the file if the contents did not actually change
file(GENERATE OUTPUT "${_shaderHeaderPath}"
	CONTENT "${_headerContent}")
target_include_directories(code PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_sources(code PRIVATE ${_structHeaderList} "${_shaderHeaderPath}")
source_group("Graphics\\Shader structs" FILES ${_structHeaderList} "${_shaderHeaderPath}")
