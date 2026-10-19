# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# GLSL shaders compiled into SPIR-V at build time, with glslang's glslangValidator, so no compiled shader is checked in.
#
# The compiler is SNX_GLSLANG when it is set. Otherwise it is KhronosGroup's glslang release for the host, a CPM package
# pinned by its SHA-256 like the libraries (cmake/dependencies.cmake), on the hosts it publishes builds for (Linux
# x86-64, Windows x86-64, macOS); elsewhere (FreeBSD, Linux on ARM) the glslangValidator on PATH, from the system's
# glslang package. Cross builds run the host's, since the shader is the same for every target.

set(SNX_GLSLANG "" CACHE FILEPATH "glslangValidator to compile shaders with (empty: the pinned release, or PATH)")
if(NOT SNX_GLSLANG)
    set(snx_glslang_version 16.6.0)
    set(snx_glslang_asset "")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(snx_glslang_asset linux-x86_64-release.tar.gz)
        set(snx_glslang_sha256 2c34071f56ecf39d16233294d3739cf43f981fd39080f2fd9dccb782101191ce)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(snx_glslang_asset windows-x86_64-release.zip)
        set(snx_glslang_sha256 82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(snx_glslang_asset macos-universal-release.tar.gz)
        set(snx_glslang_sha256 4f8c05f2888a73ed0b7b274bc524af9130e09ae94b43c0f3f9ed83cfae852834)
    endif()
    if(snx_glslang_asset)
        CPMAddPackage(NAME glslang-tools VERSION ${snx_glslang_version}
                      URL https://github.com/KhronosGroup/glslang/releases/download/${snx_glslang_version}/glslang-${snx_glslang_version}-${snx_glslang_asset}
                      URL_HASH SHA256=${snx_glslang_sha256}
                      DOWNLOAD_ONLY YES)
        set(snx_glslang_path "${glslang-tools_SOURCE_DIR}/bin/glslangValidator${CMAKE_HOST_EXECUTABLE_SUFFIX}")
        if (NOT EXISTS "${snx_glslang_path}")
            set(snx_glslang_path "${glslang-tools_SOURCE_DIR}/bin/glslang${CMAKE_HOST_EXECUTABLE_SUFFIX}")
        endif()
    else()
        find_program(snx_glslang_path NAMES glslangValidator glslang)
    endif()
    if(NOT EXISTS "${snx_glslang_path}")
        message(FATAL_ERROR "glslangValidator not found: install glslang (its tools), or set SNX_GLSLANG")
    endif()
    set(SNX_GLSLANG "${snx_glslang_path}" CACHE FILEPATH
        "glslangValidator to compile shaders with (empty: the pinned release, or PATH)" FORCE)
endif()

# snx_glsl_spirv(target SOURCE file.comp NAME symbol HEADER include/path.h [DEFINES MACRO[=value]...])
# Compiles SOURCE (its #includes followed through glslang's depfile) for Vulkan 1.1, optimized, validated and without
# debug information, into a C file added to target: `const u32 NAME[]` and `const u64 NAME_size` (bytes), as HEADER,
# which the C file includes, declares them. DEFINES are predefined macros, for another build of the same source
function(snx_glsl_spirv target)
    cmake_parse_arguments(arg "" "SOURCE;NAME;HEADER" "DEFINES" ${ARGN})
    set(defines "")
    foreach(define IN LISTS arg_DEFINES)
        list(APPEND defines "-D${define}")
    endforeach()
    get_filename_component(source "${arg_SOURCE}" ABSOLUTE)
    set(dir "${CMAKE_BINARY_DIR}/generated/spirv")
    set(words "${dir}/${arg_NAME}.inc")
    set(out "${dir}/${arg_NAME}.c")
    file(MAKE_DIRECTORY "${dir}")
    file(RELATIVE_PATH shown "${PROJECT_SOURCE_DIR}" "${source}")
    # two steps, so that glslang's depfile names its command's output (the .inc), as the Makefile generators need it
    # to (they make it a rule for the file it names). DEPENDS rather than MAIN_DEPENDENCY: a source can be built twice
    add_custom_command(OUTPUT "${words}"
        COMMAND "${SNX_GLSLANG}" -V --target-env vulkan1.1 -g0 --spirv-val -x ${defines}
                --depfile "${words}.d" -o "${words}" "${source}"
        DEPENDS "${source}"
        DEPFILE "${words}.d"
        COMMENT "Compiling ${shown} to SPIR-V (${arg_NAME})"
        VERBATIM)
    add_custom_command(OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -DWORDS=${words} -DOUT=${out} -DNAME=${arg_NAME} -DHEADER=${arg_HEADER}
                -DSOURCE=${shown} -P "${PROJECT_SOURCE_DIR}/cmake/spirv_source.cmake"
        DEPENDS "${words}" "${PROJECT_SOURCE_DIR}/cmake/spirv_source.cmake"
        VERBATIM)
    target_sources(${target} PRIVATE "${out}")
endfunction()
