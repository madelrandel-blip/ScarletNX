/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver: the extracted image's static blocks to a native module. Never creates an
   execution engine. */
#include "3ds/aot/ctr_compile.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/aot/ctr_runtime.h"
#include "3ds/aot/ctr_discover.h"
#include "common/cpu/arm/llvm.h"
#include "common/utilities/common.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(SNX_ARM_CODEGEN_OPTIONS_ABI == SNX_CTR_MODULE_CODEGEN_ABI, "codegen ABI");
_Static_assert(SNX_ARM_MEMORY_CHECKED == SNX_CTR_MODULE_CHECKED_MODE, "memory ABI");
#define CHUNK_ENTRIES 128u
#define OBJECT_LIMIT  (256u * 1024u * 1024u)
#define MODULE_LIMIT  (512u * 1024u * 1024u)
typedef struct {
    unsigned mode, index, length;
} Entry;
typedef struct {
    char* triple;
    LLVMTargetMachineRef machine;
    LLVMTargetDataRef layout;
} Host;
static uint32_t little(const unsigned char* p, unsigned size) {
    uint32_t result = 0;
    for (unsigned i = 0; i < size; ++i) result |= (uint32_t)p[i] << (i * 8);
    return result;
}
/* a staged file's name inside the staging directory */
static const char* leaf(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') last = p + 1;
    return last;
}
static void name(char out[40], unsigned mode, unsigned index) {
    snprintf(out, 40, "ctr_%c_%u", mode ? 't' : 'a', index);
}
static LLVMValueRef table(LLVMModuleRef mod, const char* id, LLVMValueRef* functions, unsigned count) {
    LLVMTypeRef ptr = LLVMPointerTypeInContext(LLVMGetModuleContext(mod), 0);
    LLVMValueRef result = LLVMAddGlobal(mod, LLVMArrayType(ptr, count), id);
    LLVMSetInitializer(result, LLVMConstArray(ptr, functions, count));
    LLVMSetGlobalConstant(result, 1);
    LLVMSetLinkage(result, LLVMInternalLinkage);
    return result;
}
static bool descriptor(LLVMModuleRef mod, LLVMTargetDataRef layout, const SnxCtrImage* image, LLVMValueRef arm,
                       LLVMValueRef thumb) {
    LLVMContextRef c = LLVMGetModuleContext(mod);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(c), i32 = LLVMInt32TypeInContext(c);
    LLVMTypeRef types[18];
    LLVMValueRef values[18], hash[32];
    types[0] = LLVMInt64TypeInContext(c);
    values[0] = LLVMConstInt(types[0], SNX_CTR_MODULE_MAGIC, 0);
    uint32_t integers[] = {SNX_CTR_MODULE_ABI,
                           sizeof(SnxCtrModule),
                           sizeof(SnxArmCPU),
                           sizeof(SnxArmCheckedCPU),
                           SNX_ARM_ABI,
                           SNX_ARM_CHECKED_ABI,
                           SNX_ARM_CODEGEN_OPTIONS_ABI,
                           SNX_ARM_PROFILE_ARM11,
                           SNX_ARM_MEMORY_CHECKED,
                           sizeof(void*),
                           image->segment[0].address,
                           image->segment[0].bytes,
                           image->segment[0].bytes / 4,
                           image->segment[0].bytes / 2};
    const size_t offsets[] = {offsetof(SnxCtrModule, magic),           offsetof(SnxCtrModule, abi),
                              offsetof(SnxCtrModule, descriptor_size), offsetof(SnxCtrModule, cpu_size),
                              offsetof(SnxCtrModule, checked_size),    offsetof(SnxCtrModule, arm_abi),
                              offsetof(SnxCtrModule, checked_abi),     offsetof(SnxCtrModule, codegen_abi),
                              offsetof(SnxCtrModule, profile),         offsetof(SnxCtrModule, memory_mode),
                              offsetof(SnxCtrModule, pointer_size),    offsetof(SnxCtrModule, text_address),
                              offsetof(SnxCtrModule, text_bytes),      offsetof(SnxCtrModule, arm_count),
                              offsetof(SnxCtrModule, thumb_count),     offsetof(SnxCtrModule, image_identity),
                              offsetof(SnxCtrModule, arm_blocks),      offsetof(SnxCtrModule, thumb_blocks)};
    for (unsigned i = 0; i < 14; ++i) {
        types[i + 1] = i32;
        values[i + 1] = LLVMConstInt(i32, integers[i], 0);
    }
    types[15] = LLVMArrayType(i8, 32);
    for (unsigned i = 0; i < 32; ++i) hash[i] = LLVMConstInt(i8, image->identity[i], 0);
    values[15] = LLVMConstArray(i8, hash, 32);
    types[16] = types[17] = LLVMPointerTypeInContext(c, 0);
    values[16] = arm;
    values[17] = thumb;
    LLVMTypeRef structure = LLVMStructTypeInContext(c, types, 18, 0);
    if (LLVMABISizeOfType(layout, structure) != sizeof(SnxCtrModule)) return false;
    for (unsigned i = 0; i < 18; ++i)
        if (LLVMOffsetOfElement(layout, structure, i) != offsets[i]) return false;
    LLVMValueRef global = LLVMAddGlobal(mod, structure, "snx_ctr_module_v1");
    LLVMSetInitializer(global, LLVMConstNamedStruct(structure, values, 18));
    LLVMSetGlobalConstant(global, 1);
#ifdef _WIN32
    /* COFF has no hidden visibility: without an explicit export, lld exports every global of the DLL, a block
       function each, past PE's 65535 exports. Exporting the descriptor alone turns that off, as the ELF module's
       hidden blocks leave it the only symbol the loader looks up. */
    LLVMSetDLLStorageClass(global, LLVMDLLExportStorageClass);
#endif
    return true;
}
static LLVMModuleRef new_module(Host* h, LLVMContextRef c) {
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext("ctr-static-aot-v1", c);
    LLVMSetTarget(m, h->triple);
    LLVMSetModuleDataLayout(m, h->layout);
    return m;
}
static bool write_object(Host* h, LLVMModuleRef mod, char* output, const char* pipeline, char* error, size_t size) {
    char* detail = NULL;
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &detail)) goto fail;
    LLVMDisposeMessage(detail);
    detail = NULL;
    if (pipeline) {
        LLVMPassBuilderOptionsRef passes = LLVMCreatePassBuilderOptions();
        LLVMErrorRef failure = LLVMRunPasses(mod, pipeline, h->machine, passes);
        LLVMDisposePassBuilderOptions(passes);
        if (failure) {
            char* message = LLVMGetErrorMessage(failure);
            snprintf(error, size, "%s", message);
            LLVMDisposeErrorMessage(message);
            return false;
        }
        if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &detail)) goto fail;
        LLVMDisposeMessage(detail);
        detail = NULL;
    }
    if (LLVMTargetMachineEmitToFile(h->machine, mod, output, LLVMObjectFile, &detail)) goto fail;
    LLVMDisposeMessage(detail);
    return true;
fail:
    snprintf(error, size, "%s", detail ? detail : "cannot emit native ARM11 object");
    LLVMDisposeMessage(detail);
    return false;
}
static bool chunk(Host* h, const SnxCtrImage* image, const Entry* entries, unsigned count, char* output,
                  bool exhaustive, char* error, size_t size) {
    LLVMContextRef c = LLVMContextCreate();
    LLVMModuleRef mod = new_module(h, c);
    bool ok = false;
    SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_CHECKED};
    for (unsigned i = 0; i < count; ++i) {
        const Entry* e = entries + i;
        unsigned width = e->mode ? 2 : 4;
        uint32_t pc = image->segment[0].address + e->index * width;
        uint32_t words[SNX_CTR_AOT_BLOCK_WORDS];
        uint16_t halves[SNX_CTR_AOT_BLOCK_WORDS];
        for (unsigned j = 0; j < e->length; ++j) {
            words[j] = little(image->code + e->index * width + j * width, width);
            halves[j] = (uint16_t)words[j];
        }
        char id[40];
        name(id, e->mode, e->index);
        size_t consumed = 0;
        LLVMValueRef f = e->mode ? snx_thumb_llvm_block_ex(mod, id, halves, e->length, pc, &consumed, &options)
                                 : snx_arm_llvm_block_ex(mod, id, words, e->length, pc, &consumed, &options);
        if (!f || consumed != e->length) {
            snprintf(error, size, "static discovery/backend termination mismatch at 0x%08x (%zu/%u)", pc, consumed,
                     e->length);
            goto done;
        }
        LLVMSetVisibility(f, LLVMHiddenVisibility);
    }
    ok = write_object(h, mod, output, exhaustive ? "default<O2>" : "default<O1>", error, size);
done:
    LLVMDisposeModule(mod);
    LLVMContextDispose(c);
    return ok;
}
static bool index_object(Host* h, const SnxCtrImage* image, const Entry* entries, unsigned count, char* output,
                         char* error, size_t size) {
    LLVMContextRef c = LLVMContextCreate();
    LLVMModuleRef mod = new_module(h, c);
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), ft = LLVMFunctionType(LLVMVoidTypeInContext(c), &ptr, 1, 0);
    LLVMValueRef tables[2];
    unsigned cursor = 0;
    bool ok = false;
    for (unsigned mode = 0; mode < 2; ++mode) {
        unsigned n = image->segment[0].bytes / (mode ? 2 : 4);
        LLVMValueRef* functions = malloc((size_t)n * sizeof *functions);
        if (!functions) {
            snprintf(error, size, "out of memory for static index");
            goto done;
        }
        for (unsigned i = 0; i < n; ++i) functions[i] = LLVMConstNull(ptr);
        while (cursor < count && entries[cursor].mode == mode) {
            const Entry* e = entries + cursor++;
            char id[40];
            name(id, mode, e->index);
            functions[e->index] = LLVMAddFunction(mod, id, ft);
            LLVMSetVisibility(functions[e->index], LLVMHiddenVisibility);
        }
        tables[mode] = table(mod, mode ? "ctr_thumb" : "ctr_arm", functions, n);
        free(functions);
    }
    if (!descriptor(mod, h->layout, image, tables[0], tables[1])) {
        snprintf(error, size, "incompatible native descriptor layout");
        goto done;
    }
    ok = write_object(h, mod, output, NULL, error, size);
done:
    LLVMDisposeModule(mod);
    LLVMContextDispose(c);
    return ok;
}
int snx_ctr_aot_compile(const char* extracted, char* error, size_t error_size) {
    int result = 1;
    SnxCtrImage image = {0}, check = {0};
    SnxCtrPlan plan = {0};
    Host host = {0};
    char *directory = NULL, *target = NULL, *stage = NULL, *module = NULL, *runtime = NULL, **objects = NULL;
    char* previous = NULL;
    Entry* entries = NULL;
    unsigned count = 0, files = 0;
    const char** command = NULL;
    LLVMTargetRef backend;
    char* detail = NULL;
    if (error_size) error[0] = 0;
    if (!snx_ctr_image_load(extracted, &image, error, error_size)) goto done;
    if (image.segment[0].bytes > SNX_CTR_AOT_TEXT_LIMIT) {
        snprintf(error, error_size, "static AOT text limit is %u bytes", SNX_CTR_AOT_TEXT_LIMIT);
        result = 3;
        goto done;
    }
    directory = snx_absolute(extracted);
    if (!directory) {
        snprintf(error, error_size, "invalid extracted path");
        goto done;
    }
    target = snx_path_join(directory, "native");
    bool exhaustive = image.segment[0].bytes <= SNX_CTR_AOT_EXHAUSTIVE_LIMIT;
    unsigned capacity = image.segment[0].bytes / 4 + image.segment[0].bytes / 2;
    if (!exhaustive && !snx_ctr_discover(&image, &plan, error, error_size)) {
        result = 3;
        goto done;
    }
    entries = calloc(exhaustive ? capacity : plan.entries, sizeof *entries);
    if (!entries) {
        snprintf(error, error_size, "out of memory for static entries");
        goto done;
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        unsigned width = mode ? 2 : 4;
        for (unsigned i = 0; i < image.segment[0].bytes / width; ++i) {
            unsigned length = exhaustive ? 1 : snx_ctr_plan_length(&plan, image.segment[0].address + i * width, !!mode);
            if (length) entries[count++] = (Entry){mode, i, length};
        }
    }
    printf("Discovered ARM11 static coverage: %u blocks%s\n", count,
           exhaustive ? " (exhaustive tiny image)" : " (conservative CFG)");
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        snprintf(error, error_size, "cannot initialize native LLVM target");
        goto done;
    }
    host.triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(host.triple, &backend, &detail)) {
        snprintf(error, error_size, "%s", detail);
        goto done;
    }
    host.machine = LLVMCreateTargetMachine(backend, host.triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC,
                                           LLVMCodeModelDefault);
    if (!host.machine) {
        snprintf(error, error_size, "cannot create native LLVM target");
        goto done;
    }
    host.layout = LLVMCreateTargetDataLayout(host.machine);
    stage = snx_temp_dir(directory);
    if (!stage) {
        snprintf(error, error_size, "cannot create private native staging directory");
        goto done;
    }
    files = (count + CHUNK_ENTRIES - 1) / CHUNK_ENTRIES + 1;
    objects = calloc(files, sizeof *objects);
    if (!objects) {
        snprintf(error, error_size, "out of memory for native chunks");
        goto done;
    }
    objects[0] = snx_path_join(stage, "code.o");
    if (!index_object(&host, &image, entries, count, objects[0], error, error_size)) goto done;
    uint64_t output_bytes = snx_file_size(objects[0]);
    for (unsigned i = 1; i < files; ++i) {
        unsigned begin = (i - 1) * CHUNK_ENTRIES, n = count - begin;
        if (n > CHUNK_ENTRIES) n = CHUNK_ENTRIES;
        objects[i] = snx_format("%s/block-%05u.o", stage, i);
        if (!chunk(&host, &image, entries + begin, n, objects[i], exhaustive, error, error_size)) goto done;
        output_bytes += snx_file_size(objects[i]);
        if (output_bytes > OBJECT_LIMIT) {
            snprintf(error, error_size, "native object output limit exceeded");
            result = 3;
            goto done;
        }
        if (!(i % 16) || i + 1 == files) printf("ARM11 AOT: %u/%u static blocks emitted\n", begin + n, count);
    }
    if (output_bytes > OBJECT_LIMIT || snx_ctr_aot_runtime_size > OBJECT_LIMIT - output_bytes) {
        snprintf(error, error_size, "native object output limit exceeded including semantic helpers");
        result = 3;
        goto done;
    }
    runtime = snx_path_join(stage, "arm-runtime.a");
    if (!runtime || snx_write_file(runtime, snx_ctr_aot_runtime, snx_ctr_aot_runtime_size)) {
        snprintf(error, error_size, "cannot stage bounded native semantic helpers");
        goto done;
    }
    module = snx_path_join(stage, "code.so");
    command = calloc(files + 7, sizeof *command);
    if (!command) {
        snprintf(error, error_size, "out of memory for native linker arguments");
        goto done;
    }
    unsigned arg = 0;
    command[arg++] = SNX_CTR_HOST_CC;
    command[arg++] = "-shared";
#ifdef __APPLE__
    /* ld64's: a dylib refuses undefined symbols, as -z defs does (its default, said); its stacks aren't executable and
       its constant data is kept read-only without being asked, which noexecstack, relro and now ask GNU ld for */
    command[arg++] = "-Wl,-undefined,error";
#elif !defined(_WIN32)
    command[arg++] = "-Wl,-z,defs,-z,noexecstack,-z,relro,-z,now";
#endif
    /* A PE DLL needs none of those: an unresolved symbol is always a link error, lld marks it NXCOMPAT, and it has no
       lazily bound PLT for relro and now to protect. lld's MinGW driver refuses -z. */
    /* The linker runs in the staging directory and is given the files' names alone. With full paths a large image's
       hundreds of chunks pass the 32767 characters a Windows command line holds (CreateProcessW fails with
       ERROR_FILENAME_EXCED_RANGE); 1024 chunks, the most the entry limit allows, take about 15000 by name. */
    for (unsigned i = 0; i < files; ++i) command[arg++] = leaf(objects[i]);
    command[arg++] = leaf(runtime);
    command[arg++] = "-o";
    command[arg++] = leaf(module);
    if (snx_run_stage(command, stage, "native linking")) {
        snprintf(error, error_size, "native linker failed; no runnable module published");
        goto done;
    }
    if (!snx_is_file(module) || snx_is_link(module) || !snx_file_size(module)) {
        snprintf(error, error_size, "native linker did not produce a regular nonempty module");
        goto done;
    }
    if (snx_file_size(module) > MODULE_LIMIT) {
        snprintf(error, error_size, "native module output limit exceeded");
        result = 3;
        goto done;
    }
    if (!snx_ctr_image_load(directory, &check, error, error_size) || memcmp(image.identity, check.identity, 32)) {
        snprintf(error, error_size, "extracted image changed during compilation");
        goto done;
    }
    /* a module from an earlier compile is replaced, only once the new one is complete */
    previous = snx_path_join(directory, "native.previous");
    if ((snx_path_exists(previous) && snx_remove_tree(previous)) ||
        (snx_path_exists(target) && snx_replace(target, previous)) || snx_replace(stage, target)) {
        snprintf(error, error_size, "cannot publish new native output");
        goto done;
    }
    (void)snx_remove_tree(previous);
    free(stage);
    stage = NULL;
    printf("Compiled ARM11 static module: %u blocks in %u objects\n", count, files);
    result = 0;
done:
    if (stage) snx_remove_tree(stage);
    if (objects)
        for (unsigned i = 0; i < files; ++i) free(objects[i]);
    free(command);
    free(objects);
    free(entries);
    free(runtime);
    free(module);
    free(stage);
    free(target);
    free(previous);
    free(directory);
    LLVMDisposeMessage(detail);
    if (host.layout) LLVMDisposeTargetData(host.layout);
    if (host.machine) LLVMDisposeTargetMachine(host.machine);
    LLVMDisposeMessage(host.triple);
    snx_ctr_plan_free(&plan);
    snx_ctr_image_free(&check);
    snx_ctr_image_free(&image);
    return result;
}
