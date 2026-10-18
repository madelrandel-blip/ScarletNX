/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the service files share: the event helpers, each family's constructors and init, and the includes every
   service file relies on */
#ifndef SNX_SERVICES_INT_H
#define SNX_SERVICES_INT_H
#include "nx/hle/services/services.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* ---- services.c */
u32 ev_handle(KEvent* e); /* a handle to e for a reply, with its own reference */
/* a new event for a reply that only the game holds, signaled or not. Returns 0, for a handler to return */
int new_event(u32* event, const char* name, int signaled);
KEvent* owned_event(const char* name); /* a new event the service keeps a reference on */

/* ---- sm.c */
SvcObj* new_sm(void);

/* ---- am.c */
extern u64 g_user_id[2];
void am_init(void); /* the applet events, and the messages queued at startup */
SvcObj* new_appletoe(void);
SvcObj* new_appletae(void);

/* ---- set.c */
void set_language_code(int lang, u8 code[8]); /* the 8-byte LanguageCode, en-US for an index out of range */
SvcObj* new_set(void);
SvcObj* new_setsys(void);

/* ---- time.c */
void time_init(void);
SvcObj* new_time(void);

/* ---- acc.c */
SvcObj* new_acc(void);

/* ---- ldr.c: ldr:ro and ro:1 */
SvcObj* new_ldr(void);
SvcObj* new_ro1(void);

/* ---- audio.c */
void audio_init(void); /* the renderer's event, before the audio clock runs */
void audio_clock(void* arg);
SvcObj* new_audren(void);
SvcObj* new_audout(void);
SvcObj* new_hwopus(void);

/* ---- misc.c: the small services */
SvcObj* new_apm(void);
SvcObj* new_lm(void);
SvcObj* new_pctl(void);
SvcObj* new_friend(void);
SvcObj* new_ns(void);
SvcObj* new_prepo(void);
SvcObj* new_psm(void);
SvcObj* new_fatal(void);
SvcObj* new_csrng(void);
SvcObj* new_mm(void);
SvcObj* new_nfp(void);
SvcObj* new_nfc(void);
SvcObj* new_caps(void);
SvcObj* new_bcat(void);
SvcObj* new_aoc(void);
SvcObj* new_irs(void);
SvcObj* new_mii(void);
SvcObj* new_ectx(void);
SvcObj* new_spl(void);
SvcObj* new_btmu(void);
SvcObj* new_audin(void);

/* ---- audren.c: the audio renderer */
int audren_update(SvcObj* o, IpcBuf in, IpcBuf out); /* RequestUpdate, on its input and output buffers */
int audren_open(SvcObj* o, const u8* params);        /* 0 when the renderer can't be made */
void audren_tick(void);                              /* one frame of every started renderer */
#endif
