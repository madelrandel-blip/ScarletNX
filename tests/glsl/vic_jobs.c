/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The VIC shader against vic.c's CPU composition, byte for byte: random jobs through vic_execute, from the config
   and registers a game writes, each handed to a stand-in renderer that packs it as gpu/vk/vic.c does
   (vic_gpu_params, the planes in two buffers), runs the shader's body on the CPU (tests/glsl/vic_glsl.cpp) in both of
   its builds, and declines, so the CPU composes the same job in guest memory after it. The jobs cover every output
   format, NV12 and NV21 inputs, pitch linear (word aligned and not) and block linear planes of every block height,
   up to eight slots overlapping in any order, odd source columns and rows (RGBA out), target rectangles, crops,
   planes cut short by the end of their mapping or unmapped, and colour matrices with their shifts and clamps. */
#include "nx/gpu/video/vic.c"

void vic_glsl_run(const void* params, const u32* inputs, u32* outputs, int unaligned);

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s (job %u)\n", __FILE__, __LINE__, #x, g_job);                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ guest memory: two mappings with a hole between them, so a plane
   near the end of either is cut short (plane_map maps its rows alone, or nothing) */
#define BASE    0x1000000ull
#define IN_SIZE (2u << 20) /* the inputs and the config */
#define OUT_AT  0x220000u  /* the outputs' mapping, after the hole */
#define ARENA   (OUT_AT + (2u << 20))
static u8 arena[ARENA];
u8* iova_ptr(u64 iova, u64 len) {
    u64 o = iova - BASE;
    if (iova < BASE || !len) return NULL;
    if (o + len <= IN_SIZE || (o >= OUT_AT && o + len <= ARENA)) return arena + o;
    return NULL;
}
/* gpu/gpu.c's, which the shader's xpart and ypart split in two */
u64 bl_offset(u32 x, u32 y, u32 z, u32 wb, u32 h, u32 bh, u32 bd) {
    u64 gobs_x = (wb + 63) >> 6, gy = y >> 3;
    (void)z, (void)h, (void)bd;
    return ((u64)(x >> 6) << (9 + bh)) + ((x & 32) << 3) + ((x & 16) << 1) + (x & 15) +
           ((gy >> bh) * gobs_x) * ((u64)512 << bh) + ((gy & ((1u << bh) - 1)) << 9) + (((y & 7) >> 1) << 6) +
           ((y & 1) << 4);
}
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}

static u32 rng = 0x2545F491u;
static u32 rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static u32 below(u32 n) { return n ? rnd() % n : 0; }

/* ------------------------------------------------ the stand-in renderer: both builds of the shader on the job */
static unsigned g_job;
static u8 *g_ol, *g_oc; /* the job's output planes in guest memory, and their sizes */
static u64 g_ol_lim, g_oc_lim;
static u32 g_buf[2][2][(2u << 20) / 4]; /* [build][inputs, outputs] */
static u32 g_oc_base;
static int g_ran[2];
static unsigned g_declined, g_unaligned, g_composed, g_fmt[0x45], g_matrix, g_odd, g_blk, g_multi;

static int shader_compose(const VicJob* j) {
    VicGpuParams p;
    u32 in_base[16] = {0}, at = sizeof p, s, b, n = 0;
    for (s = 0; s < 8; s++) {
        if (!j->slots[s].on) continue;
        n++;
        g_matrix += j->slots[s].mat_enable != 0;
        g_odd += !!((j->slots[s].sx0 - j->slots[s].dx0) & 1);
        g_blk += j->slots[s].iy.blk;
        in_base[2 * s] = at = (at + 15) & ~15u;
        at += (u32)j->slots[s].iy.lim;
        in_base[2 * s + 1] = at = (at + 3 + 4 * below(4)) & ~3u; /* any word aligned base */
        at += (u32)j->slots[s].ic.lim;
    }
    at = ((at + 3) & ~3u) + 4; /* as gpu/vk/vic.c pads it */
    g_multi += n > 1;
    g_oc_base = ((u32)j->oy.lim + 3 + 4 * below(4)) & ~3u;
    CHECK(at + 8 <= sizeof g_buf[0][0] && g_oc_base + j->oc.lim + 8 <= sizeof g_buf[0][1]);
    g_ol = j->oy.p, g_ol_lim = j->oy.lim, g_oc = j->oc.p, g_oc_lim = j->oc.p ? j->oc.lim : 0;
    g_ran[0] = g_ran[1] = 0;
    if (!vic_gpu_params(j, &p, in_base, g_oc_base)) {
        g_declined++;
        return 0;
    }
    for (b = 0; b < 2; b++) {
        u8 *in = (u8*)g_buf[b][0], *out = (u8*)g_buf[b][1];
        if (!b && !vic_gpu_aligned(j)) continue;
        memset(in, 0xA5, at); /* what isn't a plane's isn't read */
        memcpy(in, &p, sizeof p);
        for (s = 0; s < 8; s++) {
            if (!j->slots[s].on) continue;
            memcpy(in + in_base[2 * s], j->slots[s].iy.p, j->slots[s].iy.lim);
            memcpy(in + in_base[2 * s + 1], j->slots[s].ic.p, j->slots[s].ic.lim);
        }
        memset(out, 0x5A, g_oc_base + g_oc_lim + 8);
        memcpy(out, g_ol, g_ol_lim);
        if (g_oc) memcpy(out + g_oc_base, g_oc, g_oc_lim);
        vic_glsl_run(&p, g_buf[b][0], g_buf[b][1], b);
        g_ran[b] = 1;
    }
    g_unaligned += !g_ran[0];
    g_composed++;
    g_fmt[j->fmt]++;
    return 0;
}
static const Renderer stand_in = {.name = "stand-in", .vic_compose = shader_compose};
const Renderer* g_renderer = &stand_in;
int g_renderer_up = 1;

/* ------------------------------------------------ a random job's config and registers */
#define CFG 0x0000u
static u32 regs[0x800 / 4];
static void w32(u32 off, u32 v) { memcpy(arena + off, &v, 4); }
static void w64(u32 off, u64 v) { memcpy(arena + off, &v, 8); }
static u64 f20(int v) { return (u64)(u32)v & 0xFFFFF; }
/* BT.601 limited range in the 10 bit domain, S12.8 */
static const int M601[3][4] = {
    {298, 0, 409, -(298 * 64 + 409 * 512)},
    {298, -100, -208, -(298 * 64) + (100 + 208) * 512},
    {298, 516, 0, -(298 * 64 + 516 * 512)},
};

/* a 256 byte aligned place for n bytes in [lo, hi), and now and then across hi when the mapping ends there */
static u32 place(u32 lo, u32 hi, u32 n, int edge) {
    if (edge && below(10) == 0) return (hi - n / 2 - 256) & ~255u;
    return (lo + below(hi - lo - n - 256)) & ~255u;
}
/* the surface config (+0x20 or +0x40) and padded sizes for a w x h surface of bpp bytes a pixel, block linear or
   pitch linear, its rows padded or not, which for pitch linear can leave them unaligned to words. Its planes go in
   [at_lo, at_hi): apart for an output, whose planes the CPU writes one pixel at a time, anywhere for an input */
static void surface(u32 off, u32 fmt, u32 w, u32 h, u32 bpp, u32 at_lo, u32 at_hi, int out, u32 reg_y, u32 reg_c) {
    u32 blk = below(2), bh = below(5), lw = w * bpp + (below(2) ? below(70) : 0),
        cw = (w + 1) / 2 + below(3) * below(40);
    u32 ly = 0, cy = 0, n;
    w64(CFG + off, (u64)fmt | (u64)blk << 11 | (u64)bh << 15 | (u64)(w - 1) << 32 | (u64)(h - 1) << 46);
    w64(CFG + off + 8, (u64)(lw / bpp - 1) | (u64)(h - 1) << 14 | (u64)(cw - 1) << 32 | (u64)((h + 1) / 2 - 1) << 46);
    n = (lw + 255) * (h + 128); /* past either layout's extent */
    if (!out) {
        ly = place(at_lo, at_hi, n, 1);
        cy = place(at_lo, at_hi, n, 1);
    } else if (below(2)) {
        ly = place(at_lo, (at_lo + at_hi) / 2, n, 0);
        cy = place((at_lo + at_hi) / 2, at_hi, n, 1);
    } else {
        cy = place(at_lo, (at_lo + at_hi) / 2, n, 0);
        ly = place((at_lo + at_hi) / 2, at_hi, n, 1);
    }
    if (below(25) == 0) ly = 0; /* unmapped */
    if (below(25) == 0) cy = 0;
    regs[reg_y] = ly ? (u32)((BASE + ly) >> 8) : 0;
    regs[reg_c] = cy ? (u32)((BASE + cy) >> 8) : 0;
}
static void job_random(void) {
    static const u32 fmts[5] = {0x1F, 0x20, 0x23, 0x43, 0x44};
    u32 fmt = fmts[below(5)], sw = 1 + below(160), sh = 1 + below(72), s;
    u32 yuv = fmt >= 0x43, tl = below(2) ? 0 : below(sw + 8), tt = below(2) ? 0 : below(sh + 4);
    u32 tr = below(2) ? sw + below(9) : tl + below(sw), tb = below(2) ? sh + below(5) : tt + below(sh);
    memset(regs, 0, sizeof regs);
    memset(arena + CFG, 0, CFG_SIZE);
    w64(CFG + 0x10, (u64)below(1024) << 6 | (u64)below(1024) << 16 | (u64)below(1024) << 26 | (u64)below(1024) << 36);
    w32(CFG + 0x18, (tl & 0x3FFF) | (tr & 0x3FFF) << 16);
    w32(CFG + 0x1C, (tt & 0x3FFF) | (tb & 0x3FFF) << 16);
    surface(0x20, fmt, sw, sh, yuv ? 1 : 4, OUT_AT, ARENA, 1, 0x720 / 4, 0x724 / 4);
    if (!yuv) regs[0x724 / 4] = 0;
    for (s = 0; s < 8; s++) {
        u32 sl = CFG + SLOT_BASE + SLOT_SIZE * s, k, iw = 1 + below(200), ih = 1 + below(100);
        u32 sx0 = below(iw + 4), sx1 = sx0 + below(iw + 8), sy0 = below(ih + 4), sy1 = sy0 + below(ih + 8);
        u32 dx0 = below(sw + 8), dy0 = below(sh + 4), dx1, dy1;
        u32 lo = below(1024), hi = below(1024);
        if (below(3) == 0) continue;
        /* mostly as large as the source, a crop now and then */
        dx1 = below(6) ? dx0 + (sx1 - sx0) : dx0 + below(sw);
        dy1 = below(6) ? dy0 + (sy1 - sy0) : dy0 + below(sh);
        if (below(3) == 0) sx0 = sy0 = dx0 = dy0 = 0, sx1 = dx1 = sw - 1, sy1 = dy1 = sh - 1, iw = sw, ih = sh;
        if (below(12) && lo > hi) k = lo, lo = hi, hi = k; /* lo above hi now and then: the shader declines it */
        w64(sl + 0x00, 1);
        w64(sl + 0x10, (u64)lo | (u64)hi << 10 | (u64)below(1024) << 32);
        w64(sl + 0x20, (u64)(sx0 << 16 | below(0x10000)) | (u64)(sx1 << 16 | below(0x10000)) << 32);
        w64(sl + 0x28, (u64)(sy0 << 16 | below(0x10000)) | (u64)(sy1 << 16 | below(0x10000)) << 32);
        w64(sl + 0x30,
            (u64)(dx0 & 0x3FFF) | (u64)(dx1 & 0x3FFF) << 16 | (u64)(dy0 & 0x3FFF) << 32 | (u64)(dy1 & 0x3FFF) << 48);
        surface(sl - CFG + 0x40, below(20) ? 0x43 + below(2) : 0x20, iw, ih, 1, 0x1000, IN_SIZE, 0,
                (0x400 + 0x60 * s) / 4, (0x404 + 0x60 * s) / 4);
        if (below(2)) {
            int m[3][4], r, c, sh_ = (int)below(4);
            for (r = 0; r < 3; r++)
                for (c = 0; c < 4; c++) m[r][c] = below(3) ? M601[r][c] << sh_ : (int)(rnd() & 0xFFFFF) - 0x80000;
            for (k = 0; k < 4; k++)
                w64(sl + 0x60 + 8 * k, f20(m[0][k]) | f20(m[1][k]) << 20 | f20(m[2][k]) << 40 |
                                           (k == 0 ? (u64)(below(3) ? sh_ : below(16)) << 60 : 0) |
                                           (k == 1 ? 1ull << 63 : 0));
        }
    }
    regs[0x708 / 4] = (u32)((BASE + CFG) >> 8);
}

/* the output planes after the CPU composed them: both builds' must be the same bytes, and nothing around the planes
   may have been written (in gpu/vk/vic.c's buffer that is the other plane, or past the binding) */
static void compare(void) {
    int b;
    u32 i;
    for (b = 0; b < 2; b++) {
        const u8* out = (const u8*)g_buf[b][1];
        if (!g_ran[b]) continue;
        CHECK(memcmp(out, g_ol, g_ol_lim) == 0);
        if (g_oc) CHECK(memcmp(out + g_oc_base, g_oc, g_oc_lim) == 0);
        for (i = (u32)g_ol_lim; i < g_oc_base; i++) CHECK(out[i] == 0x5A);
        for (i = g_oc_base + (u32)g_oc_lim; i < g_oc_base + g_oc_lim + 8; i++) CHECK(out[i] == 0x5A);
    }
}

int main(int argc, char** argv) {
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 3000, i;
    for (i = 0; i < ARENA; i++) arena[i] = (u8)rnd();
    for (g_job = 0; g_job < n; g_job++) {
        g_ran[0] = g_ran[1] = 0;
        job_random();
        vic_execute(regs);
        compare();
    }
    printf(
        "%u jobs composed (%u ABGR, %u ARGB, %u XBGR, %u NV21, %u NV12), %u by the unaligned build alone, %u declined; "
        "slots: %u with the matrix, %u with an odd source column, %u block linear; %u jobs of several slots; "
        "%u checks\n",
        g_composed, g_fmt[0x1F], g_fmt[0x20], g_fmt[0x23], g_fmt[0x43], g_fmt[0x44], g_unaligned, g_declined, g_matrix,
        g_odd, g_blk, g_multi, checks);
    /* the random jobs must have reached every case */
    CHECK(g_fmt[0x1F] && g_fmt[0x20] && g_fmt[0x23] && g_fmt[0x43] && g_fmt[0x44]);
    CHECK(g_unaligned && g_declined && g_matrix && g_odd && g_blk && g_multi);
    return 0;
}
