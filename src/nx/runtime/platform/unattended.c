/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Unattended runs, for the windowed and the headless backend alike: the run's limit, screenshots and scripted
   presses */
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* one press of the script: at a time in ms, or at a present ("Nf") */
typedef struct {
    u64 mask;
    u64 held_since; /* plat_ns() when a present press began, 0 before */
    u32 at;
    u8 at_present;
    u8 done;
} ScriptPress;

/* The script: its presses and, after them in the same allocation, its text (to tell a reload of the same script
   from a new one). press is the allocation, NULL without a script */
typedef struct {
    ScriptPress* press;
    char* text;
    size_t n;
} Script;

static int g_seconds;      /* 0: no time limit */
static int g_frame_limit = -1;
static int g_shot_every;
static int g_press_len = 300;
static char* g_shot;       /* owned: the argv string or the .ini line it came from goes away */
static Script g_script;
static u64 g_t0;           /* unattended_start's plat_ns() */
static int g_shot_bucket;  /* frames / shot_every at the last periodic shot */
static int g_shot_count;

static int script_parse_press(const char** s, ScriptPress* p) {
    const char* c = *s;
    char* end;
    unsigned long at;
    unsigned long long mask;
    if (*c < '0' || *c > '9') return 0;
    at = strtoul(c, &end, 10);
    c = end;
    memset(p, 0, sizeof *p);
    if (*c == 'f') {
        p->at_present = 1;
        c++;
    }
    if (*c++ != ':' || at > UINT_MAX) return 0;
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'))) return 0;
    mask = strtoull(c, &end, 16);
    c = end;
    if (*c && *c != ',') return 0;
    p->at = (u32)at;
    p->mask = (u64)mask;
    *s = *c ? c + 1 : c;
    return 1;
}

static void script_set(const char* v) {
    size_t len = strlen(v), max = 1, n;
    const char* c;
    ScriptPress* press;
    if (g_script.press && !strcmp(g_script.text, v)) return; /* a reload: keep the progress, presses aren't replayed */
    if (!*v) {
        free(g_script.press);
        memset(&g_script, 0, sizeof g_script);
        return;
    }
    for (c = v; *c; c++) max += *c == ',';
    if (max > ((size_t)-1 - len - 1) / sizeof *press) {
        WARN("unattended: the script is too long, the current one stays\n");
        return;
    }
    press = malloc(max * sizeof *press + len + 1);
    if (!press) {
        WARN("unattended: out of memory for the script, the current one stays\n");
        return;
    }
    for (n = 0, c = v; *c && n < max; n++)
        if (!script_parse_press(&c, &press[n])) {
            WARN("unattended: the script stops at \"%s\", which is not ms:hex or Nf:hex\n", c);
            break;
        }
    free(g_script.press);
    g_script.press = press;
    g_script.text = (char*)(press + max);
    memcpy(g_script.text, v, len + 1);
    g_script.n = n;
}

static void text_set(const IniOpt* o, const char* v) {
    if (!strcmp(o->key, "script"))
        script_set(v);
    else { /* shot */
        char* copy = NULL;
        size_t n = strlen(v);
        if (n) {
            copy = malloc(n + 1);
            if (!copy) {
                WARN("unattended: out of memory for the shot path\n");
                return;
            }
            memcpy(copy, v, n + 1);
        }
        free(g_shot);
        g_shot = copy;
    }
}

/* read with ini_read like the .ini's settings, but no .ini section lists this table, so nothing here is written back */
static const IniOpt g_opts[] = {
    {"seconds", NULL, NULL, INI_INT, 0, &g_seconds, 0, INT_MAX, NULL, NULL, NULL, 0, 0},
    {"frames", NULL, NULL, INI_INT, 0, &g_frame_limit, -1, INT_MAX, NULL, NULL, NULL, 0, 0},
    {"shot", NULL, NULL, INI_TEXT, 0, NULL, 0, 0, text_set, NULL, NULL, 0, 0},
    {"shot_every", NULL, NULL, INI_INT, 0, &g_shot_every, 0, INT_MAX, NULL, NULL, NULL, 0, 0},
    {"press_len", NULL, NULL, INI_INT, 0, &g_press_len, 0, INT_MAX, NULL, NULL, NULL, 0, 0},
    {"script", NULL, NULL, INI_TEXT, 0, NULL, 0, 0, text_set, NULL, NULL, 0, 0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}};

int unattended_known(const char* k) { return ini_find(g_opts, k) != NULL; }
int unattended_ini(const char* k, const char* v) {
    const IniOpt* o = ini_find(g_opts, k);
    if (!o) return 0;
    ini_read(o, v);
    return 1;
}

void unattended_start(void) { g_t0 = plat_ns(); }
u64 unattended_ms(void) { return (plat_ns() - g_t0) / 1000000ull; }
int unattended_active(void) { return g_seconds > 0 || g_frame_limit >= 0; }

static int frames_now(void) { return __atomic_load_n(&g_frames, __ATOMIC_RELAXED); }

u64 unattended_buttons(void) {
    u64 now = plat_ns(), held = 0, len_ns = (u64)g_press_len * 1000000u, ms;
    int frames = frames_now();
    size_t i;
    if (!g_t0) g_t0 = now;
    ms = (now - g_t0) / 1000000u;
    for (i = 0; i < g_script.n; i++) {
        ScriptPress* p = &g_script.press[i];
        if (!p->at_present) {
            if (ms >= p->at && ms - p->at < (u64)g_press_len) held |= p->mask;
            continue;
        }
        if (p->done) continue;
        /* from the first look at or after present N, even when the presents went past it between two looks: the
           game advances a step per present, so a time in ms could come too early on a slow machine */
        if (!p->held_since && frames >= 0 && (u32)frames >= p->at) p->held_since = now;
        if (!p->held_since) continue;
        if (now - p->held_since < len_ns)
            held |= p->mask;
        else
            p->done = 1;
    }
    return held;
}

const char* unattended_shot(void) { return g_shot; }

int unattended_periodic_shot(char* path, size_t n) {
    int bucket;
    if (!g_shot || g_shot_every <= 0) return 0;
    /* by bucket, not frames % shot_every: frames may move by two between looks */
    bucket = frames_now() / g_shot_every;
    if (bucket == g_shot_bucket) return 0;
    g_shot_bucket = bucket;
    snprintf(path, n, "%s_%04d", g_shot, g_shot_count++);
    return 1;
}

int unattended_done(void) {
    if (g_frame_limit >= 0 && frames_now() >= g_frame_limit) return 1;
    return g_seconds > 0 && g_t0 && plat_ns() - g_t0 >= (u64)g_seconds * 1000000000u;
}

void unattended_exit(u64 hash, int w, int h) {
    if (w > 0 && h > 0)
        fprintf(stderr, "\nframes=%d hash=%016llx %dx%d\n", frames_now(), (unsigned long long)hash, w, h);
    else
        fprintf(stderr, "\nframes=%d hash=%016llx\n", frames_now(), (unsigned long long)hash);
    ini_poll(1); /* the settings changed during the run, as a quit saves them */
    snx_save_before_exit();
    plat_exit_now(0);
}

void input_key_sticks(u64 k, i32 st[4]) {
    int s;
    for (s = 0; s < 2; s++) {
        u64 d = k >> (40 + 4 * s); /* left, up, right, down */
        st[2 * s] = ((i32)((d >> 2) & 1) - (i32)(d & 1)) * 32767;
        st[2 * s + 1] = ((i32)((d >> 1) & 1) - (i32)((d >> 3) & 1)) * 32767;
    }
}

void input_stick_clamp(i32* x, i32* y) {
    double r;
    if (*x < -32767) *x = -32767;
    if (*x > 32767) *x = 32767;
    if (*y < -32767) *y = -32767;
    if (*y > 32767) *y = 32767;
    r = sqrt((double)*x * *x + (double)*y * *y);
    if (r > 32767.0) {
        *x = (i32)(*x * (32767.0 / r));
        *y = (i32)(*y * (32767.0 / r));
    }
}
