/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Serialized, bounded single-core thread contexts; no host threads or JIT.
   SVC register layout from Azahar kernel/svc_wrapper.h (revision in 3DS.md). */
#include "3ds/runtime/internal.h"
#include "3ds/services/gsp.h"
#include "3ds/services/dsp.h"
#include "common/utilities/common.h"
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif
#define THREAD_LIMIT 32u /* the APPLICATION resource limit's threads, each a 512-byte TLS slot */
CtrThread* snx_ctr_thread(SnxCtrProcess* p) { return &p->objects[p->current_thread].data.thread; }
void snx_ctr_object_release(SnxCtrProcess* p, uint32_t id) {
    if (!--p->objects[id].refs) p->objects[id] = (CtrObject){0};
}
void snx_ctr_thread_block(SnxCtrProcess* p, uint32_t id, uint32_t address) {
    CtrThread* t = snx_ctr_thread(p);
    t->waiting_object = id;
    t->waiting_address = address;
    t->wait_more_count = t->wait_multiple = t->wait_all = 0;
    ++p->objects[id].refs;
}
static uint32_t tls(SnxCtrProcess* p, uint32_t* address) {
    unsigned char zero[4096] = {0};
    for (uint32_t slot = 0; slot < THREAD_LIMIT; ++slot) {
        uint32_t va = SNX_CTR_INITIAL_TLS + slot * 512;
        bool used = false;
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
            if (p->objects[i].type == CTR_OBJ_THREAD && !p->objects[i].data.thread.dead &&
                p->objects[i].data.thread.context.tpidruro == va)
                used = true;
        if (used) continue;
        uint32_t page = va & ~4095u;
        if (snx_ctr_query(p->space, page, NULL, NULL) != SNX_CTR_MEM_OK) {
            uint32_t physical;
            for (physical = 0; physical < p->app_limit / 4096; ++physical)
                if (!p->physical_owner[physical]) break;
            if (physical == p->app_limit / 4096) return 0xd86007f3u;
            if (snx_ctr_map(p->space, page, CTR_PHYS_BASE + physical * 4096, 4096, 3) != SNX_CTR_MEM_OK)
                return 0xd86007f3u;
            if (snx_ctr_write_ram(p->space, page, zero, sizeof zero) != SNX_CTR_MEM_OK) {
                (void)snx_ctr_unmap(p->space, page, 4096);
                return 0xe0e01bf5u;
            }
            p->physical_owner[physical] = CTR_STATIC;
            *address = va;
            return 0;
        }
        if (snx_ctr_write_ram(p->space, va, zero, 512) != SNX_CTR_MEM_OK) return 0xe0e01bf5u;
        *address = va;
        return 0;
    }
    return 0xc860180cu; /* bounded implementation thread/TLS quota */
}
uint32_t snx_ctr_thread_create(SnxCtrProcess* p, const uint32_t* r, uint32_t* out) {
    /* Azahar svc.cpp CreateThread: priority r0 (lowest 63, and no better than the APPLICATION limit's 0x18), entry
       r1, argument r2, stack top r3, processor r4: -2 is the process's ideal processor and -1 any, both core 0; 0 to 3
       are taken as asked (Azahar checks no exheader flag for cores 2 and 3 either). Every core's threads share the
       one serialized scheduler: a syscore thread runs whenever its priority allows, without core 1's CPU-time limit. */
    *out = 0;
    if (r[0] > 63) return 0xe0e01bfdu;                 /* ResultOutOfRange */
    if (r[0] < CTR_PRIORITY_LIMIT) return 0xd9001beau; /* ResultNotAuthorized */
    int32_t processor = (int32_t)r[4];
    if (processor < -2 || processor > 3) return 0xe0e01bfdu;
    unsigned char instruction[4];
    uint32_t entry = r[1] & ~1u, width = r[1] & 1 ? 2 : 4;
    if ((entry & (width - 1)) || snx_ctr_fetch(p->space, entry, instruction, width) != SNX_CTR_MEM_OK)
        return 0xe0e01bf5u;
    if ((r[3] & 7) || r[3] < 4 || snx_ctr_read_ram(p->space, r[3] - 4, instruction, 4) != SNX_CTR_MEM_OK)
        return 0xe0e01bf5u;
    unsigned permissions;
    if (snx_ctr_query(p->space, r[3] - 4, NULL, &permissions) != SNX_CTR_MEM_OK || !(permissions & SNX_CTR_WRITE))
        return 0xe0e01bf5u;
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_THREAD, out);
    if (result) return result;
    uint32_t id = snx_ctr_handle_object(p, *out), address;
    result = tls(p, &address);
    if (result) {
        snx_ctr_handle_close(p, *out);
        *out = 0;
        return result;
    }
    CtrThread* t = &p->objects[id].data.thread;
    t->context.r[0] = r[2];
    t->context.r[13] = r[3];
    t->context.r[15] = entry;
    t->context.cpsr = 0x10 | (r[1] & 1 ? SNX_ARM_T : 0);
    t->context.tpidruro = address;
    t->priority = r[0];
    t->core = processor < 0 ? 0u : (uint32_t)processor;
    ++p->objects[id].refs; /* live thread owns a root, even if its handle closes */
    return 0;
}
static bool ready(SnxCtrProcess* p, uint32_t id, uint32_t thread) {
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_EVENT) return !!o->data.event.signaled;
    if (o->type == CTR_OBJ_TIMER) return !!o->data.timer.signaled;
    if (o->type == CTR_OBJ_THREAD) return !!o->data.thread.dead;
    if (o->type == CTR_OBJ_SEMAPHORE) return !!o->data.semaphore.count;
    return o->type == CTR_OBJ_MUTEX && (!o->data.mutex.depth || o->data.mutex.owner == thread);
}
static void acquire(SnxCtrProcess* p, uint32_t id, uint32_t thread) {
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_EVENT && o->data.event.reset_type == CTR_RESET_ONESHOT) o->data.event.signaled = 0;
    if (o->type == CTR_OBJ_TIMER && o->data.timer.reset_type == CTR_RESET_ONESHOT) o->data.timer.signaled = 0;
    if (o->type == CTR_OBJ_SEMAPHORE && o->data.semaphore.count) --o->data.semaphore.count;
    if (o->type == CTR_OBJ_MUTEX) {
        o->data.mutex.owner = thread;
        ++o->data.mutex.depth;
    }
}
SnxCtrSvcResult snx_ctr_wait_many(SnxCtrProcess* p, uint32_t* r) {
    if (!r[2] || r[2] > 64 || r[3] > 1) {
        r[0] = 0xd8e007fdu;
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    unsigned char bytes[256];
    uint32_t ids[64], count = r[2], found = UINT32_MAX;
    if (snx_ctr_read_ram(p->space, r[1], bytes, count * 4) != SNX_CTR_MEM_OK) {
        r[0] = 0xe0e01bf5u;
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    bool all = true;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char* b = bytes + i * 4;
        uint32_t handle = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        ids[i] = snx_ctr_handle_object(p, handle);
        if (!ids[i]) {
            r[0] = 0xd8e007f7u;
            r[1] = UINT32_MAX;
            return SNX_CTR_SVC_RETURN;
        }
        uint32_t type = p->objects[ids[i]].type;
        if (type != CTR_OBJ_EVENT && type != CTR_OBJ_MUTEX && type != CTR_OBJ_THREAD && type != CTR_OBJ_SEMAPHORE &&
            type != CTR_OBJ_TIMER)
            return SNX_CTR_SVC_UNSUPPORTED;
        for (uint32_t j = 0; j < i; ++j)
            if (ids[i] == ids[j]) return SNX_CTR_SVC_UNSUPPORTED;
        if (type == CTR_OBJ_MUTEX && p->objects[ids[i]].data.mutex.depth == UINT32_MAX) return SNX_CTR_SVC_UNSUPPORTED;
        bool available = ready(p, ids[i], p->current_thread);
        if (available && found == UINT32_MAX) found = i;
        all = all && available;
    }
    if (r[3] ? all : found != UINT32_MAX) {
        for (uint32_t i = 0; i < count; ++i)
            if (r[3] || i == found) acquire(p, ids[i], p->current_thread);
        r[1] = r[3] ? UINT32_MAX : found;
        r[0] = 0;
        return SNX_CTR_SVC_RETURN;
    }
    if (!r[0] && !r[4]) {
        r[0] = CTR_TIMED_OUT;
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    CtrThread* t = snx_ctr_thread(p);
    for (uint32_t i = 0; i < count; ++i) ++p->objects[ids[i]].refs;
    t->waiting_object = ids[0];
    t->wait_multiple = 1;
    t->wait_all = !!r[3];
    t->wait_more_count = count - 1;
    for (uint32_t i = 1; i < count; ++i) t->wait_more[i - 1] = ids[i];
    snx_ctr_thread_deadline(p, r[0], r[4]); /* nanoseconds: r0 low, r4 high */
    return SNX_CTR_SVC_BLOCKED;
}
void snx_ctr_wake_waiters(SnxCtrProcess* p, uint32_t object) {
    /* Highest-priority eligible waiter first. One-shot events and mutexes are
       acquired on completion; later waiters recheck the resulting state. */
    for (;;) {
        uint32_t best = 0, index = 0;
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
            if (p->objects[i].type != CTR_OBJ_THREAD) continue;
            CtrThread* t = &p->objects[i].data.thread;
            if (!t->waiting_object || t->dead) continue;
            bool includes = t->waiting_object == object, all = true;
            uint32_t first = UINT32_MAX;
            for (uint32_t j = 0; j <= t->wait_more_count; ++j) {
                uint32_t id = j ? t->wait_more[j - 1] : t->waiting_object;
                includes = includes || id == object;
                bool available = ready(p, id, i);
                if (available && first == UINT32_MAX) first = j;
                all = all && available;
            }
            if (!includes || (t->wait_all ? !all : first == UINT32_MAX)) continue;
            if (!best || t->priority < p->objects[best].data.thread.priority) {
                best = i;
                index = first;
            }
        }
        if (!best) return;
        CtrThread* t = &p->objects[best].data.thread;
        for (uint32_t j = 0; j <= t->wait_more_count; ++j)
            if (t->wait_all || j == index) acquire(p, j ? t->wait_more[j - 1] : t->waiting_object, best);
        t->context.r[0] = 0;
        if (t->wait_multiple) t->context.r[1] = t->wait_all ? UINT32_MAX : index;
        t->context.exit = t->context.syndrome = 0;
        for (uint32_t j = 0; j <= t->wait_more_count; ++j)
            snx_ctr_object_release(p, j ? t->wait_more[j - 1] : t->waiting_object);
        t->waiting_object = t->waiting_address = t->wait_more_count = t->wait_multiple = t->wait_all = 0;
        t->wake_ns = 0;
    }
}
static uint64_t now_ns(void) { return (uint64_t)(snx_seconds() * 1e9); }
void snx_ctr_thread_deadline(SnxCtrProcess* p, uint32_t low, uint32_t high) {
    /* As Azahar's Thread::WakeAfterDelay: -1 schedules no wakeup; other negative delays are already due */
    int64_t ns = (int64_t)((uint64_t)high << 32 | low);
    uint64_t now = now_ns();
    CtrThread* t = snx_ctr_thread(p);
    if (ns == -1)
        t->wake_ns = 0;
    else if (ns <= 0)
        t->wake_ns = now ? now : 1;
    else
        t->wake_ns = (uint64_t)ns > UINT64_MAX - 1 - now ? UINT64_MAX - 1 : now + (uint64_t)ns;
}
/* A waiting thread's deadline passed: its wait ends with Timeout, -1 as WaitSynchronizationN's index (Azahar's
   SVC_SyncCallback), and the objects it waited on lose the references the wait held */
static void time_out(SnxCtrProcess* p, uint32_t id) {
    CtrThread* t = &p->objects[id].data.thread;
    t->context.r[0] = CTR_TIMED_OUT;
    if (t->wait_multiple) t->context.r[1] = UINT32_MAX;
    t->context.exit = t->context.syndrome = 0;
    for (uint32_t j = 0; j <= t->wait_more_count; ++j)
        snx_ctr_object_release(p, j ? t->wait_more[j - 1] : t->waiting_object);
    t->waiting_object = t->waiting_address = t->wait_more_count = t->wait_multiple = t->wait_all = 0;
    t->wake_ns = 0;
}
void snx_ctr_timer_signal(SnxCtrProcess* p, uint32_t id, uint64_t now) {
    /* Azahar kernel/timer.cpp, Timer::Signal: signaled, every waiter woken, a pulse cleared again at once; an interval
       keeps the period from the deadline, not from when it was seen. A deadline more than a period behind (the host
       was busy) restarts from now instead of firing the missed periods back to back. */
    CtrObject* o = &p->objects[id];
    o->data.timer.signaled = 1;
    snx_ctr_wake_waiters(p, id);
    if (o->data.timer.reset_type == CTR_RESET_PULSE) o->data.timer.signaled = 0;
    if (!o->data.timer.interval) {
        o->data.timer.deadline = 0;
        return;
    }
    uint64_t next = o->data.timer.deadline + o->data.timer.interval;
    o->data.timer.deadline = next > now ? next : now + o->data.timer.interval;
}
SnxCtrSvcResult snx_ctr_thread_exit(SnxCtrProcess* p) {
    /* Azahar Thread::Stop: the thread is dead, which satisfies its waiters, and the mutexes it holds are released;
       its TLS slot is free again (tls() skips dead threads). The live thread's root reference goes. */
    uint32_t id = p->current_thread;
    CtrThread* t = snx_ctr_thread(p);
    t->dead = 1;
    t->wake_ns = 0;
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
        CtrObject* o = &p->objects[i];
        if (o->type != CTR_OBJ_MUTEX || o->data.mutex.owner != id || !o->data.mutex.depth) continue;
        o->data.mutex.owner = o->data.mutex.depth = 0;
        snx_ctr_wake_waiters(p, i);
    }
    snx_ctr_wake_waiters(p, id);
    p->cpu.state.cpu.exclusive_valid = 0;
    snx_ctr_object_release(p, id);
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
        if (p->objects[i].type == CTR_OBJ_THREAD && !p->objects[i].data.thread.dead) return SNX_CTR_SVC_BLOCKED;
    return SNX_CTR_SVC_EXIT; /* the process's last thread */
}
static void sleep_until(uint64_t deadline) {
    uint64_t now = now_ns();
    if (deadline <= now) return;
#if defined(_WIN32)
    Sleep((DWORD)((deadline - now) / 1000000 ? (deadline - now) / 1000000 : 1));
#else
    struct timespec ts = {(time_t)((deadline - now) / 1000000000), (long)((deadline - now) % 1000000000)};
    nanosleep(&ts, NULL);
#endif
}
/* The highest-priority runnable thread, the current one among equals (Azahar's PopNextReadyThread: only a strictly
   better priority takes the core). Time is part of it: a sleeping thread is runnable again at its deadline, a
   waiting one times out at its own, timers fire, and the display's vblank and the DSP's audio frame are delivered
   when due, which wakes the threads waiting on GSP's and the DSP's interrupt events. A thread that yielded runs only
   when no other can. When no thread can run but a deadline is ahead, the host sleeps until the first of them; only
   a process with nothing left to wake for is blocked. The dispatcher also calls this every SNX_CTR_POLL_BLOCKS
   blocks, so a woken thread of better priority preempts one that makes no SVC, as an interrupt would. */
bool snx_ctr_schedule(SnxCtrProcess* p) {
    /* The current thread's registers go to its context first: a vblank already due wakes the threads waiting on
       GSP's events, the current one included, by editing their saved contexts, and the thread chosen below always
       resumes from its context. Only the plain CPU prefix is saved. The checked adapter/callback context never
       moves. A context switch or an idle wait loses exclusives, the current thread carrying on keeps them. A thread
       that exited has no context to keep: its object may already be free. */
    uint32_t current = p->current_thread, exclusive = p->cpu.state.cpu.exclusive_valid;
    p->cpu.state.cpu.exclusive_valid = 0;
    if (p->objects[current].type == CTR_OBJ_THREAD && !p->objects[current].data.thread.dead)
        snx_ctr_thread(p)->context = p->cpu.state.cpu;
    for (;;) {
        uint64_t now = now_ns(), earliest = UINT64_MAX;
        if (p->gsp.shared_object && now >= p->gsp.next_vblank_ns) snx_ctr_gsp_vblank(p);
        if (p->dsp.core && now >= p->dsp.next_frame_ns) snx_ctr_dsp_frame(p, now);
        if (now >= p->time_update_ns) snx_ctr_shared_page_time(p);
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
            CtrObject* o = &p->objects[i];
            if (o->type != CTR_OBJ_TIMER || !o->data.timer.deadline) continue;
            if (o->data.timer.deadline <= now) snx_ctr_timer_signal(p, i, now);
            if (o->data.timer.deadline && o->data.timer.deadline < earliest) earliest = o->data.timer.deadline;
        }
        uint32_t next = 0, yielded = 0;
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
            if (p->objects[i].type != CTR_OBJ_THREAD) continue;
            CtrThread* t = &p->objects[i].data.thread;
            if (t->dead) continue;
            if (t->wake_ns) {
                if (t->wake_ns > now) {
                    if (t->wake_ns < earliest) earliest = t->wake_ns;
                    continue;
                }
                if (t->waiting_object) time_out(p, i);
                t->wake_ns = 0;
            }
            if (t->waiting_object) continue;
            if (t->yield) {
                t->yield = 0;
                if (!yielded || t->priority < p->objects[yielded].data.thread.priority) yielded = i;
                continue;
            }
            if (!next || t->priority < p->objects[next].data.thread.priority ||
                (t->priority == p->objects[next].data.thread.priority && i == p->current_thread))
                next = i;
        }
        if (!next) next = yielded;
        if (next) {
            p->current_thread = next;
            p->cpu.state.cpu = snx_ctr_thread(p)->context;
            if (next == current) p->cpu.state.cpu.exclusive_valid = exclusive;
            return true;
        }
        uint64_t deadline = earliest;
        if (p->gsp.shared_object && p->gsp.next_vblank_ns < deadline) deadline = p->gsp.next_vblank_ns;
        if (p->dsp.core && p->dsp.next_frame_ns < deadline) deadline = p->dsp.next_frame_ns;
        if (deadline >= UINT64_MAX - 1) return false; /* nothing to wake for (a thread sleeping forever included) */
        exclusive = 0;
        sleep_until(deadline);
    }
}
