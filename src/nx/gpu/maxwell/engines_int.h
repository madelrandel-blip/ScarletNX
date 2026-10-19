/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the GPU engines' files (engines.c, mme.c, mme_hle.c, compute.c, fermi2d.c, dma.c, maxwell3d.c) share */
#ifndef SNX_ENGINES_INT_H
#define SNX_ENGINES_INT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* engines.c: a report semaphore operation (the word v written to base + 3) of the 3D or compute engine, whose
   registers r hold the address at base and base + 1 and the payload at base + 2 */
void query_get(GpuChannel* ch, u32* r, int base, u32 v);

/* dma.c: a block linear surface's bytes up to slice z as one host pointer, with their count in span, when they are
   contiguous in guest memory and 256 MiB at most; NULL otherwise */
u8* bl_span_ptr(u64 base, u32 wbytes, u32 h, u32 bh, u32 z, u32 bd, u64* span);
/* dma.c: the bytes of a block linear surface up to the end of slice z (the span bl_span_ptr hands out) */
u64 bl_span_size(u32 wbytes, u32 h, u32 bh, u32 z, u32 bd);

/* mme.c: runs macro idx over the np parameters in ch->mme_params (it may stop at a wait, gpu.h's mme_resume),
   natively when its code is one mme_hle.c knows */
void mme_run(GpuChannel* ch, u32 idx, u32 np);
/* the macro upload methods: 0x45 (where the next code word goes, which begins an upload batch), 0x46 (a code word)
   and 0x48 (a macro's start, at the slot 0x47 set) */
void mme_upload_ptr(GpuChannel* ch, u32 v);
void mme_upload(GpuChannel* ch, u32 v);
void mme_bind(GpuChannel* ch, u32 v);
/* the methods a macro sends, recorded instead of sent: SNX_MME_VERIFY records a native implementation and the
   interpreter, and compares them. The first MME_REC_N are kept, and the hash covers them all */
#define MME_REC_N 1024
typedef struct MmeRec {
    u32 n, total; /* methods kept, and sent */
    u64 hash;
    int read_after_send; /* a register read came after a send, which recording didn't apply: what it read can differ */
    u32 m[MME_REC_N], v[MME_REC_N];
} MmeRec;
/* what a macro does, natively or interpreted: sends method m (to the 3D engine, or a macro method's register only,
   as from the pushbuffer a macro method would start a macro inside this one), or reads register a. With rec, the
   send is recorded instead */
void mme_out(GpuChannel* ch, MmeRec* rec, u32 m, u32 v);
u32 mme_reg(GpuChannel* ch, MmeRec* rec, u32 a);

/* mme_hle.c: native implementations of known macros, by the hash of their code (mme.c's mme_choose) */
/* the implementation for a hash, or -1 */
int mme_native_find(u64 hash);
const char* mme_native_name(int k);
u64 mme_native_hash(int k);
/* runs implementation k over the np parameters p, as mme_out sends: 0 when it leaves this call to the interpreter,
   which it says before sending anything */
int mme_native_run(int k, GpuChannel* ch, MmeRec* rec, const u32* p, u32 np);

#endif
