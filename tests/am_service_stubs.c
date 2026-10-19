/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the game's data folder for am.c's tests: a control.nacp only when a test gives one; and the first user alone, as
   users.c has it without users.ini */
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/services/services_int.h"
#include <stdlib.h>
#include <string.h>

extern const u8* g_test_nacp;
extern u64 g_test_nacp_size;
const u8* g_test_nacp;
u64 g_test_nacp_size;

/* apm's side of the state getter's performance commands (misc.c): what SetCpuBoostMode last handed it */
extern u32 g_test_apm_boost;
u32 g_test_apm_boost = 0xEEEEu;
u32 apm_configuration(u32 mode) { return mode ? 0x92220009u : 0x00020003u; }
void apm_set_configuration(u32 mode, u32 config) {
    (void)mode;
    (void)config;
}
void apm_cpu_boost(u32 boost) { g_test_apm_boost = boost; }

u8* fs_read_file(const char* path, u64* size) {
    u8* d;
    if (size) *size = 0;
    if (strcmp(path, "control.nacp") || !g_test_nacp) return NULL;
    d = malloc((size_t)g_test_nacp_size);
    if (!d) return NULL;
    memcpy(d, g_test_nacp, (size_t)g_test_nacp_size);
    if (size) *size = g_test_nacp_size;
    return d;
}

const SnxUser* snx_users_selected(void) {
    static SnxUser first;
    memcpy(first.uid, g_user_id, 16);
    return &first;
}
