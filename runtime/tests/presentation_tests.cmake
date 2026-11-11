# Shared by standalone runtime and title builds.
if(BUILD_TESTING AND NOT TARGET psx-presentation-scene-test)
    get_filename_component(_presentation_runtime "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    foreach(_kind scene draw)
        add_executable(psx-presentation-${_kind}-test
            "${_presentation_runtime}/tests/test_presentation_${_kind}.c"
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
    if(APPLE AND Python3_Interpreter_FOUND AND TARGET SDL3::SDL3-static)
        foreach(_fixture presentation opaque_batch wide_margins)
            add_test(NAME psx_gl_${_fixture}
                COMMAND "${Python3_EXECUTABLE}"
                    "${_presentation_runtime}/tests/run_gl_texture_filter.py" --debug-tools
                    --fixture "${_presentation_runtime}/tests/test_gl_${_fixture}.c"
                    --cc "${CMAKE_C_COMPILER}"
                    --sdl-include "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>"
                    --sdl-library "$<TARGET_FILE:SDL3::SDL3-static>"
                    --output "${CMAKE_CURRENT_BINARY_DIR}/gl-${_fixture}-results")
            set_tests_properties(psx_gl_${_fixture} PROPERTIES
                LABELS "gpu;opengl;hardware" RUN_SERIAL TRUE TIMEOUT 180)
        endforeach()
    endif()
endif()
