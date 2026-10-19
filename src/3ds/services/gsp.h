/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_GSP_H
#define SNX_CTR_GSP_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdbool.h>
#include <stdint.h>
void snx_ctr_gsp_dispatch(SnxCtrIpc* c);
/* One host-paced vblank: shows latched framebuffers and queues PDC interrupts.
   Not cycle accurate. */
void snx_ctr_gsp_vblank(struct SnxCtrProcess* p);
/* a gsp::Gpu session's last reference went: it gives up the GPU right, its relay thread slot and its event (Azahar
   GSP_GPU::ClientDisconnected and SessionData's destructor) */
void snx_ctr_gsp_session_closed(struct SnxCtrProcess* p, uint32_t session);
#endif
