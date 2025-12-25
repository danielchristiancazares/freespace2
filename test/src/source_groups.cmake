
set(source_files)

add_file_folder(""
    main.cpp
    test_stubs.cpp
)

add_file_folder("Actions"
)

add_file_folder("Actions\\\\Expression"
	actions/expression/test_ExpressionParser.cpp
)

add_file_folder("CFile"
    cfile/cfile.cpp
)

add_file_folder("Globalincs"
    globalincs/test_flagset.cpp
    globalincs/test_safe_strings.cpp
    globalincs/test_version.cpp
)

add_file_folder("Graphics"
	   graphics/test_font.cpp
	   graphics/test_post_processing_null_safety.cpp
	   graphics/test_opengl_postprocessing_smaa_fallback.cpp
)

if(FSO_BUILD_WITH_VULKAN)
add_file_folder("Graphics"
	   graphics/test_vulkan_dynstate.cpp
	   graphics/test_vulkan_pipeline_manager.cpp
	   graphics/test_vulkan_frame_lifecycle.cpp
	   graphics/test_vulkan_recordingframe_sealed.cpp
	   graphics/test_vulkan_shader_manager_model.cpp
	   graphics/test_vulkan_shader_alignment.cpp
	   graphics/test_vulkan_texture_contract.cpp
	   graphics/test_vulkan_texture_helpers.cpp
	   graphics/test_vulkan_texture_upload_alignment.cpp
	   graphics/test_vulkan_fallback_texture.cpp
	   graphics/test_vulkan_buffer_manager_retirement.cpp
	   graphics/test_vulkan_depth_format_selection.cpp
	   graphics/test_vulkan_depth_attachment_switch.cpp
	   graphics/test_vulkan_renderer_shutdown.cpp
	   graphics/test_vulkan_scene_texture_lifecycle.cpp
	   graphics/test_model_shader_spirv.cpp
	   graphics/test_vulkan_descriptor_layouts.cpp
	   graphics/test_vulkan_shader_layout_contracts.cpp
	   graphics/test_vulkan_clip_scissor.cpp
	   graphics/test_vulkan_blend_enable.cpp
	   graphics/test_vulkan_device_scoring.cpp
	   graphics/test_vulkan_swapchain_acquire.cpp
	   graphics/test_vulkan_texture_render_target.cpp
	   graphics/test_vulkan_buffer_zero_size.cpp
	   graphics/test_vulkan_clear_ops_oneshot.cpp
	   graphics/test_vulkan_render_target_state.cpp
	   graphics/test_vulkan_deferred_release.cpp
	   graphics/test_vulkan_perdraw_bindings.cpp
	   graphics/test_vulkan_post_process_targets.cpp
	   graphics/test_vulkan_post_effects_semantics.cpp
	   graphics/it_vulkan_model_present.cpp
	   graphics/it_vulkan_subsystems.cpp
)
endif()

add_file_folder("Math"
    math/test_vecmat.cpp
)

add_file_folder("menuui"
    menuui/test_intel_parse.cpp
)

add_file_folder("mod"
    mod/test_mod_table.cpp
)

add_file_folder("model"
    model/test_modelread.cpp
)

add_file_folder("Parse"
    parse/test_parselo.cpp
    parse/test_replace.cpp
)

add_file_folder("Pilotfile"
    pilotfile/plr.cpp
)

add_file_folder("Scripting"
    scripting/ade_args.cpp
    scripting/doc_parser.cpp
    scripting/require.cpp
    scripting/script_state.cpp
    scripting/ScriptingTestFixture.h
    scripting/ScriptingTestFixture.cpp
)

add_file_folder("Scripting\\\\API"
    scripting/api/async.cpp
    scripting/api/base.cpp
    scripting/api/bitops.cpp
    scripting/api/enums.cpp
    scripting/api/hookvars.cpp
)

add_file_folder("Scripting\\\\Lua"
    scripting/lua/Args.cpp
    scripting/lua/Convert.cpp
    scripting/lua/Function.cpp
    scripting/lua/Reference.cpp
    scripting/lua/Table.cpp
    scripting/lua/TestUtil.h
    scripting/lua/Thread.cpp
    scripting/lua/Util.cpp
    scripting/lua/Value.cpp
)

add_file_folder("Test Util"
    util/FSTestFixture.cpp
    util/FSTestFixture.h
    util/test_util.h
)

add_file_folder("Utils"
    utils/HeapAllocatorTest.cpp
)

add_file_folder("Weapon"
    weapon/weapons.cpp
)
