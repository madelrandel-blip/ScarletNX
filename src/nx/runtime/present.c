/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vi's buffers on the display, and the game's pace: how long vi holds back the game's next buffer */
#include "nx/runtime/main.h"
#include "nx/runtime/cheats.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/vi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

int g_frames;

void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform) {
    u64 a = nvmap_addr_by_id(nvmap_id);
    if (!a) {
        WARN_LIMIT(5, "vi: buffer of nvmap id %u has no memory, not shown\n", nvmap_id);
        return;
    }
    /* the flips are the display's: the guest's buffer is never turned */
    g_renderer->present_buffer(a + offset, w, h, pitch, layout, bhl, fmt, crop, transform);
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
    cheats_frame(); /* the cheats run once per frame shown, as dmnt's run on the console */
}

/* Several threads may queue frames, so the schedule is under a lock; the wait itself is outside it. Called on the
   game's thread from vi's DequeueBuffer: the GPU thread never waits for the display. As Ryujinx's SurfaceFlinger
   paces a swap interval of 0, the game's pace with interval 0 does not wait at all */
static PMutex g_pace_lock = PMUTEX_INIT;
static u64 g_pace_last; /* when the frame before was due, 0 before the first */

void present_pace(u32 interval) {
    int fps = __atomic_load_n(&g_opt_fps, __ATOMIC_RELAXED);
    u64 refresh, period, now, next;
    if (!fps || (fps == SNX_FPS_GAME && !interval)) return;
    refresh = 1000000000u / (fps == SNX_FPS_GAME ? (u64)VI_REFRESH_HZ : (u64)fps);
    period = fps == SNX_FPS_GAME ? refresh * interval : refresh;
    pm_lock(&g_pace_lock);
    now = plat_ns();
    next = g_pace_last + period;
    /* a schedule more than a refresh behind (a load, a pause) starts again from now: no burst to catch up */
    if (!g_pace_last || now > next + refresh) next = now;
    g_pace_last = next;
    pm_unlock(&g_pace_lock);
    if (next > now) plat_sleep_ns((i64)(next - now));
}

/* ---- buffers no render target holds, read on the CPU ---- */

enum { NV_LAYOUT_BLOCK_LINEAR = 3 }; /* libnx's NvLayout: 1 pitch, 2 tiled, 3 block linear */

/* Android's pixel formats as vi gets them: 1 RGBA8888, 2 RGBX8888, 4 RGB565, 5 BGRA8888, 6 RGBA5551, 7 RGBA4444 */
static u32 bytes_per_pixel(u32 fmt) { return fmt == 4 || fmt == 6 || fmt == 7 ? 2 : 4; }
static u32 expand(u32 v, unsigned bits) { return bits == 4 ? v * 17 : bits == 5 ? v << 3 | v >> 2 : v << 2 | v >> 4; }
static u32 to_rgba8(u32 fmt, u32 p) {
    switch (fmt) {
        case 2: return p | 0xFF000000u;
        case 4:
            return expand(p >> 11 & 31, 5) | expand(p >> 5 & 63, 6) << 8 | expand(p & 31, 5) << 16 | 0xFF000000u;
        case 5: return (p & 0xFF00FF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16;
        case 6:
            return expand(p >> 11 & 31, 5) | expand(p >> 6 & 31, 5) << 8 | expand(p >> 1 & 31, 5) << 16 |
                   (p & 1 ? 0xFF000000u : 0);
        case 7:
            return expand(p >> 12 & 15, 4) | expand(p >> 8 & 15, 4) << 8 | expand(p >> 4 & 15, 4) << 16 |
                   expand(p & 15, 4) << 24;
        default: return p;
    }
}

/* Each row's pixels are read into the end of out's row block and widened forward in place: a 2-byte pixel i is read
   from beyond where pixel i is written, so no temporary buffer is needed */
void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out) {
    u32 bpp = bytes_per_pixel(fmt), row_bytes = w * bpp, stride = pitch > row_bytes ? pitch : row_bytes;
    u32 rows = layout == NV_LAYOUT_BLOCK_LINEAR ? (h + (8u << bhl) - 1) & ~((8u << bhl) - 1) : h;
    u32 x, y;
    if (!w || !h) return;
    if (a >= AS_SIZE || (u64)stride * rows > AS_SIZE - a) {
        WARN_LIMIT(5, "vi: buffer at %llx runs outside guest memory, not shown\n", (unsigned long long)a);
        memset(out, 0, (size_t)w * h * 4);
        return;
    }
    for (y = 0; y < h; y++) {
        u8* row = (u8*)(out + (size_t)y * w);
        u8* in = row + (size_t)w * 4 - row_bytes;
        if (layout == NV_LAYOUT_BLOCK_LINEAR)
            for (x = 0; x < row_bytes; x += 16) {
                u32 n = row_bytes - x < 16 ? row_bytes - x : 16;
                memcpy(in + x, g_mem + a + bl_offset(x, y, 0, stride, h, bhl, 0), n);
            }
        else
            memcpy(in, g_mem + a + (u64)y * stride, row_bytes);
        for (x = 0; x < w; x++) {
            u32 p;
            if (bpp == 2) {
                u16 v;
                memcpy(&v, in + 2 * x, 2);
                p = v;
            } else
                memcpy(&p, in + 4 * x, 4);
            p = to_rgba8(fmt, p);
            memcpy(row + 4 * x, &p, 4);
        }
    }
}
