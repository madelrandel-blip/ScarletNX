/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Hashes from Eden's HLE macro table (src/video_core/macro.cpp at de7e6e01):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project (GPL-3.0-or-later)
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project (GPL-2.0-or-later) */
/* Native implementations of the macros NVN uploads, chosen by the hash of their code (mme.c's mme_choose). Each one
   sends the methods the macro's code sends, in the same order with the same values, through mme_out, as the
   interpreter does: what it reads (its parameters and the shadow scratch registers, 0xD00 on) it reads before it
   sends anything, as the code does. They were derived from the code, and SNX_MME_VERIFY compares them with the
   interpreter call by call.
   The parameters are the macro's: a fetch past the last one reads 0, as in the interpreter. An implementation leaves a
   call to the interpreter (returns 0, before sending) where the interpreter would do something it doesn't: a loop
   long enough to reach the interpreter's 5,000,000 instruction limit, or a method computed from a parameter that
   could make the stream wait (0x6C3) or rewrite the macros (0x45-0x48) in the middle of the macro.
   Eden's names are given where its table has the hash. Its implementations call its rasterizer directly; these send
   methods, so the 3D engine (maxwell3d_method) does what it does for the interpreted macro */
#include "nx/gpu/maxwell/engines_int.h"

/* instances a draw macro loops over at most here: about 9 instructions each, under the interpreter's limit */
#define MME_LOOP_MAX 500000u

static u32 arg(const u32* p, u32 np, u32 i) { return i < np ? p[i] : 0; }

/* the instances of one draw: BEGIN (0x586) with the topology word, the count, END (0x585). From the second instance
   on, BEGIN's instance bits (26-27) say "next instance" (1) */
static void draw_instances(GpuChannel* ch, MmeRec* rec, u32 begin, u32 count_method, u32 count, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        mme_out(ch, rec, 0x586, begin);
        mme_out(ch, rec, count_method, count);
        mme_out(ch, rec, 0x585, 0);
        begin = (begin & ~(3u << 26)) | (1u << 26);
    }
}

/* Indexed instanced draw (Eden: HLE_DrawIndexedIndirect, false; TotK's busiest macro). Parameters: topology, index
   count, instance count (masked with scratch 0xD1B), first index, base vertex, base instance. The base vertex goes to
   0x446 and 0x50D and the base instance to 0x50E for the draw, and they are zeroed after it */
static int draw_indexed(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2) & mme_reg(ch, rec, 0xD1B);
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x5F7, arg(p, np, 3));
    mme_out(ch, rec, 0x446, arg(p, np, 4));
    mme_out(ch, rec, 0x50D, arg(p, np, 4));
    mme_out(ch, rec, 0x50E, arg(p, np, 5));
    draw_instances(ch, rec, arg(p, np, 0), 0x5F8, arg(p, np, 1), n);
    mme_out(ch, rec, 0x446, 0);
    mme_out(ch, rec, 0x50D, 0);
    mme_out(ch, rec, 0x50E, 0);
    return 1;
}
/* The same draw, which also writes the base vertex and base instance into the bound constant buffer at offset 0x640
   for the shaders, and zeroes them after (Eden: HLE_DrawIndexedIndirect, true) */
static int draw_indexed_cb(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2) & mme_reg(ch, rec, 0xD1B);
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x5F7, arg(p, np, 3));
    mme_out(ch, rec, 0x446, arg(p, np, 4));
    mme_out(ch, rec, 0x50D, arg(p, np, 4));
    mme_out(ch, rec, 0x50E, arg(p, np, 5));
    mme_out(ch, rec, 0x8E3, 0x640);
    mme_out(ch, rec, 0x8E4, arg(p, np, 4));
    mme_out(ch, rec, 0x8E5, arg(p, np, 5));
    draw_instances(ch, rec, arg(p, np, 0), 0x5F8, arg(p, np, 1), n);
    mme_out(ch, rec, 0x446, 0);
    mme_out(ch, rec, 0x50D, 0);
    mme_out(ch, rec, 0x50E, 0);
    mme_out(ch, rec, 0x8E3, 0x640);
    mme_out(ch, rec, 0x8E4, 0);
    mme_out(ch, rec, 0x8E5, 0);
    return 1;
}
/* Non-indexed instanced draw (Eden: HLE_DrawArraysIndirect, false). Parameters: topology, vertex count, instance
   count (masked with 0xD1B), first vertex (0x35D), base instance (0x50E, zeroed after) */
static int draw_arrays(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2) & mme_reg(ch, rec, 0xD1B);
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x35D, arg(p, np, 3));
    mme_out(ch, rec, 0x50E, arg(p, np, 4));
    draw_instances(ch, rec, arg(p, np, 0), 0x35E, arg(p, np, 1), n);
    mme_out(ch, rec, 0x50E, 0);
    return 1;
}
/* The same draw, which also writes the base instance into the bound constant buffer at offset 0x644 (Eden:
   HLE_DrawArraysIndirect, true) */
static int draw_arrays_cb(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2) & mme_reg(ch, rec, 0xD1B);
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x35D, arg(p, np, 3));
    mme_out(ch, rec, 0x50E, arg(p, np, 4));
    mme_out(ch, rec, 0x8E3, 0x644);
    mme_out(ch, rec, 0x8E4, arg(p, np, 4));
    draw_instances(ch, rec, arg(p, np, 0), 0x35E, arg(p, np, 1), n);
    mme_out(ch, rec, 0x50E, 0);
    mme_out(ch, rec, 0x8E3, 0x644);
    mme_out(ch, rec, 0x8E4, 0);
    return 1;
}
/* Selects constant buffer i for uploads (Eden: HLE_D7333D26E0A93EDE): its size from scratch 0xD2F + i and its
   address (in 256-byte units) from 0xD2A + i, into 0x8E0-0x8E2. Parameter: i */
static int cb_select(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 i = arg(p, np, 0), addr = mme_reg(ch, rec, i + 0xD2A), size = mme_reg(ch, rec, i + 0xD2F);
    mme_out(ch, rec, 0x8E0, size);
    mme_out(ch, rec, 0x8E1, addr >> 24);
    mme_out(ch, rec, 0x8E2, addr << 8);
    return 1;
}
/* Selects the 0x7000-byte constant buffer at scratch 0xD18 for uploads at word offset p0 (Eden:
   HLE_C713C83D8F63CCF3) */
static int cb_select_offset(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 addr = mme_reg(ch, rec, 0xD18);
    mme_out(ch, rec, 0x8E0, 0x7000);
    mme_out(ch, rec, 0x8E1, addr >> 24);
    mme_out(ch, rec, 0x8E2, addr << 8);
    mme_out(ch, rec, 0x8E3, arg(p, np, 0) << 2);
    return 1;
}
/* Binds a shader program (Eden: HLE_BindShader). Parameters: program slot i, program id, code offset, bind group,
   constant buffer address. When the id equals the one scratch 0xD1C + i keeps for the slot, nothing is sent.
   Otherwise the offset goes to the slot's program (0x801 + 16 * i), the id and the offset to scratch 0xD1C + i and
   0xD22 + i, a 0x10000-byte constant buffer at the address is selected (0x8E0-0x8E2), and bound as bank 1 of the bind
   group (0x904 + 8 * group, its low 7 bits). A slot above 15 is the interpreter's: its scratch methods would leave
   0xD1C-0xD31 and could reach the methods that wait or upload macros */
static int bind_program(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 i = arg(p, np, 0), cb;
    if (i > 15) return 0;
    if (arg(p, np, 1) == mme_reg(ch, rec, i + 0xD1C)) return 1;
    mme_out(ch, rec, 0x801 | i << 4, arg(p, np, 2));
    mme_out(ch, rec, 0xD1C + i, arg(p, np, 1));
    mme_out(ch, rec, 0xD22 + i, arg(p, np, 2));
    cb = arg(p, np, 4);
    mme_out(ch, rec, 0x8E0, 0x10000);
    mme_out(ch, rec, 0x8E1, cb >> 24);
    mme_out(ch, rec, 0x8E2, cb << 8);
    mme_out(ch, rec, 0x904 + ((arg(p, np, 3) & 0x7F) << 3), 0x11);
    return 1;
}
/* Writes p0 and bits 8-16 of p1 to scratch 0xD38-0xD39, then p0 to 0x0D8 and the low byte of p1 to 0x0D9. When bit 8
   of scratch 0xD3A is set, p0's top byte becomes the low byte of the smaller of p1's bits 8-16 and 0xD3A (by the sign
   of their difference). Not in Eden's table: the hash is of the code up to its last exit's delay slot */
static int scratch_d38(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 cfg = mme_reg(ch, rec, 0xD3A), a = arg(p, np, 1), hi = (a >> 8) & 0x1FF, v = arg(p, np, 0);
    mme_out(ch, rec, 0xD38, v);
    mme_out(ch, rec, 0xD39, hi);
    if ((cfg >> 8) & 1) v = (v & 0x00FFFFFFu) | ((((hi - cfg) & 0x80000000u) ? hi : cfg) & 0xFF) << 24;
    mme_out(ch, rec, 0x0D8, v);
    mme_out(ch, rec, 0x0D9, a & 0xFF);
    return 1;
}
/* Sends 0 to 0x671, then bits 8-15 of p0 one by one to the eight render targets' blend enables (0x4D8-0x4DF). Not in
   Eden's table */
static int blend_enables(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 v = arg(p, np, 0), i;
    mme_out(ch, rec, 0x671, 0);
    for (i = 0; i < 8; i++) mme_out(ch, rec, 0x4D8 + i, (v >> (8 + i)) & 1);
    return 1;
}
/* Writes the eight render targets' color masks (0x680-0x687) from four parameters, two 13-bit halves each (bits 0-12
   and 16-28). Not in Eden's table */
static int color_masks(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 i;
    for (i = 0; i < 4; i++) {
        mme_out(ch, rec, 0x680 + 2 * i, arg(p, np, i) & 0x1FFF);
        mme_out(ch, rec, 0x681 + 2 * i, (arg(p, np, i) >> 16) & 0x1FFF);
    }
    return 1;
}

/* Indexed draw with its index buffer given in the call (Tears of the Kingdom's most called interpreted one).
   Parameters: topology, index buffer address high and low, index format, count. BEGIN, the buffer's address and
   format, first index 0 and the count, END */
static int draw_indexed_inline(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    mme_out(ch, rec, 0x586, arg(p, np, 0));
    mme_out(ch, rec, 0x5F2, arg(p, np, 1));
    mme_out(ch, rec, 0x5F3, arg(p, np, 2));
    mme_out(ch, rec, 0x5F6, arg(p, np, 3));
    mme_out(ch, rec, 0x5F7, 0);
    mme_out(ch, rec, 0x5F8, arg(p, np, 4));
    mme_out(ch, rec, 0x585, 0);
    return 1;
}
/* The three words kept in scratch 0xD3B-0xD3D: when the parameters differ from them, 0x4B2 gets 25 and the scratch
   takes the parameters (only the last two when the first is the same) */
static int scratch_d3b(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 a = mme_reg(ch, rec, 0xD3B), b = mme_reg(ch, rec, 0xD3C), c = mme_reg(ch, rec, 0xD3D);
    if (arg(p, np, 0) != a) {
        mme_out(ch, rec, 0x4B2, 25);
        mme_out(ch, rec, 0xD3B, arg(p, np, 0));
    } else if (!((b ^ arg(p, np, 1)) | (c ^ arg(p, np, 2))))
        return 1;
    else
        mme_out(ch, rec, 0x4B2, 25);
    mme_out(ch, rec, 0xD3C, arg(p, np, 1));
    mme_out(ch, rec, 0xD3D, arg(p, np, 2));
    return 1;
}
/* When scratch 0xD28 is 1: 0x0BF and 0x0DB set, 0x54C 2, 0x0DB cleared. No parameters */
static int scratch_d28(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    (void)p, (void)np;
    if (mme_reg(ch, rec, 0xD28) != 1) return 1;
    mme_out(ch, rec, 0x0BF, 1);
    mme_out(ch, rec, 0x0DB, 1);
    mme_out(ch, rec, 0x54C, 2);
    mme_out(ch, rec, 0x0DB, 0);
    return 1;
}
/* Clears every layer of a render target (Eden: HLE_MultiLayerClear). Parameter: the CLEAR_SURFACE word, whose bits
   6-9 name the target: as many clears as its layer count (register 0x206 of its eight), the layer in bits 10 on */
static void clear_layers(GpuChannel* ch, MmeRec* rec, u32 w, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) mme_out(ch, rec, 0x674, w + 0x400 * i);
}
static int multi_layer_clear(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 w = arg(p, np, 0), n = mme_reg(ch, rec, 0x206 + (((w >> 6) & 0xF) << 4));
    if (n > MME_LOOP_MAX) return 0;
    clear_layers(ch, rec, w, n);
    return 1;
}
/* The same with the depth target's layer count (register 0x48C) */
static int multi_layer_clear_depth(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = mme_reg(ch, rec, 0x48C);
    if (n > MME_LOOP_MAX) return 0;
    clear_layers(ch, rec, arg(p, np, 0), n);
    return 1;
}

typedef struct {
    u64 hash;
    int (*run)(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np);
    const char* name;
} MmeNative;
static const MmeNative mme_natives[] = {
    {0x771BB18C62444DA0ull, draw_indexed, "indexed draw"},
    {0x0217920100488FF7ull, draw_indexed_cb, "indexed draw with constants"},
    {0x0D61FC9FAAC9FCADull, draw_arrays, "draw"},
    {0x8A4D173EB99A8603ull, draw_arrays_cb, "draw with constants"},
    {0xD7333D26E0A93EDEull, cb_select, "constant buffer select"},
    {0xC713C83D8F63CCF3ull, cb_select_offset, "constant buffer select at offset"},
    {0xEB29B2A09AA06D38ull, bind_program, "program bind"},
    {0x42B97BFB33086347ull, scratch_d38, "scratch 0xD38 and 0x0D8"},
    {0x176ED6F0009E577Aull, blend_enables, "blend enables"},
    {0xE8FCEAF999995860ull, color_masks, "color masks"},
    {0x2DB33AADB741839Cull, draw_indexed_inline, "indexed draw, buffer inline"},
    {0x5DA7C45099120734ull, scratch_d3b, "scratch 0xD3B-0xD3D"},
    {0x88EEF6578DA4695Dull, scratch_d28, "scratch 0xD28"},
    {0xEAD26C3E2109B06Bull, multi_layer_clear, "clear of every layer"},
    {0x71F722B9118625E7ull, multi_layer_clear_depth, "clear of every layer, the depth target's count"},
};
int mme_native_find(u64 hash) {
    int k;
    for (k = 0; k < (int)(sizeof mme_natives / sizeof mme_natives[0]); k++)
        if (mme_natives[k].hash == hash) return k;
    return -1;
}
const char* mme_native_name(int k) { return mme_natives[k].name; }
u64 mme_native_hash(int k) { return mme_natives[k].hash; }
int mme_native_run(int k, GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    return mme_natives[k].run(ch, rec, p, np);
}
