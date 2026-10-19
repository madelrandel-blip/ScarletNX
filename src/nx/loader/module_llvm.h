/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module from the LLVM backend: what module.c and toolchain.c produce through C and Clang, written directly
   (LLVM IR per chunk, compiled in parallel, linked with the runtime library by recompiler/llvm/link.c) */
#ifndef SNX_NX_MODULE_LLVM_H
#define SNX_NX_MODULE_LLVM_H

#include "nx/loader/module.h"

typedef struct {
    const char* output; /* <Game>_code.so or .dll, replaced only by a complete module */
    char opt;           /* -O letter */
    unsigned jobs;
    int quiet;
    const char* keep_ir; /* when set, each unit's IR is saved there as .ll */
} SnxLlvmBuild;

/* SNX_ABI as this build has it: the layout its modules are written with, which the runtime of the same build checks */
unsigned snx_module_abi(void);
/* 0 on success. LLVM is a required recompiler dependency. */
int snx_build_module_llvm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build);
/* A 32-bit program's module (module_arm.c): its AArch32 blocks and an SnxArmModule. LLVM only */
int snx_build_module_arm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build);

#endif
