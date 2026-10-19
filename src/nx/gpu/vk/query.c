/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's samples-passed counter (3D 0x545 SampleCounterEnable, 0x54C ResetCounter 1, a counter report of
   selector 0x15: NVN's and deko3d's occlusion queries), counted with Vulkan occlusion queries. Each channel has a
   counter of its own, as each channel's 3D context has on the console.

   The count runs from its reset. It is kept in segments: a segment is one query, begun at the first draw counted in a
   rendering pass and ended with the pass, at a reset, at a report, or at a draw of another channel or another sample
   count. A segment belongs to the channel that began it, and holds that draw's guest sample count. At a submit (and
   at a report), the results of the segments ended so far are copied into host-cached memory and their queries reset,
   in the same command buffer: once per submit, as each copy waits for its queries on the GPU. A report is a release
   (release.c) gated on the submit being recorded, which happens when anything needs the release, and at the latest
   when the GPFIFO submission ends (renderer.h stream_end): a game that reports often doesn't submit once for each.
   When it is performed, the GPU has counted, and the channel's segments since its previous report are added to the
   epoch's sum, which is its value: a game that resets once and subtracts reports keeps only the segments since its
   last report live. Whatever reads the report (the render enable's comparison, a semaphore acquire, the CPU) waits
   for that release, as for any other.

   Counting starts at a channel's first reset: deko3d and NVN turn the counter on at startup, and a game that never
   resets it never reads it, so it costs those games nothing. A report before any reset, or in an epoch that outgrew
   the ring, keeps the made-up value the front end writes (maxwell/engines.c), over which a render enable draws.

   Targets are drawn single-sampled, where the console counts each sample of a multisampled one: a segment's count is
   multiplied by its guest sample count (3D 0x574, deko3d's MsaaMode). Render targets scaled by the resolution
   setting pass that many more samples: the sum is divided back, and a count that wasn't zero stays at least 1, as an
   object behind it is visible.
   Everything here runs with vk_lock held. */
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"

#define QN    4096 /* segments the ring holds: queries in the pool, and result slots */
#define QWAIT 1024 /* a counter's reports queued and not performed yet, at most (the release queue's size) */
#define QCH   8    /* channels that can count at once */

static VkQueryPool g_qpool;
static VkBuffer g_qbuf;
static VmaAllocation g_qmem;
static u64* g_qres;   /* segment s's count at s % QN, once its copy has run */
static int g_qfailed; /* the pool couldn't be made: reports keep the made-up value */
/* each slot's segment: its counter (1 + its index in g_qc, 0 none) and its guest sample count */
static u8 g_qown[QN], g_qmul[QN];
static struct {
    u64 next;   /* segments begun */
    u64 copied; /* segments whose results have their copy recorded */
    int open;   /* the last segment's query is active in the rendering pass */
} g_q;
typedef struct {
    u32 chan;     /* the channel's id */
    int used;     /* the channel has counted */
    int disabled; /* SampleCounterEnable is off: its draws aren't counted */
    int started;  /* a reset came: counting is on from there */
    int bad;      /* the epoch outgrew the ring: its reports keep the made-up value until the next reset */
    u64 first;    /* the epoch's first segment number */
    u64 reported; /* the end of the epoch's last report queued, its first before one */
    /* the oldest segment each queued report not performed yet adds to its epoch's sum, in order: their slots aren't
       used again until then */
    u64 wait[QWAIT];
    u32 whead, wtail;
    /* the reports performed: the sum of epoch sum_first's segments up to sum_end */
    u64 sum, sum_first, sum_end;
} QCounter;
static QCounter g_qc[QCH];

static int pool_make(void) {
    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = QN};
    if (!VK_OK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &g_qpool))) {
        g_qpool = VK_NULL_HANDLE;
        return 0;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = QN * sizeof(u64),
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_qbuf, &g_qmem, &info))) {
        vkDestroyQueryPool(g_vk.dev, g_qpool, NULL);
        g_qpool = VK_NULL_HANDLE;
        return 0;
    }
    g_qres = info.pMappedData;
    /* a query is reset before its first use, outside a rendering pass */
    vk_render_end();
    rec_CmdResetQueryPool(vk_cmd(), g_qpool, 0, QN);
    if (!g_vk.feat.occlusionQueryPrecise)
        WARN("vulkan: the device's occlusion queries aren't precise: samples-passed counts may be any nonzero value\n");
    return 1;
}

/* the counter of channel chan, made if make is set; NULL when there is none (or no room for one) */
static QCounter* counter(u32 chan, int make) {
    QCounter* free_one = NULL;
    for (int i = 0; i < QCH; i++) {
        if (g_qc[i].used && g_qc[i].chan == chan) return &g_qc[i];
        if (!g_qc[i].used && !free_one) free_one = &g_qc[i];
    }
    if (!make) return NULL;
    if (!free_one) {
        LOG_LIMIT(4, "vulkan: more than %d channels count samples passed, channel %u's reports are made up\n", QCH,
                  chan);
        return NULL;
    }
    memset(free_one, 0, sizeof *free_one);
    free_one->used = 1;
    free_one->chan = chan;
    return free_one;
}
/* the oldest segment a counter's report may still add: its oldest queued report's, or its next report's */
static u64 live_from(const QCounter* c) { return c->whead != c->wtail ? c->wait[c->wtail % QWAIT] : c->reported; }
/* the oldest segment any counter may still add, whose slot can't be used again yet */
static u64 live_oldest(void) {
    u64 oldest = g_q.next;
    for (int i = 0; i < QCH; i++)
        if (g_qc[i].used && g_qc[i].started && live_from(&g_qc[i]) < oldest) oldest = live_from(&g_qc[i]);
    return oldest;
}
/* the guest sample count of an MsaaMode (3D 0x574): 1x1, 2x1, 2x2, 4x2, 4x2 D3D, 2x1 D3D, 4x4, and the 2x2 and 4x2
   virtual coverage modes */
static u32 msaa_samples(u32 mode) {
    static const u8 n[16] = {1, 2, 4, 8, 8, 2, 16, 1, 4, 4, 8, 8, 1, 1, 1, 1};
    return n[mode & 15];
}

void vk_query_draw(VkCommandBuffer cb, u32 chan, u32 msaa_mode) {
    QCounter* c = counter(chan, 0);
    u32 mul = msaa_samples(msaa_mode);
    int own = c ? (int)(c - g_qc) + 1 : 0;
    if (g_q.open) {
        u32 at = (u32)((g_q.next - 1) % QN);
        if (g_qown[at] == own && g_qmul[at] == mul) return; /* the segment open now counts this draw too */
        vk_query_pass_end(cb);
    }
    if (!c || !c->started || c->disabled || c->bad) return;
    /* the slot is still needed by a report, or its query not reset yet: this epoch counts no further */
    if (g_q.next - live_oldest() >= QN || g_q.next - g_q.copied >= QN) {
        c->bad = 1;
        LOG_LIMIT(4, "vulkan: a samples-passed count spans more than %u passes, its reports are made up\n", QN);
        return;
    }
    u32 at = (u32)(g_q.next % QN);
    g_qown[at] = (u8)own;
    g_qmul[at] = (u8)mul;
    rec_CmdBeginQuery(cb, g_qpool, at, g_vk.feat.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    g_q.next++;
    g_q.open = 1;
}
void vk_query_pass_end(VkCommandBuffer cb) {
    if (!g_q.open) return;
    rec_CmdEndQuery(cb, g_qpool, (u32)((g_q.next - 1) % QN));
    g_q.open = 0;
}
void vk_query_flush(VkCommandBuffer cb) {
    if (g_q.copied == g_q.next) return;
    while (g_q.copied < g_q.next) { /* in runs that don't wrap around the ring */
        u32 at = (u32)(g_q.copied % QN), n = QN - at;
        if (n > g_q.next - g_q.copied) n = (u32)(g_q.next - g_q.copied);
        rec_CmdCopyQueryPoolResults(cb, g_qpool, at, n, g_qbuf, (VkDeviceSize)at * sizeof(u64), sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        rec_CmdResetQueryPool(cb, g_qpool, at, n);
        g_q.copied += n;
    }
    /* for the host's reads, and for a later copy into a slot of a segment no report read */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
}

/* a segment open now of this counter ends: what comes after is another epoch's, or isn't counted */
static void end_own(const QCounter* c) {
    if (g_q.open && g_qown[(g_q.next - 1) % QN] == (u8)(c - g_qc + 1)) vk_query_pass_end(vk_cmd());
}
void vk_query_reset(u32 chan) {
    if (!g_qpool) {
        if (g_qfailed) return;
        if (!pool_make()) {
            g_qfailed = 1;
            return;
        }
    }
    QCounter* c = counter(chan, 1);
    if (!c) return;
    end_own(c);
    if (!c->started) LOG("vulkan: channel %u counts samples passed, with occlusion queries\n", chan);
    c->started = 1;
    c->bad = 0;
    c->first = c->reported = g_q.next;
}
void vk_query_enable(u32 chan, int on) {
    QCounter* c = counter(chan, 1);
    if (!c) return;
    if (!on) end_own(c);
    c->disabled = !on;
}
int vk_query_report(u32 chan, u64 gva, u32 len, u64 ticks) {
    QCounter* c = counter(chan, 0);
    if (!c || !c->started || c->bad || c->whead - c->wtail == QWAIT) return 0;
    end_own(c);
    vk_render_end();
    vk_query_flush(vk_cmd()); /* the segments counted so far, copied in the submit the report waits for */
    c->wait[c->whead++ % QWAIT] = c->reported;
    c->reported = g_q.next;
    vk_release_samples(gva, len, (u32)(c - g_qc), c->first, g_q.next, ticks);
    return 1;
}
/* a report of counter ci being performed, in order: the count of its epoch's segments [first, end), whose copies
   have run */
u64 vk_query_value(u32 ci, u64 first, u64 end) {
    QCounter* c = &g_qc[ci % QCH];
    vmaInvalidateAllocation(g_vk.vma, g_qmem, 0, VK_WHOLE_SIZE);
    if (first != c->sum_first) { /* a new epoch (one with segments starts past the last one's) */
        c->sum_first = c->sum_end = first;
        c->sum = 0;
    }
    for (u64 s = c->sum_end; s < end; s++)
        if (g_qown[s % QN] == ci % QCH + 1) c->sum += g_qres[s % QN] * g_qmul[s % QN];
    if (end > c->sum_end) c->sum_end = end;
    if (c->whead != c->wtail) c->wtail++;
    double area = (double)g_vk_scale * (double)g_vk_scale;
    u64 v = area == 1.0 ? c->sum : (u64)((double)c->sum / area + 0.5);
    return c->sum && !v ? 1 : v;
}
