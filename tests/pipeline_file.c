/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The file of shaders and pipelines kept across runs (gpu/vk/pipeline_file.c), without a GPU: modules are numbers.
   Shader and pipeline records come back as written, a pipeline's shaders as the modules the next run made of them;
   a pipeline record waits for its shaders' records and is dropped when one of them couldn't be made again; records
   appended after a reading refer to the shaders read; a record cut short by a crash or changed on disk ends the
   reading, and the file goes on from the last good record; a file of another key starts over; a full file takes no
   more */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/pipeline_file.c"

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }

#define PATH "pipeline_file_test.vkshaders"
#define KEYB 96 /* a pipeline key's bytes */

/* what a reading handed back */
static struct {
    int nsh, npipe;
    PfShader sh[8];
    u32 cbr[8][8], spv[8][16];
    u64 mods[8][5];
    u8 key[8][KEYB];
    u64 next_module; /* the module the next shader is made into; 0 fails the first one when fail_first */
    int fail_first;
} got;

static u64 load_shader(void* ctx, const PfShader* s, const u32* cbr, const u32* spv) {
    (void)ctx;
    CHECK(got.nsh < 8 && s->ncbr <= 2 && s->nspv <= 16);
    got.sh[got.nsh] = *s;
    memcpy(got.cbr[got.nsh], cbr, s->ncbr * 16);
    memcpy(got.spv[got.nsh], spv, s->nspv * 4);
    got.nsh++;
    if (got.fail_first && got.nsh == 1) return 0;
    return got.next_module++;
}
static void load_pipe(void* ctx, const u64 modules[5], const void* key, u32 keylen) {
    (void)ctx;
    CHECK(got.npipe < 8 && keylen == KEYB);
    memcpy(got.mods[got.npipe], modules, sizeof got.mods[0]);
    memcpy(got.key[got.npipe], key, KEYB);
    got.npipe++;
}

static void open_file(PFile* pf, const char* key, u64 first_module, int fail_first, u32 loaded[2]) {
    PfLoad load;
    load.shader = load_shader;
    load.pipe = load_pipe;
    load.ctx = NULL;
    memset(&got, 0, sizeof got);
    got.next_module = first_module;
    got.fail_first = fail_first;
    memset(pf, 0, sizeof *pf);
    pm_init(&pf->lock);
    pf_open(pf, PATH, key, &load, loaded);
}
static void close_file(PFile* pf) {
    if (pf->f) fclose(pf->f);
    free(pf->map);
    pm_destroy(&pf->lock);
    memset(pf, 0, sizeof *pf);
}
static long file_size(void) {
    FILE* f = fopen(PATH, "rb");
    long n = -1;
    if (f && !fseek(f, 0, SEEK_END)) n = ftell(f);
    if (f) fclose(f);
    return n;
}

/* shader n of the test: its fields, reads and words from n */
static void make_shader(int n, PfShader* s, u32* cbr, u32* spv) {
    int i;
    memset(s, 0, sizeof *s);
    s->hash = 0x1234567890ABCDEFull * (u64)(n + 1);
    s->qh = 0xFEDCBA0987654321ull ^ (u64)n;
    s->qh_a = n == 1 ? 0x55AA55AA55AA55AAull : 0;
    s->stage = n == 1 ? ST_FS : ST_VS;
    s->clen = 0x100 + 8 * (u32)n;
    s->clen_a = n == 1 ? 0x80 : 0;
    s->ncbr = (u32)n % 3; /* 0, 1 or 2 reads */
    s->nspv = 5 + (u32)n;
    for (i = 0; i < 20; i++) s->mask[i] = (u32)(n * 100 + i);
    s->si.stage = (int)s->stage;
    s->si.ntex = n;
    s->si.tex[0].flags = SNX_TEX_INDEXED;
    s->si.cbuf_max[17] = 0xC0DE0000u + (u32)n;
    snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported, "shader %d", n);
    for (i = 0; i < (int)s->ncbr * 4; i++) cbr[i] = (u32)(n * 1000 + i);
    for (i = 0; i < (int)s->nspv; i++) spv[i] = i == 0 ? 0x07230203u : (u32)(n * 7 + i);
}
static void make_key(int n, u8* key) {
    int i;
    for (i = 0; i < KEYB; i++) key[i] = (u8)(n * 31 + i);
}
static void check_shader(int i, int n) {
    PfShader s;
    u32 cbr[8], spv[16];
    make_shader(n, &s, cbr, spv);
    CHECK(!memcmp(&got.sh[i], &s, sizeof s));
    CHECK(!memcmp(got.cbr[i], cbr, s.ncbr * 16));
    CHECK(!memcmp(got.spv[i], spv, s.nspv * 4));
}
static void add_shader(PFile* pf, int n, u64 module) {
    PfShader s;
    u32 cbr[8], spv[16];
    make_shader(n, &s, cbr, spv);
    pf_shader(pf, module, &s, cbr, spv);
}
static void add_pipe(PFile* pf, int n, u64 vs, u64 tcs, u64 tes, u64 gs, u64 fs) {
    u64 m[5];
    u8 key[KEYB];
    m[0] = vs;
    m[1] = tcs;
    m[2] = tes;
    m[3] = gs;
    m[4] = fs;
    make_key(n, key);
    pf_pipe(pf, m, key, KEYB);
}
static void check_pipe(int i, int n, u64 vs, u64 tcs, u64 tes, u64 gs, u64 fs) {
    u8 key[KEYB];
    make_key(n, key);
    CHECK(!memcmp(got.key[i], key, KEYB));
    CHECK(got.mods[i][0] == vs && got.mods[i][1] == tcs && got.mods[i][2] == tes && got.mods[i][3] == gs &&
          got.mods[i][4] == fs);
}

int main(void) {
    PFile pf;
    u32 loaded[2];
    long whole, header;
    remove(PATH);

    /* a first run: no file, records written as shaders and pipelines are made */
    open_file(&pf, "key one", 0xA0, 0, loaded);
    CHECK(loaded[0] == 0 && loaded[1] == 0 && got.nsh == 0 && pf.f);
    header = file_size();
    CHECK(header > 16 && header % 8 == 0);
    add_shader(&pf, 0, 0x1111);
    add_shader(&pf, 1, 0x2222);
    add_pipe(&pf, 0, 0x1111, 0, 0, 0, 0x2222);
    add_pipe(&pf, 1, 0x1111, 0, 0, 0, 0x9999); /* a module without a record (a clear's): not kept */
    add_pipe(&pf, 2, 0x2222, 0, 0, 0, 0);
    CHECK(pf.nsh == 2);
    close_file(&pf);

    /* the next run: everything back in order, the pipelines' shaders as the modules made of them now */
    open_file(&pf, "key one", 0xB0, 0, loaded);
    CHECK(loaded[0] == 2 && loaded[1] == 2 && got.nsh == 2 && got.npipe == 2);
    check_shader(0, 0);
    check_shader(1, 1);
    check_pipe(0, 0, 0xB0, 0, 0, 0, 0xB1);
    check_pipe(1, 2, 0xB1, 0, 0, 0, 0);
    /* what it makes refers to the shaders read and the ones it adds */
    add_shader(&pf, 2, 0x3333);
    add_pipe(&pf, 3, 0xB0, 0xB1, 0x3333, 0, 0xB1);
    close_file(&pf);
    whole = file_size();

    open_file(&pf, "key one", 0xC0, 0, loaded);
    CHECK(loaded[0] == 3 && loaded[1] == 3);
    check_shader(2, 2);
    check_pipe(2, 3, 0xC0, 0xC1, 0xC2, 0, 0xC1);
    CHECK(file_size() == whole && pf.size == (u64)whole);
    close_file(&pf);

    /* a shader that can't be made again: the pipelines with it are dropped */
    open_file(&pf, "key one", 0xD0, 1, loaded);
    CHECK(loaded[0] == 2 && got.nsh == 3 && loaded[1] == 1 && got.npipe == 1);
    check_pipe(0, 2, 0xD0, 0, 0, 0, 0);
    close_file(&pf);

    /* a record cut short (a crash while writing it): the reading stops before it, and the file goes on from there */
    {
        FILE* f = fopen(PATH, "rb");
        u8* all = (u8*)malloc((size_t)whole);
        CHECK(f && all && fread(all, 1, (size_t)whole, f) == (size_t)whole);
        fclose(f);
        f = fopen(PATH, "wb");
        CHECK(f && fwrite(all, 1, (size_t)whole - 12, f) == (size_t)whole - 12);
        fclose(f);
        open_file(&pf, "key one", 0xE0, 0, loaded);
        CHECK(loaded[0] == 3 && loaded[1] == 2); /* the last pipeline is gone */
        CHECK(pf.size < (u64)whole && file_size() == (long)pf.size);
        add_pipe(&pf, 4, 0xE2, 0, 0, 0, 0);
        close_file(&pf);
        open_file(&pf, "key one", 0xF0, 0, loaded);
        CHECK(loaded[0] == 3 && loaded[1] == 3);
        check_pipe(2, 4, 0xF2, 0, 0, 0, 0);
        close_file(&pf);

        /* a changed byte in the second shader's payload: only the first is read, then the file is cut there */
        f = fopen(PATH, "wb");
        all[header + PF_REC + sizeof(PfShader) + 40]++; /* past the first record, inside the second */
        CHECK(f && fwrite(all, 1, (size_t)whole, f) == (size_t)whole);
        fclose(f);
        free(all);
        open_file(&pf, "key one", 0x10, 0, loaded);
        CHECK(got.nsh == 1 && loaded[0] == 1 && loaded[1] == 0);
        CHECK(pf.nsh == 1 && file_size() == (long)pf.size);
        /* the shaders' numbers go on from the one left */
        add_shader(&pf, 1, 0x4444);
        add_pipe(&pf, 5, 0x10, 0, 0, 0, 0x4444);
        close_file(&pf);
        open_file(&pf, "key one", 0x20, 0, loaded);
        CHECK(loaded[0] == 2 && loaded[1] == 1);
        check_shader(1, 1);
        check_pipe(0, 5, 0x20, 0, 0, 0, 0x21);
        close_file(&pf);
    }

    /* another build or device: nothing read, the file started over */
    open_file(&pf, "key two", 0x30, 0, loaded);
    CHECK(loaded[0] == 0 && loaded[1] == 0 && file_size() == header && pf.nsh == 0);
    add_shader(&pf, 0, 0x5555);
    close_file(&pf);
    open_file(&pf, "key one", 0x40, 0, loaded);
    CHECK(loaded[0] == 0 && file_size() == header);
    close_file(&pf);

    /* a full file takes no more, and the run goes on */
    open_file(&pf, "key one", 0x50, 0, loaded);
    pf.size = PF_MAX - 64;
    add_shader(&pf, 3, 0x6666);
    CHECK(!pf.f && pf.nsh == 0);
    add_pipe(&pf, 6, 0x6666, 0, 0, 0, 0);
    close_file(&pf);
    CHECK(file_size() == header);

    remove(PATH);
    printf("pipeline_file: %u checks passed\n", checks);
    return 0;
}
