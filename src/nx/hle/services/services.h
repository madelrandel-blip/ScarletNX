/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Registering the HLE services, for startup. The services' own declarations are in services_int.h */
#ifndef SNX_SERVICES_H
#define SNX_SERVICES_H
#include "nx/hle/kernel/ipc.h"

/* each registers its family's service names with the service manager */
void svc_core_init(void);
void svc_core_exit(void); /* at exit, what the others hear of this console leaving (the Eden room) */
void svc_fs_init(void);
void svc_hid_init(void);
void svc_vi_init(void);
void svc_pl_init(void);
/* how many nn::settings::Language values set knows, to check the language the settings file names */
int set_language_count(void);
#include "common/utilities/users.h" /* SNX_USERS_MAX, the .ini's user at most */
#endif
