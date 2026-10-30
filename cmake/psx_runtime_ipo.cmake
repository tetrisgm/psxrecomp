include_guard(GLOBAL)

set(PSX_RUNTIME_IPO "AUTO" CACHE STRING
    "IPO/LTO for optimized runtime targets: AUTO (supported toolchains), ON, OFF")
set_property(CACHE PSX_RUNTIME_IPO PROPERTY STRINGS AUTO ON OFF)
set(PSX_RUNTIME_IPO_JOBS "4" CACHE STRING "Maximum supported linker/LTO worker count")
if(NOT PSX_RUNTIME_IPO_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "PSX_RUNTIME_IPO_JOBS must be a positive integer")
endif()

function(psxrecomp_apply_runtime_ipo target)
    # OFF must not alter a parent project's explicit IPO policy or add flags.
    if(NOT PSX_RUNTIME_IPO)
        return()
    endif()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _psx_ipo_ok OUTPUT _psx_ipo_error LANGUAGES C CXX)
    if(NOT _psx_ipo_ok)
        if(PSX_RUNTIME_IPO STREQUAL "AUTO")
            message(STATUS "psxrecomp: IPO unavailable; retaining normal optimization for ${target}")
            return()
        endif()
        message(FATAL_ERROR
            "PSX_RUNTIME_IPO=ON was requested, but C/C++ IPO is unsupported.\n"
            "Use a compatible compiler/linker or configure PSX_RUNTIME_IPO=OFF.\n"
            "${_psx_ipo_error}")
    endif()
    # Target/configuration-scoped: do not silently enable IPO in third-party
    # libraries, build helpers, Debug, or custom configurations.
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_DEBUG FALSE)
    foreach(_psx_ipo_config RELEASE RELWITHDEBINFO MINSIZEREL)
        set_property(TARGET ${target}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_psx_ipo_config} TRUE)
    endforeach()
    # LLD can otherwise start one worker per hardware thread, independently
    # of the build tool's job limit. Probe the actual linker (also covers
    # MinGW Clang); unsupported linkers retain their native policy.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT APPLE)
        include(CheckLinkerFlag)
        check_linker_flag(C "-Wl,--thinlto-jobs=${PSX_RUNTIME_IPO_JOBS},--threads=${PSX_RUNTIME_IPO_JOBS}"
            _psx_ipo_bounded_lld)
        if(_psx_ipo_bounded_lld)
            target_link_options(${target} PRIVATE
                "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:LINKER:--thinlto-jobs=${PSX_RUNTIME_IPO_JOBS},--threads=${PSX_RUNTIME_IPO_JOBS}>")
        endif()
    elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-flto=${PSX_RUNTIME_IPO_JOBS}>")
    endif()
    message(STATUS "psxrecomp: runtime IPO enabled for ${target} (optimized configurations)")
endfunction()
