/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RtAPI: the table of runtime functions generated code calls (CPU.rt). Game modules index it by position, so its order
   and size are part of SNX_ABI (cpu.h): a field added, removed or moved needs a new SNX_ABI */
#ifndef SNX_RUNTIME_INTERFACE_H
#define SNX_RUNTIME_INTERFACE_H
#include <stddef.h>
#include <stdint.h>
struct CPU;
typedef struct RtAPI {
    void (*svc)(struct CPU* c, uint32_t imm);              /* SVC #imm */
    void (*undef)(struct CPU* c, uint64_t pc, uint32_t w); /* an instruction word the recompiler has no code for */
    void (*note_hint)(uint64_t pc); /* a function entry the recompiler missed, for the next recompile */
    uint64_t (*ticks)(void);        /* CNTPCT_EL0 and CNTVCT_EL0 */
    void (*brk)(struct CPU* c, uint64_t pc, uint32_t imm); /* BRK #imm */
    void (*fpcr_changed)(struct CPU* c);                   /* a write to FPCR */
    void (*sev)(void);                                     /* SEV, SEVL */
    void (*wfe)(struct CPU* c);                            /* WFE */
    /* CPU.poll was set: a loop that makes no SVC lets the runtime take the thread off its core here (SNX_ABI 7) */
    void (*poll)(struct CPU* c);
    /* SVC #imm for imm below SNX_SVC_COUNT, by number, each with svc's arguments: the NX game modules call the
       SVC's own entry, which their code names by its constant imm (SNX_ABI 9). NULL for a runtime whose modules call
       svc for every SVC (SnxCodegen.direct_svc off) */
    void (*const* svcs)(struct CPU* c, uint32_t imm);
    /* snx_lookup's answer for a pc without code in CPU.disp, called as the guest function at pc (a GuestFn): the
       runtime runs it in the interpreter or compiles it first. The module no longer carries its own fallback, so the
       runtime decides what a miss does */
    void (*miss)(struct CPU* c, uint64_t pc);
} RtAPI;
#define SNX_SVC_COUNT 0x100

/* the layout modules were built with: eleven pointers in this order, nothing between them */
#define SNX_RTAPI_SLOT sizeof(void (*)(void))
typedef char snx_check_rtapi_size[sizeof(RtAPI) == 11 * SNX_RTAPI_SLOT ? 1 : -1];
typedef char snx_check_rtapi_head[offsetof(RtAPI, svc) == 0 && offsetof(RtAPI, undef) == SNX_RTAPI_SLOT &&
                                          offsetof(RtAPI, note_hint) == 2 * SNX_RTAPI_SLOT &&
                                          offsetof(RtAPI, ticks) == 3 * SNX_RTAPI_SLOT
                                      ? 1
                                      : -1];
typedef char snx_check_rtapi_tail[offsetof(RtAPI, brk) == 4 * SNX_RTAPI_SLOT &&
                                          offsetof(RtAPI, fpcr_changed) == 5 * SNX_RTAPI_SLOT &&
                                          offsetof(RtAPI, sev) == 6 * SNX_RTAPI_SLOT &&
                                          offsetof(RtAPI, wfe) == 7 * SNX_RTAPI_SLOT &&
                                          offsetof(RtAPI, poll) == 8 * SNX_RTAPI_SLOT &&
                                          offsetof(RtAPI, svcs) == 9 * SNX_RTAPI_SLOT &&
                                          offsetof(RtAPI, miss) == 10 * SNX_RTAPI_SLOT
                                      ? 1
                                      : -1];
#undef SNX_RTAPI_SLOT

#endif
