# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
add_library(scarletnx-gnuabi STATIC
    abi/syscalls/syscalls.c loader/elf/elf.c memory/memory.c process/process.c
    signals/signals.c threading/futex.c runtime/runtime.c runtime/execution.c runtime/arm_exec.c runtime/jit.c
    process/spawn.c filesystem/vfs.c filesystem/devfs/devfs.c filesystem/tmpfs/tmpfs.c filesystem/procfs/procfs.c
    filesystem/hostfs/hostfs.c
    network/socket.c network/poll.c network/etc.c)
set_target_properties(scarletnx-gnuabi PROPERTIES OUTPUT_NAME scarletnx-gnuabi-runtime C_STANDARD 23 C_STANDARD_REQUIRED ON)
# AArch32 execution needs the ARM LLVM block backend. Build it here when the NX
# runtime (which normally owns it) is not part of this configuration.
if(NOT TARGET snx_arm_llvm AND NOT SNX_BUILD_NX)
    include("${CMAKE_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/arm" gnuabi-arm-backend)
    if(COMMAND snx_test_sanitize) # tests/CMakeLists.txt: made after it looked
        snx_test_sanitize(snx_arm_llvm)
    endif()
endif()
# The JIT compiles through the recompiler library (snx_recompiler.h), which the NX driver's tree configures. Without
# the NX runtime, configure it here
if(NOT TARGET scarletnx-recompiler)
    include("${CMAKE_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/recompiler/llvm" gnuabi-llvm-backend)
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/recompiler" gnuabi-recompiler-library)
endif()
target_link_libraries(scarletnx-gnuabi PUBLIC scarletnx-common scarletnx-net snx_arm_llvm scarletnx-recompiler)
target_link_libraries(scarletnx-gnuabi PUBLIC OpenSSL::SSL)
target_compile_options(scarletnx-gnuabi PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(scarletnx-gnuabi PRIVATE _CRT_SECURE_NO_WARNINGS)
if(WIN32)
    target_link_libraries(scarletnx-gnuabi PRIVATE bcrypt)
endif()
add_executable(scarletnx-gnuabi-run runtime/main.c)
target_link_libraries(scarletnx-gnuabi-run PRIVATE scarletnx-gnuabi)
# its checkout, where an older version kept the default sysroot, linux/ (snx_checkout)
target_compile_definitions(scarletnx-gnuabi-run PRIVATE SNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SNX_BUILD_DIR="${CMAKE_BINARY_DIR}")
set_target_properties(scarletnx-gnuabi-run PROPERTIES OUTPUT_NAME scarletnx-gnuabi
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}" C_STANDARD 23 C_STANDARD_REQUIRED ON)
if(BUILD_TESTING)
    add_executable(snx-test-gnuabi-runtime "${PROJECT_SOURCE_DIR}/tests/gnuabi_runtime.c")
    target_link_libraries(snx-test-gnuabi-runtime PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-runtime PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-runtime PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_memory_elf_syscalls COMMAND snx-test-gnuabi-runtime)
    # again as on a host with 64 KiB pages (PowerPC64), each holding sixteen guest pages
    add_test(NAME gnuabi_memory_64k_pages COMMAND snx-test-gnuabi-runtime)
    set_tests_properties(gnuabi_memory_64k_pages PROPERTIES ENVIRONMENT SNX_HOST_PAGE=65536)
    add_executable(snx-test-gnuabi-vfs "${PROJECT_SOURCE_DIR}/tests/gnuabi_vfs.c")
    target_link_libraries(snx-test-gnuabi-vfs PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-vfs PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-vfs PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_vfs_isolation COMMAND snx-test-gnuabi-vfs)
    if (NOT WIN32)
        add_executable(snx-test-gnuabi-hostfs "${PROJECT_SOURCE_DIR}/tests/gnuabi_hostfs.c")
        target_link_libraries(snx-test-gnuabi-hostfs PRIVATE scarletnx-gnuabi)
        target_compile_options(snx-test-gnuabi-hostfs PRIVATE -Wall -Wextra -Werror)
        set_target_properties(snx-test-gnuabi-hostfs PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        add_test(NAME gnuabi_hostfs COMMAND snx-test-gnuabi-hostfs)
    endif()
    add_executable(snx-test-gnuabi-network "${PROJECT_SOURCE_DIR}/tests/gnuabi_network.c")
    target_link_libraries(snx-test-gnuabi-network PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-network PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-network PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_network COMMAND snx-test-gnuabi-network)
    set_tests_properties(gnuabi_network PROPERTIES TIMEOUT 30)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gnuabi_elf32 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/gnuabi_elf32.py"
            $<TARGET_FILE:scarletnx-gnuabi-run>)
        if (NOT WIN32)
            add_test(NAME gnuabi_process COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/gnuabi_process.py"
                $<TARGET_FILE:scarletnx-gnuabi-run>)
            set_tests_properties(gnuabi_process PROPERTIES TIMEOUT 300)
        endif()
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS scarletnx-gnuabi-run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(COMMAND snx_sanitizer_path) # tests/CMakeLists.txt: the runtime made here, and this folder's tests
    snx_test_sanitize(scarletnx-gnuabi)
    snx_sanitizer_path()
endif()
