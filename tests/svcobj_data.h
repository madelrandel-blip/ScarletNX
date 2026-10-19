/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* svcobj_new_data as ipc.c's, for a test that stubs svcobj_new: the object is the test's own, its data a slot of
   the pool the service names (in g_hle), taken zeroed but for its first u32, which gets the object's number (non-zero,
   as ipc.c writes the object's pool index + 1). test_svcobj_data_free gives the slot back as svcobj_unref does, or
   frees data a test allocated itself (datasize 0). Included once, by the test's .c file, after its svcobj_new */
#ifndef SNX_TEST_SVCOBJ_DATA_H
#define SNX_TEST_SVCOBJ_DATA_H
#include <stdlib.h>
#include <string.h>
#include "nx/hle/hle.h"

static u32 test_svcobj_number;

SvcObj* svcobj_new_data(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), void* base, size_t size, u32 count,
                        u32* next, const char* what) {
    SvcObj* o = svcobj_new(name, dispatch);
    if (o && base) {
        u32 id = ++test_svcobj_number;
        o->data = pool_take(base, size, count, next, what);
        o->datasize = (u32)size;
        memcpy(o->data, &id, sizeof id);
    }
    return o;
}

static void test_svcobj_data_free(SvcObj* o) {
    if (o->datasize)
        pool_put(o->data, o->datasize);
    else
        free(o->data);
    o->data = NULL;
    o->datasize = 0;
}
#endif
