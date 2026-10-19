/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/users.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USERS_FILE       "users.ini"
#define USERS_PICTURE    0x20000 /* nn::account's largest profile image */
#define USERS_FIRST_NAME "Player"

SnxUser g_users[SNX_USERS_MAX];
int g_user_count;
static int g_user_selected;            /* g_users' index of the user the game runs as */
static unsigned char* g_user_pictures; /* every user's picture, one after another */

void snx_users_uid_text(const uint64_t uid[2], char text[33]) {
    snprintf(text, 33, "%016llx%016llx", (unsigned long long)uid[0], (unsigned long long)uid[1]);
}
/* 32 hex digits, uid[0] the first 16 as snx_users_uid_text writes them */
static int users_uid_parse(const char* text, uint64_t uid[2]) {
    int i, d;
    uid[0] = uid[1] = 0;
    for (i = 0; i < 32; i++) {
        char c = text[i];
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
            d = (c | 0x20) - 'a' + 10;
        else
            return -1;
        uid[i / 16] = uid[i / 16] << 4 | (uint64_t)d;
    }
    return text[32] ? -1 : 0;
}

const SnxUser* snx_users_find(const uint64_t uid[2]) {
    int i;
    for (i = 0; i < g_user_count; i++)
        if (g_users[i].uid[0] == uid[0] && g_users[i].uid[1] == uid[1]) return &g_users[i];
    return NULL;
}
const SnxUser* snx_users_selected(void) { return &g_users[g_user_selected]; }
int snx_users_selected_number(void) { return g_user_selected + 1; }
const unsigned char* snx_users_picture(const SnxUser* user, uint32_t* size) {
    *size = user->picture_size;
    return user->picture_size ? g_user_pictures + user->picture_at : NULL;
}

int snx_users_nickname_valid(const char* nickname) {
    size_t n = strlen(nickname), i;
    if (n < SNX_USERS_NICKNAME_MIN || n > SNX_USERS_NICKNAME_MAX) return 0;
    for (i = 0; i < n; i++) {
        char c = nickname[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* the [userN] of section, 1 to 8, or 0 */
static int users_section(const char* section, size_t n) {
    if (n != 5 || strncmp(section, "user", 4) || section[4] < '1' || section[4] > '0' + SNX_USERS_MAX) return 0;
    return section[4] - '0';
}
/* why the users.ini text is no list of users, or NULL with their count: each line a comment, a key=value, or a [userN],
   these [user1] to [userN] in that order, each once */
static const char* users_lines(const char* text, int* count) {
    const char* line = text;
    *count = 0;
    while (*line) {
        const char* end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        while (n && (*line == ' ' || *line == '\t')) line++, n--;
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
        if (n && *line == '[') {
            if (n < 2 || line[n - 1] != ']') return "a section line has no ]";
            if (!users_section(line + 1, n - 2)) return "a section is not [user1] to [user8]";
            if (users_section(line + 1, n - 2) != *count + 1)
                return "the users are [user1], [user2]... in order, once each";
            ++*count;
        } else if (n && *line != ';' && *line != '#') {
            if (!memchr(line, '=', n)) return "a line is neither a [userN], a key=value nor a comment";
            if (!*count) return "a key comes before [user1]";
        }
        if (!end) break;
        line = end + 1;
    }
    return *count ? NULL : "no [user1]";
}

/* users/<uid>.jpg of each user, read into one block: a JPEG (FF D8 to FF D9) of at most 128 KiB, or none */
static int users_read_pictures(const char* dir, char* why, size_t why_size) {
    unsigned char* data[SNX_USERS_MAX];
    size_t size[SNX_USERS_MAX], total = 0;
    int i, read = 0, rc = 0;
    for (i = 0; i < g_user_count && !rc; i++) {
        char name[40], *path;
        snx_users_uid_text(g_users[i].uid, name);
        strcat(name, ".jpg");
        path = snx_path_join(dir, name);
        size[i] = 0;
        data[i] = snx_is_file(path) ? snx_read_file(path, &size[i]) : NULL;
        read++;
        if (snx_is_file(path) && !data[i]) {
            snprintf(why, why_size, "%s could not be read", path);
            rc = -1;
        } else if (data[i] && (size[i] < 4 || size[i] > USERS_PICTURE || data[i][0] != 0xFF || data[i][1] != 0xD8 ||
                               data[i][size[i] - 2] != 0xFF || data[i][size[i] - 1] != 0xD9)) {
            snprintf(why, why_size, "%s is not a JPEG of at most %u bytes", path, (unsigned)USERS_PICTURE);
            rc = -1;
        }
        if (!data[i]) size[i] = 0;
        total += size[i];
        free(path);
    }
    if (!rc && total) {
        g_user_pictures = (unsigned char*)snx_alloc(total);
        total = 0;
        for (i = 0; i < g_user_count; i++) {
            g_users[i].picture_at = (uint32_t)total;
            g_users[i].picture_size = (uint32_t)size[i];
            if (size[i]) memcpy(g_user_pictures + total, data[i], size[i]);
            total += size[i];
        }
    }
    for (i = 0; i < read; i++) free(data[i]);
    return rc;
}

char* snx_users_dir(const char* folder) {
    char *data = snx_menu_dir(folder, SNX_USER_DATA), *dir = data ? snx_path_join(data, "users") : NULL;
    free(data);
    return dir;
}
int snx_users_load(const char* dir, int selected, char* why, size_t why_size) {
    char *path = snx_path_join(dir, USERS_FILE), *text, *key, *value;
    const char* bad;
    size_t size;
    SnxIni ini;
    int count = 1, i, j, n, rc = -1;
    free(g_user_pictures);
    g_user_pictures = NULL;
    memset(g_users, 0, sizeof g_users);
    g_user_count = 1;
    text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
    if (snx_is_file(path) && !text) {
        snprintf(why, why_size, "%s could not be read", path);
        goto done;
    }
    if (text && (bad = users_lines(text, &count)) != NULL) {
        snprintf(why, why_size, "%s: %s", path, bad);
        goto done;
    }
    if (text) {
        snx_ini_start(&ini, text);
        while (snx_ini_next(&ini, &key, &value)) {
            SnxUser* u;
            n = users_section(ini.section, strlen(ini.section));
            u = &g_users[n - 1]; /* users_lines saw every key under a [userN] */
            if (!strcmp(key, "uid")) {
                uint64_t uid[2];
                if (users_uid_parse(value, uid) || (!uid[0] && !uid[1])) {
                    snprintf(why, why_size, "%s: [user%d] uid=%s is not 32 hex digits, nor zero", path, n, value);
                    goto done;
                }
                if (n == 1 && (uid[0] != SNX_USERS_FIRST_UID0 || uid[1] != SNX_USERS_FIRST_UID1)) {
                    snprintf(why, why_size, "%s: [user1] is always uid 0123456789abcdeffedcba9876543210, not %s", path,
                             value);
                    goto done;
                }
                u->uid[0] = uid[0];
                u->uid[1] = uid[1];
            } else if (!strcmp(key, "nickname")) {
                if (!snx_users_nickname_valid(value)) {
                    snprintf(why, why_size,
                             "%s: [user%d] nickname=%.40s is not %d to %d letters, digits, spaces, '.', '_' or '-', "
                             "as Eden's rooms take it",
                             path, n, value, SNX_USERS_NICKNAME_MIN, SNX_USERS_NICKNAME_MAX);
                    goto done;
                }
                memcpy(u->nickname, value, strlen(value) + 1);
            } else {
                snprintf(why, why_size, "%s: [user%d] %s is not a setting: uid, nickname", path, n, key);
                goto done;
            }
        }
    }
    /* user 1 is the console's first user whatever users.ini says of it, and "Player" unless it names it */
    g_users[0].uid[0] = SNX_USERS_FIRST_UID0;
    g_users[0].uid[1] = SNX_USERS_FIRST_UID1;
    if (!g_users[0].nickname[0]) strcpy(g_users[0].nickname, USERS_FIRST_NAME);
    for (i = 1; i < count; i++) {
        if (!g_users[i].uid[0] && !g_users[i].uid[1]) {
            snprintf(why, why_size, "%s: [user%d] has no uid", path, i + 1);
            goto done;
        }
        if (!g_users[i].nickname[0]) {
            snprintf(why, why_size, "%s: [user%d] has no nickname", path, i + 1);
            goto done;
        }
        for (j = 0; j < i; j++)
            if (g_users[j].uid[0] == g_users[i].uid[0] && g_users[j].uid[1] == g_users[i].uid[1]) {
                snprintf(why, why_size, "%s: [user%d] has the uid of [user%d]", path, i + 1, j + 1);
                goto done;
            }
    }
    g_user_count = count;
    if (selected < 1 || selected > g_user_count) {
        snprintf(why, why_size, "user=%d, and %s has %d user%s", selected, text ? path : "no users.ini", g_user_count,
                 g_user_count == 1 ? "" : "s");
        goto done;
    }
    g_user_selected = selected - 1;
    rc = users_read_pictures(dir, why, why_size);
done:
    if (rc) { /* nothing half read stays: the caller stops the game */
        memset(g_users, 0, sizeof g_users);
        g_users[0].uid[0] = SNX_USERS_FIRST_UID0;
        g_users[0].uid[1] = SNX_USERS_FIRST_UID1;
        strcpy(g_users[0].nickname, USERS_FIRST_NAME);
        g_user_count = 1;
        g_user_selected = 0;
    }
    free(text);
    free(path);
    return rc;
}
