/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest pages pending GPU results cover, counted per page, so a read or write of guest memory finds out with one
   pass over its pages whether any result is pending there, instead of checking every result. Both Eden (MemoryTracker,
   one bit per page, de7e6e01) and Ryujinx (region tracking, a2c00350) key what the GPU wrote by guest address and
   translate a GPU range once, when it is made: GPU addresses that alias the same guest pages meet without more work */
#ifndef SNX_VK_PENDING_PAGES_H
#define SNX_VK_PENDING_PAGES_H
#include "common/cpu/aarch64/cpu.h"

/* Every guest page under 2^40 in a two-level table: leaves of 4096 pages made when a result first covers one, each
   with a count per page, the pages of each group of 64 that have one, and its pages in all, so a lookup skips what no
   result covers. Zeroed is empty. Not locked: the caller serializes (vk_lock) */
typedef struct PendLeaf PendLeaf;
typedef struct PendPages PendPages;
struct PendPages {
    PendLeaf* leaf[1u << 16];
    u64 total;    /* every page's count added up */
    u64 adds;     /* results added so far: unchanged, no result covers what was checked without a hit since */
    u64 overflow; /* pages past 2^40 results cover, read as covering every page */
    /* a table every page this one gains or loses is counted in as well, NULL for none: one table then answers whether
       anything at all is pending on a page (g_pend_any) */
    PendPages* also;
};
/* the guest pages pending compute results, graphics results and queued releases cover, all together: a read that meets
   none of them needs none of their checks (vk_mem_pending) */
extern PendPages g_pend_any;
/* the guest pages behind [gva, gva + size) (a GPU range, translated through the GMMU now) gain a result (delta 1) or
   lose one (-1). A result loses its pages with the mappings it gained them with: a mapping change lands every result
   over it first */
void pend_add(PendPages* t, u64 gva, u64 size, int delta);
/* nonzero when a result covers a guest page behind [gva, gva + size) */
int pend_hit(const PendPages* t, u64 gva, u64 size);
/* one guest page (an address >> 12) gains or loses a result: for results that keep their own translated pages */
void pend_add_page(PendPages* t, u64 page, int delta);
/* the same for pages counted by their GPU address, with no translation: for what is compared by GPU address */
void pend_add_va(PendPages* t, u64 gva, u64 size, int delta);
int pend_hit_va(const PendPages* t, u64 gva, u64 size);
/* nonzero when no result covers any page: once every result is removed, each with the mappings it was added with */
int pend_empty(const PendPages* t);

#endif
