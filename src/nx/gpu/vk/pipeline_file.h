/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_PIPELINE_FILE_H
#define SNX_VK_PIPELINE_FILE_H
/* Not in vk.h: the lock below brings host.h, which includes <windows.h> on Windows, whose macros (far, near) break
   the renderer's other files */
#include "common/cpu/aarch64/cpu.h"
#include "common/filesystem/host.h" /* PMutex */
#include "nx/gpu/shader/shader.h"
#include <stdio.h>

/* pipeline_file.c: the translated shaders and the pipelines made, kept across runs in shader_cache/<game>.vkshaders,
   so the next run makes them before the game asks (pipeline.c). The file is a header (PF_MAGIC, PF_VERSION, then
   the key: the build and the device, whatever changes the SPIR-V or the meaning of a pipeline key) and records
   appended as they are made, each {u32 type, u32 payload bytes, u64 check of the payload} and its payload, a multiple
   of 8 bytes. Reading stops at the first record that doesn't check out, and a file of another key is started over.
   Modules are known to the file by a u64 of their handle, and records refer to shaders by their number in the file
   and their check, never by handle */
#define PF_SHADER 1
#define PF_PIPE   2
#define PF_MAX    (256u << 20) /* past this size nothing more is appended */
/* a shader record: this, then ncbr constant buffer reads ({idx, off, ok, v}, VkCbRead's words) and nspv SPIR-V
   words. Everything vk_shader_get keys a shader by and checks a found one with */
typedef struct {
    u64 hash, qh, qh_a;
    u32 stage, clen, clen_a, ncbr, nspv, pad;
    u32 mask[20];
    ShaderInfo si;
} PfShader;
/* a pipeline record: this, then keylen bytes of the PipeKey with its module handles zeroed. sh: the shaders' record
   numbers (from 1; 0 for none) in the order vs, tcs, tes, gs, fs, and check: those records' checks */
typedef struct {
    u64 check[5];
    u32 sh[5];
    u32 keylen;
} PfPipe;
typedef struct {
    u64 module, check; /* module 0: a free slot */
    u32 rec, pad;
} PfMod;
typedef struct {
    PMutex lock;
    FILE* f;  /* appended to; NULL once nothing more is (an error, or the file reached PF_MAX) */
    u64 size; /* the file's bytes */
    u32 nsh;  /* shader records in it */
    /* each module with a record: its number and check, by open addressing on the handle (cap a power of two) */
    PfMod* map;
    u32 cap, nmap;
} PFile;
/* What reading a file hands back. shader: a record, with its reads and SPIR-V, made into a module; the module's u64
   (its handle's bits), or 0 when it couldn't be made. pipe: a record whose shaders were all made, with their modules' u64s
   (0 for none) and the key's bytes */
typedef struct {
    u64 (*shader)(void* ctx, const PfShader* s, const u32* cbr, const u32* spv);
    void (*pipe)(void* ctx, const u64 modules[5], const void* key, u32 keylen);
    void* ctx;
} PfLoad;
/* opens path: the records written there with the same key go to load in their order, and what follows is appended.
   A missing file, or one with another key, is started over. The counts of shader and pipeline records handed on go
   in loaded[0] and [1] */
void pf_open(PFile* pf, const char* path, const char* key, const PfLoad* load, u32 loaded[2]);
/* a shader record for module (made from spv), whose number pf_pipe then refers to; skipped once the file is full */
void pf_shader(PFile* pf, u64 module, const PfShader* s, const u32* cbr, const u32* spv);
/* a pipeline record with modules (vs, tcs, tes, gs, fs; 0 for none): skipped when one of them has no record */
void pf_pipe(PFile* pf, const u64 modules[5], const void* key, u32 keylen);

#endif
