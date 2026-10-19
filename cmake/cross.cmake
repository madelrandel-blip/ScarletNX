# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# The target's flags for what is compiled outside CMake's own compiles (FFmpeg's and OpenSSL's builds and the
# recompiler's runtime library), as cmake/apple.cmake gives them for Apple's: SNX_TARGET_FLAGS. It holds:
#   - SNX_ARCH_FLAGS, the CPU baseline the whole build compiles for, which CMake's compiles get too (added here).
#     PowerPC64 builds for POWER8: before it there is no lqarx/stqcx., and Clang calls libatomic's
#     __atomic_compare_exchange_16 for snx_cas128 (helpers/integer.c), which a module links no library to have.
#     ppc64le starts at POWER8 anyway; the BE machines before it (970) have no Vulkan driver.
#   - for a Linux cross build with Clang (the linux-ppc64* presets): the target triple and the sysroot, which CMake
#     passes to its own compiles from CMAKE_C_COMPILER_TARGET and CMAKE_SYSROOT and the others don't get from the
#     compiler's path. SNX_TARGET_LDFLAGS adds what their links need: LLD, as the build machine's ld only knows its
#     own architecture.
# All are empty for the other hosts and builds.
include_guard(GLOBAL)
set(SNX_ARCH_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ppc64|ppc64le|powerpc64|powerpc64le)$")
    set(SNX_ARCH_FLAGS -mcpu=power8)
endif()
add_compile_options(${SNX_ARCH_FLAGS})

set(SNX_TARGET_FLAGS "")
set(SNX_TARGET_LDFLAGS "")
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    if(NOT CMAKE_C_COMPILER_TARGET)
        message(FATAL_ERROR "A Linux cross build names its target in CMAKE_C_COMPILER_TARGET (e.g. "
                            "powerpc64le-linux-gnu), which FFmpeg, OpenSSL and the runtime library are built for.")
    endif()
    list(APPEND SNX_TARGET_FLAGS --target=${CMAKE_C_COMPILER_TARGET})
    if(CMAKE_SYSROOT)
        list(APPEND SNX_TARGET_FLAGS --sysroot=${CMAKE_SYSROOT})
    endif()
    set(SNX_TARGET_LDFLAGS -fuse-ld=lld)
endif()
list(APPEND SNX_TARGET_FLAGS ${SNX_ARCH_FLAGS})

# SNX_TARGET_C_COMPILER: a C compiler for the target, for the tests that give one to the program they run
# (native_codegen, native_recompiler, llvm_differential), which builds a library with it and loads it. In a cross
# build that program runs under the emulator, so the compiler is this one with the target's flags, in a wrapper
set(SNX_TARGET_C_COMPILER "${CMAKE_C_COMPILER}")
if(SNX_TARGET_LDFLAGS)
    string(REPLACE ";" " " snx_cross_flags "${SNX_TARGET_FLAGS};${SNX_TARGET_LDFLAGS}")
    file(WRITE "${CMAKE_BINARY_DIR}/target-cc" "#!/bin/sh\nexec '${CMAKE_C_COMPILER}' ${snx_cross_flags} \"$@\"\n")
    file(CHMOD "${CMAKE_BINARY_DIR}/target-cc" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
         GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(SNX_TARGET_C_COMPILER "${CMAKE_BINARY_DIR}/target-cc")
endif()
