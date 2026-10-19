/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Deterministic queue scheduling: real release/writeback code, fake completed GPU
   submissions. No Vulkan device or worker timing is needed to expose recursion,
   decreasing gates, physical aliases, and remapping order. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/runtime/platform/plat.h"
#define SNX_VK_H
#define VK_NULL_HANDLE 0
void vk_result_begin(void);
void vk_result_end(void);
int vk_result_landing(void);
void vk_lock(void);
void vk_unlock(void);
void vk_compute_land_through(u64);
void vk_land_through(u64);
void vk_wait_submit(u64);
void vk_compute_wait(u64);
void vk_gfx_image_land(void);
u64 vk_compute_put_aside(void);
int vk_buf_pending_any(void);
int vk_buf_pending(u64, u64);
int vk_compute_pending(u64, u64);
int vk_gfx_image_pending(u64, u64);
int vk_buf_pending_unsubmitted(void);
void vk_submit(u64, u64, u64);
u64 vk_last_submit(void);
u64 vk_submit_number(void);
void vk_rt_before_write(u64, u64);
u64 vk_query_value(u32, u64, u64);
#include "nx/gpu/vk/release.c"
#include "nx/gpu/vk/writeback.c"

int g_gpu_releases;
static unsigned lock_depth, landing_calls, payload_writes, submits, query_reads;
static u64 next_compute, next_graphics = 1, waited_compute, waited_graphics;
static int pending, landing_probe, other_pending;
static u32 completed[128];
static u8 memory[8192];
static u64 mapping = 4096;
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 n) {
    (void)c;
    (void)m;
    (void)n;
    abort();
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    (void)fn;
    (void)arg;
    (void)stack;
    return 0;
}
void fatal(const char* fmt, ...) {
    (void)fmt;
    abort();
}
void vk_lock(void) { lock_depth++; }
void vk_unlock(void) {
    assert(lock_depth);
    lock_depth--;
}
u64 gmmu_translate(u64 address) {
    if (address >= 0x10000 && address < 0x11000) return mapping + (address & 4095);
    if (address >= 0x20000 && address < 0x21000) return 4096 + (address & 4095);
    return 0;
}
/* the queued payloads' page index (pending_pages.c) translates runs of contiguous pages */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    u64 n, first = gmmu_translate(gva);
    if (!max || !first) return 0;
    *cpu = first;
    n = 0x1000 - (gva & 0xFFF);
    while (n < max && gmmu_translate(gva + n) == first + n) n += 0x1000;
    return n < max ? n : max;
}
u32 syncpt_read(u32 id) { return id < 128 ? completed[id] : 0; }
void syncpt_incr(u32 id) {
    assert(lock_depth && vk_result_landing());
    completed[id]++;
}
int gmem_write(u64 address, const void* data, u64 size) {
    assert(lock_depth && vk_result_landing());
    vk_release_need(address, size); /* the real memory layer reenters here */
    u64 physical = gmmu_translate(address);
    assert(physical && physical + size <= sizeof memory);
    memcpy(memory + physical, data, size);
    payload_writes++;
    return 1;
}
u8* gmem_ptr(u64 address, u64 size) {
    (void)address;
    (void)size;
    return NULL;
}
int gmem_read(u64 address, void* data, u64 size) {
    vk_release_need(address, size);
    memcpy(data, memory + gmmu_translate(address), size);
    return 1;
}
void gpu_mem_written_shader(u64 address, u64 size) {
    (void)address;
    (void)size;
}
void vk_rt_before_write(u64 address, u64 size) {
    (void)address;
    (void)size;
}
void vk_compute_land_through(u64 gate) {
    assert(lock_depth && vk_result_landing());
    assert(waited_compute >= gate);
    landing_calls++;
    if (landing_probe) {
        landing_probe = 0;
        u8 original = 0, result = 7;
        vk_write_changed(0x10000, &result, &original, 1);
        assert(completed[3] == 0); /* release follows all output reconciliation */
    }
}
void vk_land_through(u64 gate) { assert(waited_graphics >= gate); }
void vk_wait_submit(u64 gate) {
    if (gate > waited_graphics) waited_graphics = gate;
}
void vk_compute_wait(u64 gate) {
    if (gate > waited_compute) waited_compute = gate;
}
void vk_gfx_image_land(void) {}
u64 vk_compute_put_aside(void) {
    u64 value = next_compute;
    next_compute = 0;
    return value;
}
int vk_buf_pending_any(void) { return pending; }
/* another storage result pending over the range read (vk_release_read waits for it instead) */
int vk_compute_pending(u64 a, u64 n) {
    (void)a;
    (void)n;
    return other_pending;
}
int vk_buf_pending(u64 a, u64 n) {
    (void)a;
    (void)n;
    return 0;
}
int vk_gfx_image_pending(u64 a, u64 n) {
    (void)a;
    (void)n;
    return 0;
}
void gmem_read_raw(u64 address, void* data, u64 size) { memcpy(data, memory + gmmu_translate(address), size); }
int vk_buf_pending_unsubmitted(void) { return 0; }
/* a submit: what was recorded becomes the last submission */
void vk_submit(u64 a, u64 b, u64 c) {
    (void)a;
    (void)b;
    (void)c;
    submits++;
    next_graphics++;
}
/* a made-up count of query.c's counter 3's segments, read under the lock when a report is performed */
u64 vk_query_value(u32 counter, u64 first, u64 end) {
    assert(lock_depth && !vk_result_landing() && counter == 3);
    query_reads++;
    return (end - first) * 100 + first;
}
u64 vk_last_submit(void) { return next_graphics; }
u64 vk_submit_number(void) { return next_graphics + 1; }

int main(void) {
    pending = 1;
    next_compute = 9;
    vk_release_incr(3);
    next_compute = 0; /* older compute work has landed through another read */
    u8 value = 11;
    vk_release_write(0x10000, &value, 1);
    assert(g_rq[1].cgate == 9 && vk_release_value(3) == 1);
    landing_probe = 1;
    vk_release_need(0x20000, 1); /* another GVA names the same backing */
    assert(landing_calls == 1 && completed[3] == 1 && memory[4096] == 11);
    assert(g_rhead == g_rtail && !g_gpu_releases && !vk_result_landing());
    assert(vk_release_value(3) == 1);
    value = 13;
    vk_release_write(0x10000, &value, 1);
    vk_release_drain(); /* mapping callback must drain before changing backing */
    mapping = 1;
    assert(memory[4096] == 13 && memory[1] == 0);
    pending = 0;
    value = 17;
    vk_release_write(0x10000, &value, 1); /* immediate writes hold the mapping lock */
    assert(memory[1] == 17 && payload_writes == 4);
    pending = 1;
    vk_release_incr(5);
    vk_release_retire(5);
    assert(completed[5] == 1 && g_rhead == g_rtail);
    /* a samples-passed report waits for the submit being recorded even with nothing pending, without submitting it:
       whoever needs it submits, and its count is read when it is performed (the count, then the ticks) */
    pending = 0;
    unsigned before = submits;
    u64 recording = vk_submit_number();
    vk_release_samples(0x10010, 16, 3, 2, 5, 0x77);
    assert(submits == before && g_rhead == g_rtail + 1 && g_rq[g_rtail % RQ_N].gate == recording && !query_reads);
    vk_release_need(0x10010, 16);
    u64 report[2];
    memcpy(report, memory + 1 + 0x10, 16);
    assert(submits == before + 1 && vk_last_submit() == recording);
    assert(query_reads == 1 && report[0] == 302 && report[1] == 0x77 && g_rhead == g_rtail);
    /* the end of the GPFIFO submission submits it too, and a second stream end has nothing to submit; the count's low
       word alone */
    vk_release_samples(0x10030, 4, 3, 7, 9, 0x99);
    vk_release_submit();
    vk_release_submit();
    assert(submits == before + 2);
    vk_release_drain();
    u32 word;
    memcpy(&word, memory + 1 + 0x30, 4);
    assert(query_reads == 2 && word == 207 && memory[1 + 0x34] == 0);
    /* a read over queued releases takes their payloads, in their order, without waiting for the work before them or
       writing memory; the same memory through another address, another pending result there or a samples-passed count
       not known yet make it decline, and the caller waits as before */
    mapping = 4096; /* 0x10000 and 0x20000 name the same page */
    pending = 1;
    u64 waited = waited_graphics;
    value = 21;
    vk_release_write(0x10040, &value, 1);
    value = 23;
    vk_release_write(0x10041, &value, 1);
    value = 25;
    vk_release_write(0x10040, &value, 1);
    memory[4096 + 0x42] = 9;
    u8 got[3] = {0};
    assert(vk_release_read(0x10040, got, 3) == 1 && got[0] == 25 && got[1] == 23 && got[2] == 9);
    assert(waited_graphics == waited && memory[4096 + 0x40] == 0 && g_rhead == g_rtail + 3 &&
           g_vk_release_forwards == 1);
    assert(vk_release_read(0x20040, got, 1) == -1);
    other_pending = 1;
    assert(vk_release_read(0x10040, got, 1) == -1 && vk_release_read(0x10060, got, 1) == 0);
    other_pending = 0;
    vk_release_samples(0x10048, 4, 3, 1, 2, 0);
    assert(vk_release_read(0x10048, got, 4) == -1 && vk_release_read(0x10050, got, 1) == 0);
    vk_release_drain();
    assert(memory[4096 + 0x40] == 25 && memory[4096 + 0x41] == 23 && g_rhead == g_rtail);
    assert(lock_depth == 0);
    return 0;
}
