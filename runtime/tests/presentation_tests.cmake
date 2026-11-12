# Shared by standalone runtime and title builds.
if(BUILD_TESTING AND NOT TARGET psx-presentation-scene-test)
    get_filename_component(_presentation_runtime "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    foreach(_source test_presentation_scene.c test_presentation_draw.c)
        string(REGEX REPLACE "test_presentation_(.*)\\.c" "\\1" _kind "${_source}")
        add_executable(psx-presentation-${_kind}-test
            "${_presentation_runtime}/tests/${_source}"
            "${_presentation_runtime}/src/presentation_scene.c")
        if(_kind STREQUAL "draw")
            target_sources(psx-presentation-${_kind}-test PRIVATE
                "${_presentation_runtime}/src/presentation_draw.c")
        endif()
        target_include_directories(psx-presentation-${_kind}-test PRIVATE
            "${_presentation_runtime}/include")
        if(MSVC)
            target_compile_options(psx-presentation-${_kind}-test PRIVATE /UNDEBUG)
        else()
            target_compile_options(psx-presentation-${_kind}-test PRIVATE -UNDEBUG)
            target_link_libraries(psx-presentation-${_kind}-test PRIVATE m)
        endif()
        add_test(NAME psx_presentation_${_kind} COMMAND psx-presentation-${_kind}-test)
    endforeach()
    add_executable(psx-render-pass-sandbox-test
        "${_presentation_runtime}/tests/test_render_pass_sandbox.c")
    foreach(_source render_pass.c frame_trace.c stereo_shed_policy.c render_pass_plan.c psx_cycles.c timers.c psx_video_timing.c)
        target_sources(psx-render-pass-sandbox-test PRIVATE "${_presentation_runtime}/src/${_source}")
    endforeach()
    target_include_directories(psx-render-pass-sandbox-test PRIVATE "${_presentation_runtime}/include")
    if(NOT MSVC)
        target_compile_options(psx-render-pass-sandbox-test PRIVATE -UNDEBUG)
        target_link_libraries(psx-render-pass-sandbox-test PRIVATE m)
    endif()
    add_test(NAME psx_render_pass_sandbox COMMAND psx-render-pass-sandbox-test)
    if(APPLE)
        add_executable(psx-bundle-storage-test "${_presentation_runtime}/tests/test_macos_bundle_storage.cpp")
        target_include_directories(psx-bundle-storage-test PRIVATE "${_presentation_runtime}/include")
        target_compile_features(psx-bundle-storage-test PRIVATE cxx_std_17)
        target_compile_options(psx-bundle-storage-test PRIVATE -UNDEBUG)
        add_test(NAME psx_bundle_storage COMMAND psx-bundle-storage-test)
    endif()
    add_executable(psx-pgxp-propagation-test
        "${_presentation_runtime}/tests/test_pgxp.cpp"
        "${_presentation_runtime}/tests/pgxp_test_mod_memory.cpp"
        "${_presentation_runtime}/src/pgxp.cpp"
        "${_presentation_runtime}/src/psx_ram_geometry.c")
    target_include_directories(psx-pgxp-propagation-test PRIVATE "${_presentation_runtime}/include")
    add_test(NAME psx_pgxp_propagation COMMAND psx-pgxp-propagation-test)
    if(APPLE AND Python3_Interpreter_FOUND AND TARGET SDL3::SDL3-static)
        add_executable(psx-keyboard-pad-chord-test
            "${_presentation_runtime}/tests/test_keyboard_pad_chord.c"
            "${_presentation_runtime}/src/psx_keybinds.c"
            "${_presentation_runtime}/src/host_keymap.c")
        target_include_directories(psx-keyboard-pad-chord-test PRIVATE "${_presentation_runtime}/include")
        target_compile_definitions(psx-keyboard-pad-chord-test PRIVATE SDL_MAIN_HANDLED PSX_SDL3=1)
        target_link_libraries(psx-keyboard-pad-chord-test PRIVATE SDL3::SDL3-static)
        add_test(NAME psx_keyboard_pad_chord COMMAND psx-keyboard-pad-chord-test)
        if(TARGET retcomm_rbengine)
            add_executable(psx-rewind-direct-test "${_presentation_runtime}/tests/test_rewind_load_context.c" "${_presentation_runtime}/src/rewind_history.c")
            target_include_directories(psx-rewind-direct-test PRIVATE "${_presentation_runtime}/include" "${_presentation_runtime}/src")
            target_compile_definitions(psx-rewind-direct-test PRIVATE PSX_HAS_RBENGINE_SNAP=1 PSX_NATIVE_CAPTURE_JOBS=1 PSX_SDL3=1)
            target_compile_options(psx-rewind-direct-test PRIVATE -O2 -flto -UNDEBUG)
            target_link_options(psx-rewind-direct-test PRIVATE -flto -Wl,-dead_strip)
            target_link_libraries(psx-rewind-direct-test PRIVATE retcomm_rbengine SDL3::SDL3-static compression)
            add_test(NAME psx_rewind_direct COMMAND psx-rewind-direct-test)
        endif()
        add_executable(psx-rewind-history-test
            "${_presentation_runtime}/tests/test_rewind_history.c"
            "${_presentation_runtime}/src/rewind_history.c")
        target_include_directories(psx-rewind-history-test PRIVATE "${_presentation_runtime}/include")
        target_compile_definitions(psx-rewind-history-test PRIVATE PSX_SDL3=1)
        target_compile_options(psx-rewind-history-test PRIVATE -UNDEBUG)
        target_link_libraries(psx-rewind-history-test PRIVATE SDL3::SDL3-static compression)
        add_test(NAME psx_rewind_history COMMAND psx-rewind-history-test)
        # Exercise the Windows/zlib path on the Mac too; Apple Compression
        # passing alone does not qualify portable history invalidation/decoding.
        add_executable(psx-rewind-history-portable-test
            "${_presentation_runtime}/tests/test_rewind_history.c"
            "${_presentation_runtime}/src/rewind_history.c")
        target_include_directories(psx-rewind-history-portable-test PRIVATE "${_presentation_runtime}/include")
        target_compile_definitions(psx-rewind-history-portable-test PRIVATE PSX_SDL3=1 PSX_REWIND_TEST_PORTABLE_CODEC=1)
        target_compile_options(psx-rewind-history-portable-test PRIVATE -UNDEBUG)
        target_link_libraries(psx-rewind-history-portable-test PRIVATE SDL3::SDL3-static ZLIB::ZLIB)
        add_test(NAME psx_rewind_history_portable COMMAND psx-rewind-history-portable-test)
        foreach(_source test_gl_presentation.c test_gl_opaque_batch.c test_gl_wide_margins.c)
            string(REGEX REPLACE "test_gl_(.*)\\.c" "\\1" _fixture "${_source}")
            add_test(NAME psx_gl_${_fixture}
                COMMAND "${Python3_EXECUTABLE}"
                    "${_presentation_runtime}/tests/run_gl_texture_filter.py" --debug-tools
                    --fixture "${_presentation_runtime}/tests/${_source}"
                    --cc "${CMAKE_C_COMPILER}"
                    --sdl-include "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>"
                    --sdl-library "$<TARGET_FILE:SDL3::SDL3-static>"
                    --output "${CMAKE_CURRENT_BINARY_DIR}/gl-${_fixture}-results")
            set_tests_properties(psx_gl_${_fixture} PROPERTIES
                LABELS "gpu;opengl;hardware" RUN_SERIAL TRUE TIMEOUT 180)
        endforeach()
    endif()
endif()
