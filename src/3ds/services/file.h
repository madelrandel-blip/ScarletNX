/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_FILE_H
#define SNX_CTR_FILE_H
#include "3ds/ipc/ctr_ipc.h"
void snx_ctr_file_dispatch(SnxCtrIpc* c);
/* close a file object's host file and tree node, once: at FSFILE Close, or when its last reference goes */
void snx_ctr_file_release(struct SnxCtrProcess* p, uint32_t object);
#endif
