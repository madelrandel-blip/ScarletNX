/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host1x/vic.cpp, vic.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* VIC: composes the enabled slots' NV12 (or NV21) surfaces, each read from guest memory where NVDEC wrote it, onto
   the output surface, over the background colour the target rectangle is filled with. The renderer composes it as
   a compute shader (vic.comp, through the renderer's vic_compose) once it is up; the CPU composes in guest memory
   when it declines, or when SNX_NO_VIC_GPU is set.

   The pixel pipeline is Eden's: a sample becomes a 10 bit pixel, Y, U and V in R, G and B (the pair's chroma for
   both of its pixels) and the slot's planar alpha in A. With the slot's colour matrix enabled each channel is
   ((R*c0 + G*c1 + B*c2) >> shift + c3) >> 8, clamped to the slot's soft clamp (the alpha stays out of both);
   without it the samples are copied as they are, VIC doing no YUV to RGB of its own. A later slot overwrites an
   earlier one, with no blending, and the output is 8 bit.

   The config VIC reads (0x610 bytes at register 0x708, iova >> 8), by byte offset:
     0x10  u64 background: alpha bits 6-15, R 16-25, G 26-35, B 36-45
     0x18  u32 target rectangle left 0-13, right 16-29; 0x1C top, bottom the same (inclusive)
     0x20  u64 output surface: format 0-6, block kind 11-14 (0 pitch linear), block height log2 15-18,
           width - 1 32-45, height - 1 46-59
     0x28  u64 output padded sizes: luma width - 1 0-13, height - 1 14-27, chroma the same from 32
     0x90  8 slots of 0xB0 bytes:
           +0x00 bit 0 enable; +0x10 soft clamp low 0-9, high 10-19, planar alpha 32-41;
           +0x20 source left 0-29 and right 32-61, 16.16 fixed inclusive; +0x28 top and bottom the same;
           +0x30 destination left 0-13, right 16-29, top 32-45, bottom 48-61, inclusive;
           +0x40 surface config and +0x48 padded sizes, laid out as the output's;
           +0x60 colour matrix: four u64 of three signed 20 bit coefficients (columns c0..c3 of rows R, G, B),
                 the shift in bits 60-63 of the first, the enable in bit 63 of the second
   A slot's planes are at registers 0x400 + slot * 0x60 (luma) and + 4 (chroma), the output's at 0x720 and 0x724 */
#include "nx/gpu/video/host1x_int.h"
#include "nx/gpu/video/vic.h"
#include "nx/gpu/renderer.h"

#define CFG_SIZE  0x610u
#define SLOT_BASE 0x90u
#define SLOT_SIZE 0xB0u
#define FMT_ABGR  0x1Fu /* A8B8G8R8 */
#define FMT_ARGB  0x20u /* A8R8G8B8 */
#define FMT_XBGR  0x23u /* X8B8G8R8 */
#define FMT_NV21  0x43u /* Y8___U8V8_N420: V in each chroma pair's even byte (NVIDIA names bytes from the high one) */
#define FMT_NV12  0x44u /* Y8___V8U8_N420 */

static u32 bits(u64 v, u32 lo, u32 n) { return (u32)(v >> lo) & ((1u << n) - 1); }
static u32 min_u32(u32 a, u32 b) { return a < b ? a : b; }
static int sext20(u64 v) { return (int)(u32)(v & 0xFFFFF) - (int)((v & 0x80000) << 1); }

/* a plane's row in bytes: the padded width the config names, as it is (rounding it turned a width that isn't a
   multiple of 16 green), unless that is short of the surface's row */
static u32 row_bytes(u32 padded, u32 need, u32 blk) {
    if (padded >= need) return padded;
    return blk ? need : (need + 255) & ~255u;
}
/* the plane at iova: rows pitch bytes apart, or a block linear surface pitch bytes wide. It is mapped for its whole
   extent (whole GOBs wide, whole blocks high), or for its rows alone when the buffer ends inside the last block
   row. 0 when unmapped */
static int plane_map(VicPlane* pl, u64 iova, u32 pitch, u32 rows, u32 blk, u32 bh) {
    u64 gw = (u64)(pitch + 63) / 64 * 64, br = 8ull << bh, ext;
    memset(pl, 0, sizeof *pl);
    if (!iova || !pitch || !rows) return 0;
    ext = blk ? gw * ((rows + br - 1) / br * br) : (u64)pitch * rows;
    pl->p = iova_ptr(iova, ext);
    if (!pl->p && blk) pl->p = iova_ptr(iova, ext = gw * rows);
    if (!pl->p) return 0;
    pl->lim = ext;
    pl->pitch = pitch;
    pl->h = rows;
    pl->blk = blk != 0;
    pl->bh = bh;
    return 1;
}
static u64 plane_off(const VicPlane* p, u32 x, u32 y) {
    return p->blk ? bl_offset(x, y, 0, p->pitch, p->h, p->bh, 0) : (u64)y * p->pitch + x;
}
/* bytes past the mapping read as 0 and take no writes */
static u32 plane_rd(const VicPlane* p, u32 x, u32 y) {
    u64 o = plane_off(p, x, y);
    return o < p->lim ? p->p[o] : 0;
}
static void plane_wr(const VicPlane* p, u64 o, u8 v) {
    if (o < p->lim) p->p[o] = v;
}

/* the output surface, its planes and the target rectangle. 0 when the job can't be composed */
static int job_output(VicJob* j, const u8* cfg, const u32* r) {
    u64 sc = rd64(cfg, 0x20), pad = rd64(cfg, 0x28), bg = rd64(cfg, 0x10);
    u32 blk = bits(sc, 11, 4), bh = bits(sc, 15, 4), bpp, yuv, tl, tr, tt, tb;
    j->fmt = bits(sc, 0, 7);
    j->sw = bits(sc, 32, 14) + 1;
    j->sh = bits(sc, 46, 14) + 1;
    switch (j->fmt) {
        case FMT_ABGR:
        case FMT_ARGB:
        case FMT_XBGR: bpp = 4, yuv = 0; break;
        case FMT_NV21:
        case FMT_NV12: bpp = 1, yuv = 1; break;
        default: WARN_LIMIT(4, "video: VIC output format %#x isn't composed\n", j->fmt); return 0;
    }
    if (!plane_map(&j->oy, (u64)r[0x720 / 4] << 8, row_bytes((bits(pad, 0, 14) + 1) * bpp, j->sw * bpp, blk), j->sh,
                   blk, bh)) {
        WARN_LIMIT(8, "video: VIC output surface isn't mapped, job skipped\n");
        return 0;
    }
    if (yuv && !plane_map(&j->oc, (u64)r[0x724 / 4] << 8, row_bytes((bits(pad, 32, 14) + 1) * 2, (j->sw + 1) / 2 * 2, blk),
                          (j->sh + 1) / 2, blk, bh))
        WARN_LIMIT(4, "video: VIC output chroma isn't mapped, only the luma is written\n");
    tl = bits(rd32(cfg, 0x18), 0, 14);
    tr = bits(rd32(cfg, 0x18), 16, 14);
    tt = bits(rd32(cfg, 0x1C), 0, 14);
    tb = bits(rd32(cfg, 0x1C), 16, 14);
    if (yuv) { /* a chroma pair starts on an even pixel and row; RGBA keeps an odd start */
        tl &= ~1u;
        tt &= ~1u;
    }
    if (tr >= tl && tb >= tt && tl < j->sw && tt < j->sh) {
        j->tx0 = tl;
        j->ty0 = tt;
        j->tw = min_u32(tr, j->sw - 1) - tl + 1;
        j->th = min_u32(tb, j->sh - 1) - tt + 1;
    }
    j->bg[0] = bits(bg, 16, 10);
    j->bg[1] = bits(bg, 26, 10);
    j->bg[2] = bits(bg, 36, 10);
    j->bg[3] = bits(bg, 6, 10);
    return 1;
}
/* slot s resolved: its rectangles cropped to each other (VIC scales, this doesn't), clipped to the target and the
   input, its planes mapped. Off when nothing of it composes */
static void slot_resolve(VicJob* j, const u8* sl, const u32* sr, u32 s) {
    VicSlot* o = &j->slots[s];
    u64 sc = rd64(sl, 0x40), pad = rd64(sl, 0x48), dst = rd64(sl, 0x30);
    u32 fmt = bits(sc, 0, 7), blk = bits(sc, 11, 4), bh = bits(sc, 15, 4), iw = bits(sc, 32, 14) + 1;
    u32 ih = bits(sc, 46, 14) + 1, yuv = j->fmt == FMT_NV12 || j->fmt == FMT_NV21, k, w, h, cut;
    u32 sx0 = bits(rd64(sl, 0x20), 0, 30) >> 16, sx1 = bits(rd64(sl, 0x20), 32, 30) >> 16;
    u32 sy0 = bits(rd64(sl, 0x28), 0, 30) >> 16, sy1 = bits(rd64(sl, 0x28), 32, 30) >> 16;
    u32 dx0 = bits(dst, 0, 14), dx1 = bits(dst, 16, 14), dy0 = bits(dst, 32, 14), dy1 = bits(dst, 48, 14);
    u64 m[4];
    memset(o, 0, sizeof *o);
    if (!(rd64(sl, 0) & 1)) return;
    if (fmt != FMT_NV12 && fmt != FMT_NV21) {
        WARN_LIMIT(4, "video: VIC slot format %#x isn't composed\n", fmt);
        return;
    }
    if (yuv) {
        sx0 &= ~1u;
        sy0 &= ~1u;
        dx0 &= ~1u;
        dy0 &= ~1u;
    }
    if (sx1 < sx0 || sy1 < sy0 || dx1 < dx0 || dy1 < dy0 || !j->tw || !j->th) return;
    w = sx1 - sx0 + 1;
    h = sy1 - sy0 + 1;
    if (w != dx1 - dx0 + 1 || h != dy1 - dy0 + 1) {
        WARN_LIMIT(4, "video: VIC slot %u scales %ux%u to %ux%u: cropped instead\n", s, w, h, dx1 - dx0 + 1,
                   dy1 - dy0 + 1);
        w = min_u32(w, dx1 - dx0 + 1);
        h = min_u32(h, dy1 - dy0 + 1);
    }
    /* the target rectangle (inside the surface) cuts the source by as much as the destination */
    if (dx0 < j->tx0) {
        if ((cut = j->tx0 - dx0) >= w) return;
        sx0 += cut, dx0 += cut, w -= cut;
    }
    if (dy0 < j->ty0) {
        if ((cut = j->ty0 - dy0) >= h) return;
        sy0 += cut, dy0 += cut, h -= cut;
    }
    if (dx0 >= j->tx0 + j->tw || dy0 >= j->ty0 + j->th || sx0 >= iw || sy0 >= ih) return;
    w = min_u32(min_u32(w, j->tx0 + j->tw - dx0), iw - sx0);
    h = min_u32(min_u32(h, j->ty0 + j->th - dy0), ih - sy0);
    if (!plane_map(&o->iy, (u64)sr[0] << 8, row_bytes(bits(pad, 0, 14) + 1, iw, blk), ih, blk, bh) ||
        !plane_map(&o->ic, (u64)sr[1] << 8, row_bytes((bits(pad, 32, 14) + 1) * 2, (iw + 1) / 2 * 2, blk),
                   (ih + 1) / 2, blk, bh)) {
        WARN_LIMIT(8, "video: VIC slot %u's surface isn't mapped, slot skipped\n", s);
        memset(o, 0, sizeof *o);
        return;
    }
    for (k = 0; k < 4; k++) m[k] = rd64(sl, 0x60 + 8 * k);
    for (k = 0; k < 12; k++) o->mat[k / 4][k % 4] = sext20(m[k % 4] >> (20 * (k / 4)));
    o->mat_shift = (int)(m[0] >> 60);
    o->mat_enable = (int)(m[1] >> 63);
    o->alpha = bits(rd64(sl, 0x10), 32, 10);
    o->clamp_lo = bits(rd64(sl, 0x10), 0, 10);
    o->clamp_hi = bits(rd64(sl, 0x10), 10, 10);
    o->vu = fmt == FMT_NV21;
    o->sx0 = sx0;
    o->sy0 = sy0;
    o->dx0 = dx0;
    o->dy0 = dy0;
    o->w = w;
    o->h = h;
    o->on = 1;
}

/* the composition in guest memory, as vic.comp does it */
static void slot_pixel(const VicSlot* sl, u32 x, u32 y, u32 px[4]) {
    u32 sx = sl->sx0 + (x - sl->dx0), sy = sl->sy0 + (y - sl->dy0), kc = sx & ~1u, c;
    int q[3];
    q[0] = (int)(plane_rd(&sl->iy, sx, sy) << 2);
    q[1] = (int)(plane_rd(&sl->ic, kc + sl->vu, sy >> 1) << 2);
    q[2] = (int)(plane_rd(&sl->ic, kc + 1 - sl->vu, sy >> 1) << 2);
    for (c = 0; c < 3; c++) {
        int v = q[c];
        if (sl->mat_enable) {
            v = q[0] * sl->mat[c][0] + q[1] * sl->mat[c][1] + q[2] * sl->mat[c][2];
            v = ((v >> sl->mat_shift) + sl->mat[c][3]) >> 8;
            v = v < (int)sl->clamp_lo ? (int)sl->clamp_lo : v > (int)sl->clamp_hi ? (int)sl->clamp_hi : v;
        }
        px[c] = (u32)v;
    }
    px[3] = sl->alpha;
}
static void compose_cpu(const VicJob* j) {
    u32 x, y, s, k, yuv = j->fmt == FMT_NV12 || j->fmt == FMT_NV21, vu = j->fmt == FMT_NV21;
    for (y = 0; y < j->sh; y++)
        for (x = 0; x < j->sw; x++) {
            u32 px[4];
            int covered = x >= j->tx0 && x - j->tx0 < j->tw && y >= j->ty0 && y - j->ty0 < j->th;
            memcpy(px, j->bg, sizeof px);
            for (s = 0; s < 8; s++) {
                const VicSlot* sl = &j->slots[s];
                if (!sl->on || x < sl->dx0 || x - sl->dx0 >= sl->w || y < sl->dy0 || y - sl->dy0 >= sl->h) continue;
                slot_pixel(sl, x, y, px);
                covered = 1;
            }
            if (!covered) continue;
            if (!yuv) {
                u64 o = plane_off(&j->oy, 4 * x, y);
                u32 rb = j->fmt == FMT_ARGB ? 2 : 0; /* A8R8G8B8 stores R and B the other way around */
                plane_wr(&j->oy, o + rb, (u8)(px[0] >> 2));
                plane_wr(&j->oy, o + 1, (u8)(px[1] >> 2));
                plane_wr(&j->oy, o + 2 - rb, (u8)(px[2] >> 2));
                plane_wr(&j->oy, o + 3, (u8)(px[3] >> 2));
                continue;
            }
            plane_wr(&j->oy, plane_off(&j->oy, x, y), (u8)(px[0] >> 2));
            /* a pair's chroma comes from its even pixel on the even row */
            if (!(x & 1) && !(y & 1) && j->oc.p) {
                u64 o = plane_off(&j->oc, x, y >> 1);
                for (k = 0; k < 2; k++) plane_wr(&j->oc, o + (k ^ vu), (u8)(px[1 + k] >> 2));
            }
        }
}

/* ------------------------------------------------ the shader's parameters (vic_params.glsl) */
static VicGpuPlane gpu_plane(const VicPlane* p, u32 base) {
    VicGpuPlane g;
    memset(&g, 0, sizeof g);
    g.base = base;
    if (!p->p) return g; /* lim 0: nothing is read or written */
    g.lim = (u32)p->lim;
    g.blk = p->blk;
    g.pitch = p->pitch;
    g.bsh = 9 + p->bh;
    g.ysh = 3 + p->bh;
    g.gm = ((1u << p->bh) - 1) << 9;
    g.bstride = ((p->pitch + 63) / 64) << g.bsh;
    return g;
}
int vic_gpu_params(const VicJob* j, VicGpuParams* p, const u32 in_base[16], u32 oc_base) {
    u32 yuv = j->fmt == FMT_NV12 || j->fmt == FMT_NV21, ovu = j->fmt == FMT_NV21, rb = j->fmt == FMT_ARGB ? 16 : 0;
    u32 bg[4], s, n = 0;
    VicGpuJob* g = &p->job;
    memset(p, 0, sizeof *p);
    for (s = 0; s < 4; s++) bg[s] = j->bg[s] >> 2;
    g->nv12 = yuv;
    g->rpos = rb;
    g->gpos = yuv ? 8 * ovu : 8;
    g->bpos = yuv ? 8 - 8 * ovu : 16 - rb;
    g->tx0 = j->tx0;
    g->tx1 = j->tx0 + j->tw;
    g->ty0 = j->ty0;
    g->th = j->th;
    g->gx0 = j->tx0 & ~3u;
    g->gy0 = j->ty0 & ~1u;
    g->bgw = (bg[0] << g->rpos) | (bg[1] << 8) | (bg[2] << g->bpos) | (bg[3] << 24);
    g->bgy = bg[0] * 0x01010101u;
    g->bgc = ((bg[1] << g->gpos) | (bg[2] << g->bpos)) * 0x00010001u;
    g->ol = gpu_plane(&j->oy, 0);
    g->oc = gpu_plane(&j->oc, oc_base);
    for (s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        VicGpuSlot* o = &p->slots[n];
        if (!sl->on) continue;
        /* lo above hi clamps to lo below it and to hi above, which the shader's clamp of bytes can't. And the shader
           writes the target rectangle alone, which holds every slot slot_resolve leaves on */
        if (sl->mat_enable && sl->clamp_lo > sl->clamp_hi) return 0;
        if (sl->dx0 < j->tx0 || sl->dx0 + sl->w > j->tx0 + j->tw || sl->dy0 < j->ty0 ||
            sl->dy0 + sl->h > j->ty0 + j->th)
            return 0;
        n++;
        o->dx0 = sl->dx0;
        o->dx1 = sl->dx0 + sl->w;
        o->dy0 = sl->dy0;
        o->h = sl->h;
        o->ox = sl->sx0 - sl->dx0;
        o->oy = sl->sy0 - sl->dy0;
        o->mat = sl->mat_enable != 0;
        o->msh = (u32)sl->mat_shift;
        o->ry = sl->mat[0][0] * 4, o->ru = sl->mat[0][1] * 4, o->rv = sl->mat[0][2] * 4, o->r3 = sl->mat[0][3];
        o->gy = sl->mat[1][0] * 4, o->gu = sl->mat[1][1] * 4, o->gv = sl->mat[1][2] * 4, o->g3 = sl->mat[1][3];
        o->by = sl->mat[2][0] * 4, o->bu = sl->mat[2][1] * 4, o->bv = sl->mat[2][2] * 4, o->b3 = sl->mat[2][3];
        o->lo = (int)(sl->clamp_lo >> 2);
        o->hi = (int)(sl->clamp_hi >> 2);
        o->aw = (sl->alpha >> 2) << 24;
        o->ush = 8 * sl->vu;
        o->vsh = 8 - 8 * sl->vu;
        o->cswap = sl->vu ^ ovu;
        o->iy = gpu_plane(&sl->iy, in_base[2 * s]);
        o->ic = gpu_plane(&sl->ic, in_base[2 * s + 1]);
    }
    g->nslots = n;
    return 1;
}

/* the shader's build for word aligned rows needs every plane's rows to keep a word aligned column word aligned: block
   linear rows do (bl_offset keeps the column's alignment), pitch linear ones when the pitch is a multiple of 4 */
static int plane_aligned(const VicPlane* p) { return !p->p || p->blk || !(p->pitch & 3); }
int vic_gpu_aligned(const VicJob* j) {
    u32 s;
    for (s = 0; s < 8; s++)
        if (j->slots[s].on && !(plane_aligned(&j->slots[s].iy) && plane_aligned(&j->slots[s].ic))) return 0;
    return plane_aligned(&j->oy) && plane_aligned(&j->oc);
}

/* SNX_NO_VIC_GPU set (and not 0): the CPU composes every job */
static int gpu_allowed(void) {
    static int state; /* 0 unread, 1 allowed, 2 not */
    int s = __atomic_load_n(&state, __ATOMIC_RELAXED);
    if (!s) {
        const char* e = getenv("SNX_NO_VIC_GPU");
        s = e && *e && strcmp(e, "0") ? 2 : 1;
        __atomic_store_n(&state, s, __ATOMIC_RELAXED);
    }
    return s == 1;
}

void vic_execute(const u32* r) {
    VicJob j;
    const u8* cfg = iova_ptr((u64)r[0x708 / 4] << 8, CFG_SIZE);
    u32 s;
    int any = 0;
    if (!cfg) {
        WARN_LIMIT(8, "video: VIC config isn't mapped, job skipped\n");
        return;
    }
    memset(&j, 0, sizeof j);
    if (!job_output(&j, cfg, r)) return;
    for (s = 0; s < 8; s++) {
        slot_resolve(&j, cfg + SLOT_BASE + SLOT_SIZE * s, r + (0x400 + 0x60 * s) / 4, s);
        any |= j.slots[s].on;
    }
    if (!any && !j.tw) return;
    /* the GPU once the renderer is up, as the 2D engine and the draws wait for it */
    if (g_renderer_up && g_renderer && g_renderer->vic_compose && gpu_allowed() && g_renderer->vic_compose(&j)) return;
    compose_cpu(&j);
}
