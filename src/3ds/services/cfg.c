/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   cfg:u, cfg:s and cfg:i, the system configuration, as far as applications read it: GetConfig (GetConfigInfoBlk2) of
   the sound output mode, which libctru's ndsp asks for when it starts. The block and its default (stereo) follow
   Azahar core/hle/service/cfg/{cfg.cpp,cfg_defaults.cpp} (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator
   Project / Azahar Emulator Project) at a4b335e, which serves all three names as this does. Any other block stops,
   naming it, rather than answering with an invented value. */
#include "3ds/services/cfg.h"
#include "3ds/runtime/internal.h"
#include <stdio.h>

#define CFG_SOUND_OUTPUT_MODE 0x00070001u
#define CFG_ERR_INVALID_SIZE  0xd90103ecu /* InvalidSize, Config, WrongArgument, Permanent */

static void get_config(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 2)) return;
    uint32_t size = c->in[1], block = c->in[2], descriptor = c->in[3], address = c->in[4];
    if ((descriptor & 15) != 12 || (descriptor >> 4) < size) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (block != CFG_SOUND_OUTPUT_MODE) {
        fprintf(stderr, "3ds cfg: config block 0x%08x is not implemented\n", (unsigned)block);
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x42;
    if (size != 1) {
        c->out[1] = CFG_ERR_INVALID_SIZE;
    } else {
        const uint8_t stereo = 1; /* 0 mono, 1 stereo, 2 surround */
        if (snx_ctr_write_ram(c->process->space, address, &stereo, 1) != SNX_CTR_MEM_OK) {
            c->transport_result = 0xe0e01bf5u;
            return;
        }
        c->out[1] = 0;
    }
    c->out[2] = descriptor;
    c->out[3] = address;
}
void snx_ctr_cfg_dispatch(SnxCtrIpc* c) {
    switch (c->in[0] >> 16) {
        case 0x0001: get_config(c); break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
