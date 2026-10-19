/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   SRV wire commands/results referenced against Azahar service/sm/{srv.cpp,sm.h}. */
#include "3ds/runtime/internal.h"
#include "3ds/services/dsp.h"
#include "services/srv.swipc.inc"
#include <stdio.h>
#include <string.h>
static uint32_t snx_ctr_srv_RegisterClient(SnxCtrIpc* c, uint32_t process) {
    if (process != CTR_PROCESS_ID) return SNX_CTR_IPC_INVALID_STATE;
    c->process->objects[c->object].data.session.client_pid = process;
    return 0;
}
static uint32_t snx_ctr_srv_GetServiceHandle(SnxCtrIpc* c, const uint8_t* name, uint32_t length, uint32_t flags,
                                             uint32_t* session) {
    SnxCtrProcess* p = c->process;
    if (p->objects[c->object].data.session.client_pid != CTR_PROCESS_ID) return 0xd8e06406u;
    if (!length || length > 8) return 0xd9006405u;
    if (flags > 1) return 0xd8e093edu;
    for (unsigned i = 0; i < length; ++i) {
        if (!name[i]) return 0xd9006407u;
        if (name[i] < 0x21 || name[i] > 0x7e) return 0xd8e06406u;
    }
    if (length == 7 && !memcmp(name, "fs:USER", 7)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, session);
    if (length == 5 && !memcmp(name, "APT:S", 5)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_APT, session);
    if (length == 8 && !memcmp(name, "ptm:sysm", 8)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_PTM, session);
    if (length == 8 && !memcmp(name, "hid:USER", 8)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_HID, session);
    if (length == 8 && !memcmp(name, "gsp::Gpu", 8)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_GSP, session);
    if (length == 5 && (!memcmp(name, "cfg:u", 5) || !memcmp(name, "cfg:s", 5) || !memcmp(name, "cfg:i", 5)))
        return snx_ctr_session_create(p, SNX_CTR_SERVICE_CFG, session);
    if (length == 8 && !memcmp(name, "dsp::DSP", 8)) {
        uint32_t result = snx_ctr_dsp_open(p);
        return result ? result : snx_ctr_session_create(p, SNX_CTR_SERVICE_DSP, session);
    }
    if (flags & 1) return 0xd0406401u;
    /* The console waits for the service to be registered (Azahar's GetServiceHandle). No other process runs here
       to register it, so the thread waits forever: say which service it was. */
    snx_ctr_thread_block(p, c->object, 0);
    memcpy(snx_ctr_thread(p)->waiting_service, name, length);
    snx_ctr_thread(p)->waiting_service[length] = 0;
    fprintf(stderr, "3ds srv: the runtime has no service %s. The thread that asked for it waits forever.\n",
            snx_ctr_thread(p)->waiting_service);
    c->state = SNX_CTR_IPC_BLOCKED;
    return 0;
}
