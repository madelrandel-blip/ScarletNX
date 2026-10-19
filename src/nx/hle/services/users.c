/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's users (common/utilities/users.c) of the data folder the game is in, the user's for any other folder:
   the game runs as the one its .ini's `user` names, and anything users.ini says that is not a user stops it */
#include "nx/hle/services/services_int.h"
#include "common/utilities/common.h"

void snx_users_init(void) {
    char why[512], *dir = snx_users_dir(g_exedir[0] ? g_exedir : ".");
    if (!dir) fatal("Out of memory finding the users folder");
    if (snx_users_load(dir, g_opt_user, why, sizeof why)) fatal("The console's users: %s", why);
    LOG("acc: %d user%s in %s, playing as %s (user %d)\n", g_user_count, g_user_count == 1 ? "" : "s", dir,
        snx_users_selected()->nickname, snx_users_selected_number());
    free(dir);
}
