/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_3d.cpp, src/video_core/engines/draw_manager.cpp,
   src/video_core/engines/maxwell_3d.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 3D engine's methods (MAXWELL_B, 0xB197): the shadow RAM, draws and their merging into instanced draws, inline
   indices, render enable, the report semaphores and counters shared with the compute engine, macro calls, barriers.
   Register numbers are method indices (byte offset / 4) */
#include "nx/gpu/maxwell/engines_int.h"

/* ------------------------------------------------------------ timestamps */
/* The timestamp a report or semaphore carries: the guest clock when the stream reaches it, as 614.4 MHz GPU ticks
   (ns * 384 / 625), at least 16 ns past the one before so that stamps strictly increase. sead compares them with the
   CPU's tick. It is not real GPU time. The GPU thread's alone */
u64 gpu_ticks(void) {
    static u64 last;
    u64 ns = guest_ns();
    if (ns < last + 16) ns = last + 16;
    last = ns;
    return ns / 625 * 384 + ns % 625 * 384 / 625;
}

/* ------------------------------------------------------------ made-up counter values */
/* Counter reports other than samples passed after its reset carry made-up values, and a condition comparing them
   could skip a draw the game needs: a render enable over an address that last got one draws. The last 64 such
   addresses; a release or a real count written there takes it out */
#define MADEUP_N 64
static u64 g_madeup[MADEUP_N];
static u32 g_madeup_next;
static int madeup_has(u64 a) {
    u32 i;
    for (i = 0; i < MADEUP_N; i++)
        if (a && g_madeup[i] == a) return 1;
    return 0;
}
static void madeup_set(u64 a, int made) {
    u32 i;
    for (i = 0; i < MADEUP_N; i++)
        if (a && g_madeup[i] == a) {
            if (!made) g_madeup[i] = 0;
            return;
        }
    if (made && a) g_madeup[g_madeup_next++ % MADEUP_N] = a;
}

/* ------------------------------------------------------------ report semaphores */
/* one word (STRUCTURE_SIZE ONE_WORD, bit 28), or the value as 64 bits and the timestamp */
static void report_write(u64 a, u64 value, int one) {
    if (one) {
        u32 w = (u32)value;
        gpu_release_write(a, &w, 4);
    } else {
        u64 d[2];
        d[0] = value;
        d[1] = gpu_ticks();
        gpu_release_write(a, d, 16);
    }
}
/* the counters a CLEAR_REPORT_VALUE (3D 0x54C) resets, as bits by report selector */
static u32 reset_selectors(u32 v) {
    switch (v) {
        case 0x01: return 1u << 0x15 | 1u << 0x02; /* samples passed */
        case 0x02: return 1u << 0x0A | 1u << 0x0C | 1u << 0x0E | 1u << 0x10; /* ZCull stats */
        case 0x10: return 1u << 0x0B; /* transform feedback primitives written */
        case 0x12: return 1u << 0x01; /* vertices generated */
        case 0x13: return 1u << 0x03 | 1u << 0x12; /* input primitives */
        case 0x15: return 1u << 0x05; /* vertex shader invocations */
        case 0x16: return 1u << 0x1B; /* tessellation control invocations */
        case 0x17: return 1u << 0x1D; /* tessellation evaluation invocations */
        case 0x18: return 1u << 0x1F; /* tessellation evaluation primitives */
        case 0x1A: return 1u << 0x07; /* geometry shader invocations */
        case 0x1B: return 1u << 0x09; /* geometry shader primitives */
        case 0x1C: return 1u << 0x0F; /* clipper input */
        case 0x1D: return 1u << 0x11; /* clipper output */
        case 0x1E: return 1u << 0x13; /* fragment shader invocations */
        case 0x1F: return 1u << 0x12; /* generated primitives (Ryujinx and Eden disagree) */
        default: return 0;
    }
}
static void counter_reset(GpuChannel* ch, u32 v) {
    u32 mask = reset_selectors(v), s;
    for (s = 0; s < 32; s++)
        if (mask & (1u << s)) ch->counter_base[s] = g_draws;
    ch->counter_reset |= mask;
    if (v == 1 && g_renderer->samples_reset) g_renderer->samples_reset(ch); /* samples passed is counted from here */
    if (!mask) LOG_LIMIT(8, "gpu: counter reset %x\n", v);
}
static void counter_report(GpuChannel* ch, u64 a, u32 sel, u32 payload, int one) {
    u64 v;
    if (!sel) { /* the payload, as Eden finds the hardware gives */
        report_write(a, payload, one);
        madeup_set(a, 0);
        return;
    }
    if ((sel == 0x15 || sel == 0x02) && (ch->counter_reset & (1u << sel)) && g_renderer->samples_report &&
        g_renderer->samples_report(ch, a, one ? 4 : 16, gpu_ticks())) {
        madeup_set(a, 0);
        return;
    }
    /* made up: three vertices a draw for vertices generated, 1000 for the others, counted from a reset */
    if (ch->counter_reset & (1u << sel))
        v = (g_draws - ch->counter_base[sel]) * (sel == 1 ? 3 : 1000);
    else
        v = sel == 1 ? g_draws * 3 : 1000;
    report_write(a, v, one);
    madeup_set(a, 1);
}
void query_get(GpuChannel* ch, u32* r, int base, u32 v) {
    u64 a = ((u64)(r[base] & 0xFF) << 32) | r[base + 1];
    u32 payload = r[base + 2];
    int one = (v >> 28) & 1; /* bit 4 is the release's timing (after reads or writes), not its size */
    switch (v & 3) {
        case 0: /* release */
            report_write(a, payload, one);
            madeup_set(a, 0);
            break;
        case 1: /* acquire: GE (bit 16) or EQ */ gpu_park(ch, GPU_PARK_REPORT, a, payload, (v >> 16) & 1); break;
        case 2: /* report a counter, selector bits 23-27 */ counter_report(ch, a, (v >> 23) & 31, payload, one); break;
        default: LOG_LIMIT(4, "gpu: report semaphore trap %08x is ignored\n", v); break;
    }
}

/* ------------------------------------------------------------ render enable */
/* Decided when the mode (0x556) or the override (0x651) is written, as Eden's ProcessQueryCondition, and kept until
   one of them is written again */
static void render_enable_update(GpuChannel* ch) {
    const u32* r = ch->r3d;
    u64 a = ((u64)(r[0x554] & 0xFF) << 32) | r[0x555];
    u32 mode = r[0x556] & 7;
    int on = 1;
    switch (r[0x651] & 3) {
        case 1: on = 1; break; /* ALWAYS */
        case 2: on = 0; break; /* NEVER */
        default:
            switch (mode) {
                case 0: on = 0; break;
                case 1: on = 1; break;
                case 2: { /* CONDITIONAL: both words of the report's value set */
                    u32 w[2];
                    gmem_read(a, w, 8);
                    on = madeup_has(a) || (w[0] && w[1]);
                    break;
                }
                case 3: /* IF_EQUAL and IF_NOT_EQUAL: two reports' values, 16 bytes apart (their stamps don't count) */
                case 4: {
                    u64 x, y;
                    if (madeup_has(a) || madeup_has(a + 16)) {
                        on = 1;
                        break;
                    }
                    gmem_read(a, &x, 8);
                    gmem_read(a + 16, &y, 8);
                    on = (x == y) == (mode == 3);
                    break;
                }
                default: LOG_LIMIT(4, "gpu: render enable mode %u draws\n", mode); break;
            }
            break;
    }
    ch->render_off = !on;
}

/* ------------------------------------------------------------ draws */
/* the kinds of draw that merge with their own kind only */
enum { DRAW_ARRAYS, DRAW_INDEXED, DRAW_ARRAY_INSTANCED, DRAW_IB32, DRAW_IB16, DRAW_IB8 };
/* the draw held while the next ones may be more instances of it: GPU thread state, one for all channels */
static struct {
    GpuChannel* ch; /* NULL: none */
    u32 kind;
    int indexed;
    u32 topology, first, count, base_vertex, base_instance;
    u32 start, n; /* the instance it started at, and its instances */
} g_pend;
int r3d_pending_on(void) { return g_pend.ch != NULL; }
void r3d_pending_flush(void) {
    GpuChannel* ch = g_pend.ch;
    if (!ch) return;
    g_pend.ch = NULL;
    r3d_draw(ch, g_pend.indexed, g_pend.topology, g_pend.first, g_pend.count, g_pend.base_vertex,
             g_pend.base_instance + g_pend.start, g_pend.n);
}
/* a draw of instance inst: one more instance of the held draw when everything else is the same */
static void queue_draw(GpuChannel* ch, u32 kind, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex,
                       u32 inst) {
    u32 bi = ch->r3d[0x50E];
    if (g_pend.ch == ch && g_pend.kind == kind && g_pend.topology == topology && g_pend.first == first &&
        g_pend.count == count && g_pend.base_vertex == base_vertex && g_pend.base_instance == bi &&
        g_pend.start + g_pend.n == inst) {
        g_pend.n++;
        return;
    }
    r3d_pending_flush();
    g_pend.ch = ch;
    g_pend.kind = kind;
    g_pend.indexed = indexed;
    g_pend.topology = topology;
    g_pend.first = first;
    g_pend.count = count;
    g_pend.base_vertex = base_vertex;
    g_pend.base_instance = bi;
    g_pend.start = inst;
    g_pend.n = 1;
}
/* DRAW_BEGIN (0x586): the topology, and the instance: the next one (bit 26), the first (bits 26-27 clear) or the
   same */
static void draw_begin(GpuChannel* ch, u32 v) {
    if (v & (1u << 26))
        ch->instance_count++;
    else if (!(v & (1u << 27)))
        ch->instance_count = 0;
}
/* DRAW_END (0x585): the inline indices when some came, else the index buffer's range when its count is set (and
   then used up), else the vertex range */
static void draw_end(GpuChannel* ch) {
    u32* r = ch->r3d;
    u32 topology = r[0x586] & 0xFFFF;
    if (ch->inline_n) {
        r3d_pending_flush();
        r3d_draw(ch, 3, topology, 0, ch->inline_n, r[0x50D], r[0x50E] + ch->instance_count, 1);
        ch->inline_n = 0;
    } else if (r[0x5F8]) {
        queue_draw(ch, DRAW_INDEXED, 1, topology, r[0x5F7], r[0x5F8], r[0x50D], ch->instance_count);
        r[0x5F8] = 0;
    } else
        queue_draw(ch, DRAW_ARRAYS, 0, topology, r[0x35D], r[0x35E], 0, ch->instance_count);
}
/* the inline index methods' indices, widened to u32 */
#define INLINE_MAX (16u << 20)
static void inline_push(GpuChannel* ch, const u32* idx, u32 n) {
    if (ch->inline_n + n > ch->inline_cap) {
        u32 cap = ch->inline_cap ? ch->inline_cap * 2 : 1024;
        u32* b;
        if (ch->inline_n + n > INLINE_MAX) {
            WARN_LIMIT(4, "gpu: more than %u inline indices: the rest are dropped\n", INLINE_MAX);
            return;
        }
        if (cap > INLINE_MAX) cap = INLINE_MAX;
        b = (u32*)realloc(ch->inline_indices, (size_t)cap * 4);
        if (!b) {
            WARN_LIMIT(4, "gpu: no memory for %u inline indices\n", cap);
            return;
        }
        ch->inline_indices = b;
        ch->inline_cap = cap;
    }
    memcpy(ch->inline_indices + ch->inline_n, idx, (size_t)n * 4);
    ch->inline_n += n;
}
/* ------------------------------------------------------------ the shadow RAM and the merged draw */
/* the methods the shadow RAM leaves alone: the class's own (below 0x80), the window origin, and the data streams */
static int shadowed(u32 m) {
    return m >= 0x80 && m != 0x4EB && m != 0x4C1 && m != 0x57A && m != 0x57C && !(m >= 0x8E4 && m <= 0x8F3);
}
/* whether a write of v to m leaves the held draw alone: what the next instance of it rewrites, and any other register
   written with the value it has, except the methods that act */
static int keeps_draw(const u32* r, u32 m, u32 v) {
    switch (m) {
        case 0x586: return (v & (1u << 26)) != 0; /* BEGIN of the next instance */
        case 0x486:
        case 0x585:
        case 0x35D:
        case 0x35E:
        case 0x5F7:
        case 0x5F8:
        case 0x5FC:
        case 0x5FD:
        case 0x5FE: return 1;
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x6C:
        case 0x6D:
        case 0xB2:
        case 0x378:
        case 0x3DD:
        case 0x3DF:
        case 0x42B:
        case 0x4C1:
        case 0x509:
        case 0x50A:
        case 0x545:
        case 0x54C:
        case 0x556:
        case 0x57A:
        case 0x57C:
        case 0x651:
        case 0x674:
        case 0x6C3:
        case 0x8C4:
        case 0x904:
        case 0x90C:
        case 0x914:
        case 0x91C:
        case 0x924: return 0;
        default: return r[m] == v;
    }
}
static void barrier(int kind) {
    if (g_renderer->method_barrier) g_renderer->method_barrier(kind);
}
/* a macro call (0xE00 up: the even method starts a call, the odd one takes more parameters): it runs at the batch's
   last word */
static void macro_call(GpuChannel* ch, u32 m, u32 v, int last) {
    if (!ch->mme_collecting) {
        ch->mme_collecting = 1;
        ch->mme_index = (m - 0xE00) >> 1;
        ch->mme_nparams = 0;
    }
    if (ch->mme_nparams < sizeof ch->mme_params / sizeof ch->mme_params[0])
        ch->mme_params[ch->mme_nparams++] = v;
    else
        WARN_LIMIT(4, "gpu: a macro call's parameters past %u are dropped\n", ch->mme_nparams);
    if (last) {
        ch->mme_collecting = 0;
        mme_run(ch, ch->mme_index, ch->mme_nparams);
    }
}

void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last) {
    u32* r = ch->r3d;
    m &= 0xFFF;
    if (m >= 0xE00) {
        r3d_const_flush(ch);
        r[m] = v;
        macro_call(ch, m, v, last);
        return;
    }
    if (m >= 0x8E4 && m <= 0x8F3) { /* constant buffer data: the held draw read the constants before it */
        r3d_pending_flush();
        r[m] = v;
        r3d_const_upload(ch, v, last);
        return;
    }
    r3d_const_flush(ch);
    if (shadowed(m)) {
        if (ch->shadow_mode == 3)
            v = ch->r3d_shadow[m]; /* replay */
        else if (ch->shadow_mode <= 1)
            ch->r3d_shadow[m] = v; /* track, with or without the filter */
    }
    if (g_pend.ch && (g_pend.ch != ch || !keeps_draw(r, m, v))) r3d_pending_flush();
    r[m] = v;
    switch (m) {
        case 0x44: /* WAIT_FOR_IDLE */
            /* The idle wait and the texture cache invalidation below order GPU work, which method_barrier gives
               Vulkan. Neither is anything the game sees, so pending shader results don't land here: whatever reads
               guest memory lands the results over the bytes it reads (gmem_need, through gmem_ptr for textures and
               render targets), and a release lands them before the game can see it (release.c). Landing every
               result at each of them cost TotK several vk_finish waits a frame */
            barrier(4);
            break;
        case 0x45: mme_upload_ptr(ch, v); break; /* the macro code's upload address, beginning an upload batch */
        case 0x46: mme_upload(ch, v); break;
        case 0x47: ch->mme_start_ptr = v; break; /* the macro start table's upload index */
        case 0x48: mme_bind(ch, v); break;
        case 0x49: ch->shadow_mode = v & 3; break; /* SET_MME_SHADOW_RAM_CONTROL */
        case 0x6C: i2m_launch(ch, r, 0x60); break;
        case 0x6D: i2m_data(ch, v); break;
        case 0xB2: /* SYNC_INFO: the syncpoint (low 16 bits) once the work before it is done, when bit 20 is set */
            if (!(v & (1u << 20)))
                LOG_LIMIT(4, "gpu: SyncInfo %08x without bit 20 doesn't increment\n", v);
            else if (g_renderer->stream_incr)
                g_renderer->stream_incr(v & 0xFFFF);
            break;
        case 0x378: barrier(0); break; /* FRAGMENT_BARRIER */
        case 0x3DD: barrier(2); break; /* INVALIDATE_TEXTURE_DATA_CACHE */
        case 0x3DF: barrier(1); break; /* TILED_CACHE_BARRIER */
        case 0x42B: r3d_draw_texture(ch); break;
        case 0x4C1: { /* four 8-bit indices, the low byte first */
            u32 idx[4];
            idx[0] = v & 0xFF, idx[1] = (v >> 8) & 0xFF, idx[2] = (v >> 16) & 0xFF, idx[3] = v >> 24;
            inline_push(ch, idx, 4);
            break;
        }
        case 0x485: /* VERTEX_ARRAY_INSTANCE_FIRST: instance 0 of first (bits 0-15), count (16-27), topology (28-31) */
        case 0x486: /* _SUBSEQUENT: the next instance */
            if (m == 0x486)
                ch->instance_count++;
            else
                ch->instance_count = 0;
            queue_draw(ch, DRAW_ARRAY_INSTANCED, 0, v >> 28, v & 0xFFFF, (v >> 16) & 0xFFF, 0, ch->instance_count);
            break;
        case 0x509: /* INVALIDATE_SAMPLER_CACHE_NO_WFI */
        case 0x50A: /* INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI */ barrier(3); break;
        case 0x545: /* the samples-passed counter counts while bit 0 is set */
            if (g_renderer->samples_enable) g_renderer->samples_enable(ch, v & 1);
            break;
        case 0x54C: counter_reset(ch, v); break;
        case 0x556:
        case 0x651: render_enable_update(ch); break;
        case 0x57A: inline_push(ch, &v, 1); break; /* one 32-bit index */
        case 0x57C: {                              /* two 16-bit indices, the low half first */
            u32 idx[2];
            idx[0] = v & 0xFFFF, idx[1] = v >> 16;
            inline_push(ch, idx, 2);
            break;
        }
        case 0x585: draw_end(ch); break;
        case 0x586: draw_begin(ch, v); break;
        case 0x5F9: /* INDEX_BUFFER32/16/8_FIRST and _SUBSEQUENT: the index buffer's first and count in one word */
        case 0x5FA:
        case 0x5FB:
        case 0x5FC:
        case 0x5FD:
        case 0x5FE:
            if (m >= 0x5FC)
                ch->instance_count++;
            else
                ch->instance_count = 0;
            queue_draw(ch, DRAW_IB32 + (m - 0x5F9) % 3, 1, v >> 28, v & 0xFFFF, (v >> 16) & 0xFFF, r[0x50D],
                       ch->instance_count);
            break;
        case 0x674: r3d_clear(ch, v); break;
        case 0x6C3: query_get(ch, r, 0x6C0, v); break;
        case 0x8C4: r[0xD00] = 1; break; /* firmware call 4 signals its completion in shadow scratch 0 */
        case 0x904:
        case 0x90C:
        case 0x914:
        case 0x91C:
        case 0x924: r3d_cb_bind(ch, (int)((m - 0x904) >> 3), v); break;
        default: break;
    }
}

