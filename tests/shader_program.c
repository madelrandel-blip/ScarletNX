/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Program structure (gpu/shader/shader.c, corpus.c): VertexA + VertexB merged, the CRS stack resolved statically,
   and the shader corpus written, found complete or rewritten, and read back. */
#include "nx/gpu/shader/translator.h"

#define SCHED  0x001F8000FC0007E0ull
#define EXIT   0xE30000000007000Full
#define MOV32I 0x010000000007F001ull /* MOV32I R1, imm */
#define SYNC   0xF0F800000007000Full
#define BRK    0xE34000000007000Full
#define NOP    0x50B0000000070F00ull

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* a relative flow instruction at word at to word to (opcode in the top 12 bits) */
static u64 flow(u64 top12, u32 at, u32 to) {
    return top12 << 52 | (u64)(((to - (at + 1)) * 8) & 0xFFFFFF) << 20 | 0x7000Full;
}

static void merge(void) {
    u64 a[64] = {0}, b[64] = {0}, out[64];
    int i;
    for (i = 0; i < 10; i++) {
        a[i] = 0xAAAAAAAA00000000ull | (u64)i;
        b[i] = 0xBBBBBBBB00000000ull | (u64)i;
    }
    a[10] = b[10] = SCHED;
    a[11] = MOV32I;
    a[12] = (EXIT & ~0xF001Full) | 0x30007ull; /* EXIT under P3, flow condition 7 */
    a[13] = EXIT;
    b[11] = MOV32I | 2;
    b[12] = EXIT;
    check(shader_merge_dual((const u8*)a, (const u8*)b, (u8*)out, sizeof out) == 1, "VertexA and VertexB merge");
    for (i = 0; i < 10; i++) check(out[i] == b[i], "the merged program has VertexB's header");
    check(out[10] == SCHED && out[11] == MOV32I, "VertexA's code comes first");
    check(decode(out[12])->id == O_BRA && bra_target(out[12], 12) == 15 && B(out[12], 16, 4) == 3 &&
              B(out[12], 0, 5) == 7,
          "a conditional EXIT of VertexA branches to VertexB under its predicate and condition");
    check(decode(out[13])->id == O_BRA && bra_target(out[13], 13) == 15 && flow_always(out[13]),
          "VertexA's final EXIT branches to VertexB");
    check(out[14] == SCHED && out[15] == b[11] && out[16] == b[12] && !out[17], "VertexB follows at the next bundle");
    check(shader_merge_dual((const u8*)a, (const u8*)b, (u8*)out, 16 * 8) == 0, "a merge that doesn't fit fails");
    /* VertexA ending before its last bundle does: the bundle's word after the EXIT is VertexA's, never zero, which no
       instruction is (Catherine's dual vertex programs end in a BRA to itself with a word left in the bundle) */
    a[12] = EXIT;
    a[13] = NOP;
    check(shader_merge_dual((const u8*)a, (const u8*)b, (u8*)out, sizeof out) == 1 && out[13] == NOP &&
              out[14] == SCHED && out[15] == b[11],
          "VertexA's last bundle is copied whole");
}

static void crs(void) {
    u64 code[32] = {0};
    u32 pops[32];
    int i;
    for (i = 10; i < 32; i += 4) code[i] = SCHED;
    code[11] = flow(0xE29, 11, 19); /* SSY 19 */
    code[12] = flow(0xE24, 12, 16) & ~0xF0000ull; /* @P0 BRA 16 */
    code[13] = SYNC;
    code[15] = MOV32I;
    code[16] = SYNC;
    code[17] = MOV32I;
    code[19] = EXIT;
    check(shader_resolve_crs(code, 20, pops) == 1 && pops[13] == 19 && pops[16] == 19,
          "both SYNCs of an SSY region pop to its target");
    code[11] = MOV32I;
    check(shader_resolve_crs(code, 20, pops) == 0, "a SYNC with no SSY token fails");
    /* PBK 23, SSY 19, BRK: the BRK pops through the SSY token to the PBK's */
    code[11] = flow(0xE2A, 11, 23);
    code[12] = flow(0xE29, 12, 19);
    code[13] = BRK;
    code[15] = code[16] = code[17] = MOV32I;
    code[19] = SYNC;
    code[23] = EXIT;
    check(shader_resolve_crs(code, 24, pops) == 1 && pops[13] == 23, "a BRK drops the SSY token above its own");
    code[17] = flow(0xE26, 17, 21); /* CAL */
    check(shader_resolve_crs(code, 24, pops) == 0, "a program with calls keeps its stack at run time");
}

static long file_size(const char* path) {
    long size;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    return size;
}

static void corpus(const char* dir) {
    u64 code[16] = {0};
    char path[1024];
    ShaderInfo si, back;
    u32 max_bytes = 0;
    int stage = -1;
    u8* loaded;
    long full;
    FILE* f;
    memset(&si, 0, sizeof si);
    si.stage = ST_FS;
    si.rt_int_mask = 0x1234;
    code[10] = SCHED;
    code[11] = EXIT;
    snprintf(path, sizeof path, "%s/00000000c0ffee00_4.snxs", dir);
    remove(path);
    check(shader_corpus_save(dir, (const u8*)code, 96, 0x10000, ST_FS, &si, 0xc0ffee00ull) == 1, "a corpus entry saves");
    full = file_size(path);
    check(full == (long)(24 + sizeof si + 96), "the entry holds the header, the ShaderInfo and the code");
    loaded = shader_corpus_load(path, &max_bytes, &stage, &back);
    check(loaded && max_bytes == 96 && stage == ST_FS && back.rt_int_mask == 0x1234 && !memcmp(loaded, code, 96),
          "the entry loads back with its recorded length as the window");
    free(loaded);
    f = fopen(path, "wb"); /* a write cut short */
    if (f) {
        fwrite("SNXS", 1, 4, f);
        fclose(f);
    }
    check(!shader_corpus_load(path, &max_bytes, &stage, &back), "a cut-off entry doesn't load");
    check(shader_corpus_save(dir, (const u8*)code, 96, 0x10000, ST_FS, &si, 0xc0ffee00ull) == 1 &&
              file_size(path) == full,
          "a cut-off entry is written again");
    remove(path);
}

int main(int argc, char** argv) {
    merge();
    crs();
    if (argc > 1) corpus(argv[1]);
    return failures ? 1 : 0;
}
