
find_program(GLSLC_PATH glslc
	HINTS
		"$ENV{VULKAN_SDK}/Bin"
		"$ENV{VULKAN_SDK}/bin"
)

# Shader compilation controls
option(SHADERS_ENABLE_COMPILATION "Enable compilation of shaders to SPIR-V" OFF)
option(SHADER_FORCE_PREBUILT "Force using prebuilt shaders (skip tool checks/compilation)" OFF)
option(SHADER_DEBUG_INFO "Emit debug info (-g) when compiling shaders" OFF)

mark_as_advanced(SHADERS_ENABLE_COMPILATION SHADER_FORCE_PREBUILT SHADER_DEBUG_INFO)

# Tool checks (only when Vulkan is enabled and we actually compile)
if (FSO_BUILD_WITH_VULKAN AND SHADERS_ENABLE_COMPILATION AND NOT SHADER_FORCE_PREBUILT)
	if (NOT GLSLC_PATH)
		message(FATAL_ERROR "glslc not found. Install/enable the Vulkan SDK or set GLSLC_PATH, or set SHADER_FORCE_PREBUILT=ON to use prebuilts.")
	endif()
endif()

if (SHADERS_ENABLE_COMPILATION AND NOT SHADER_FORCE_PREBUILT)
	if(PLATFORM_WINDOWS)
		set(SHADERTOOL_FILENAME "shadertool-windows.tar.gz")
	elseif(PLATFORM_LINUX)
		set(SHADERTOOL_FILENAME "shadertool-linux.tar.gz")
	else()
		message(FATAL_ERROR "SHADERS_ENABLE_COMPILATION is ON but shadertool binaries are unavailable on this platform. Set SHADER_FORCE_PREBUILT=ON to use prebuilts.")
	endif()

	# The existence of glslc indicated whether we can compile our shaders or not
	message(STATUS "Found glslc program. Shaders will be compiled during the build.")

	set(SHADERTOOL_VERSION "v1.0")
	set(SHADERTOOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/shadertool/${SHADERTOOL_VERSION}")

	# Download correct shadertool version if we do not have it already
	if (NOT IS_DIRECTORY "${SHADERTOOL_DIR}")
		set(DOWNLOAD_URL "https://github.com/scp-fs2open/fso-shadertool/releases/download/${SHADERTOOL_VERSION}/${SHADERTOOL_FILENAME}")
		set(DOWNLOAD_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADERTOOL_FILENAME}")

		set(MAX_RETRIES 5)
		foreach(i RANGE 1 ${MAX_RETRIES})
			if (NOT (i EQUAL 1))
				message(STATUS "Retry after 5 seconds (attempt #${i}) ...")
				execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep "5")
			endif()

			message(STATUS "Downloading shadertool binaries from \"${DOWNLOAD_URL}\" (try ${i}/${MAX_RETRIES})")
			file(DOWNLOAD "${DOWNLOAD_URL}" "${DOWNLOAD_FILE}" SHOW_PROGRESS TLS_VERIFY ON STATUS DOWNLOAD_STATUS_LIST)

			list(GET DOWNLOAD_STATUS_LIST 0 DOWNLOAD_STATUS)
			list(GET DOWNLOAD_STATUS_LIST 1 DOWNLOAD_ERROR)
			if (DOWNLOAD_STATUS EQUAL 0)
				break()
			endif()
			message(STATUS "Download of shadertool binaries failed: ${DOWNLOAD_ERROR}!")
		endforeach()

		if (NOT (DOWNLOAD_STATUS EQUAL 0))
			message(FATAL_ERROR "${MAX_RETRIES} download attempts failed!")
			return()
		endif()

		# Make sure the directory exists
		file(MAKE_DIRECTORY "${SHADERTOOL_DIR}")

		# Extract the downloaded file
		message(STATUS "Extracting shadertool package...")
		execute_process(
			COMMAND ${CMAKE_COMMAND} -E tar xzf "${DOWNLOAD_FILE}"
			WORKING_DIRECTORY "${SHADERTOOL_DIR}"
			RESULT_VARIABLE EXTRACT_RESULT
			ERROR_VARIABLE ERROR_TEXT
		)

		if (NOT (EXTRACT_RESULT EQUAL 0))
			message(FATAL_ERROR "Extracting shadertool binaries failed! Error message: ${ERROR_TEXT}")
			return()
		endif()

		file(REMOVE "${DOWNLOAD_FILE}")
	endif()

	add_executable(glslc IMPORTED GLOBAL)
	set_target_properties(glslc PROPERTIES IMPORTED_LOCATION "${GLSLC_PATH}")

	# Just use CMake for finding the shadertool binaries to avoid platform specific code here
	find_program(SHADERTOOL_PATH shadertool
		PATHS "${SHADERTOOL_DIR}/bin"
		NO_DEFAULT_PATH)

	add_executable(shadertool IMPORTED GLOBAL)
	set_target_properties(shadertool PROPERTIES IMPORTED_LOCATION "${SHADERTOOL_PATH}")
	if (NOT SHADERTOOL_PATH)
		message(FATAL_ERROR "shadertool not found after download/extract. Verify SHADERTOOL_PATH or set SHADER_FORCE_PREBUILT=ON to use prebuilts.")
	endif()
endif ()
