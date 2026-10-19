/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's clock and vi's presenting side (clock.c, present.c) against a clock the test sets: the system tick
   exact at any uptime, fps_limit's pacing of DequeueBuffer, and a CPU-read buffer's formats and layouts */
#include "nx/runtime/clock.c"
#include "nx/runtime/present.c"
#include <stdarg.h>
#include <stdio.h>

u8* g_mem;
int g_opt_fps = SNX_FPS_GAME;
static u64 now_ns, slept_ns;
static unsigned checks, failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "runtime_present.c:%d: ", __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

u64 plat_ns(void) { return now_ns; }
void plat_sleep_ns(i64 ns) {
    slept_ns += (u64)ns;
    now_ns += (u64)ns;
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
u64 nvmap_addr_by_id(u32 id) { return id == 7 ? 0x1000 : 0; }
void cheats_frame(void) {}
static u64 presented;
static void test_present(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4], u32 t) {
    (void)w, (void)h, (void)pitch, (void)layout, (void)bhl, (void)fmt, (void)crop, (void)t;
    presented = a;
}
static Renderer test_renderer;
const Renderer* g_renderer = &test_renderer;
/* a stand-in layout: rows of 64-byte GOB columns, enough to see the conversion goes through bl_offset */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    (void)z, (void)height, (void)bh_log2, (void)bd_log2;
    return (u64)(x_bytes / 64) * 64 * 8 + (u64)(y % 8) * 64 + (u64)(y / 8) * 8 * width_bytes + x_bytes % 64;
}

static void ticks(void) {
    static const u64 at[] = {0, 1, 9999, 10000, 52083, 1000000000u, SNX_U64C(0x2D79883u, 0xD2000000u) /* 10 years */,
                             SNX_U64C(0x7FFFFFFFu, 0xFFFFFFFFu)};
    size_t i;
    for (i = 0; i < sizeof at / sizeof *at; i++) {
        unsigned __int128 exact = (unsigned __int128)at[i] * 192 / 10000;
        now_ns = at[i]; /* clock_start ran at 0 */
        CHECK(guest_ticks() == (u64)exact, "ticks at %llu ns: %llu, not %llu", (unsigned long long)at[i],
              (unsigned long long)guest_ticks(), (unsigned long long)exact);
    }
}

static void pacing(void) {
    const u64 refresh = 1000000000u / 60;
    u64 before;
    int i;
    now_ns = 1000;
    slept_ns = 0;
    present_pace(2); /* the first frame doesn't wait */
    CHECK(!slept_ns, "the first frame waited %llu ns", (unsigned long long)slept_ns);
    for (i = 0; i < 3; i++) {
        before = slept_ns;
        now_ns += 1000000; /* the game took 1 ms for its frame */
        present_pace(2);
        CHECK(slept_ns - before == 2 * refresh - 1000000, "interval 2 waited %llu ns",
              (unsigned long long)(slept_ns - before));
    }
    before = slept_ns;
    present_pace(0);
    CHECK(slept_ns == before, "the game's pace with interval 0 waited");
    now_ns += 10 * refresh; /* a load: the schedule starts again rather than catching up */
    present_pace(1);
    CHECK(slept_ns == before, "a late frame waited");
    present_pace(1);
    CHECK(slept_ns - before == refresh, "the frame after a late one waited %llu ns",
          (unsigned long long)(slept_ns - before));
    g_opt_fps = 30;
    before = slept_ns;
    present_pace(0);
    present_pace(4); /* a number ignores the interval */
    CHECK(slept_ns - before == 2 * (1000000000u / 30), "fps_limit=30 waited %llu ns over two frames",
          (unsigned long long)(slept_ns - before));
    g_opt_fps = 0;
    before = slept_ns;
    present_pace(1);
    CHECK(slept_ns == before, "fps_limit=0 waited");
    g_opt_fps = SNX_FPS_GAME;
}

static void convert(void) {
    static u8 memory[0x10000];
    u32 out[16 * 16];
    u16 rgb565 = 0xF800;
    u32 bgra = 0x11223344;
    g_mem = memory;
    memcpy(memory + 0x1000, &bgra, 4);
    vi_convert_cpu(0x1000, 1, 1, 4, 1, 0, 5, out); /* BGRA swaps red and blue */
    CHECK(out[0] == 0x11443322u, "BGRA8888 came out %08x", out[0]);
    memcpy(memory + 0x1000, &rgb565, 2);
    vi_convert_cpu(0x1000, 1, 1, 2, 1, 0, 4, out);
    CHECK(out[0] == 0xFF0000FFu, "RGB565 red came out %08x", out[0]);
    { /* 2 bytes a pixel widen in place without losing any: a row of different pixels */
        u16 px[16];
        int i;
        for (i = 0; i < 16; i++) px[i] = (u16)(i << 11);
        memcpy(memory + 0x2000, px, sizeof px);
        vi_convert_cpu(0x2000, 16, 1, 32, 1, 0, 4, out);
        for (i = 0; i < 16; i++) CHECK((out[i] & 0xFF) == (u32)(i << 3 | i >> 2), "pixel %d came out %08x", i, out[i]);
    }
    { /* block linear: pixel (x, y) from where bl_offset puts it */
        u32 x, y, ok = 1;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                u32 v = 0x01000000u * (x + 16 * y) | 0xFF;
                memcpy(memory + 0x4000 + bl_offset(x * 4, y, 0, 64, 16, 0, 0), &v, 4);
            }
        vi_convert_cpu(0x4000, 16, 16, 64, NV_LAYOUT_BLOCK_LINEAR, 0, 1, out);
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) ok &= out[x + 16 * y] == (0x01000000u * (x + 16 * y) | 0xFF);
        CHECK(ok, "a block linear buffer came out of order");
    }
}

int main(void) {
    i32 crop[4] = {0, 0, 0, 0};
    clock_start();
    ticks();
    pacing();
    convert();
    test_renderer.present_buffer = test_present;
    vi_show_buffer(7, 0x40, 1, 1, 4, 1, 0, 1, crop, 0);
    vi_show_buffer(8, 0, 1, 1, 4, 1, 0, 1, crop, 0);
    CHECK(presented == 0x1040 && g_frames == 1, "presented %llx, %d frames", (unsigned long long)presented,
          g_frames);
    if (failures) {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("runtime present: %u checks passed\n", checks);
    return 0;
}
