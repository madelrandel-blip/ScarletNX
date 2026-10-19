/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's .ini: the sections it is written in (the runtime's own settings here, the window's, the on-screen
   controls' and the gamepads' from their modules), the command line's overrides over it, its reload while the game
   runs, and the values the game's menu changes, written back */
#include "nx/runtime/options.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services.h"
#include "nx/memory/mem.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/platform/vpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_opt_fullscreen, g_opt_docked = 1, g_opt_lang = 1, g_opt_fps = SNX_FPS_GAME, g_opt_diag, g_opt_shadows = 1;
int g_opt_prof, g_opt_auto_stub_ipc = 1, g_opt_network, g_opt_vsync = 1, g_opt_aniso;
float g_opt_resf = 1;
int g_opt_user = 1;
int g_opt_jit_threads, g_opt_async_pipes = 0;

/* ---- the runtime's own settings ---- */

/* a language the console doesn't have is English (US), as the clamp to the table's range would not say */
static void language_set(const IniOpt* o, const char* v) {
    char* end;
    long n = strtol(v, &end, 10);
    (void)o;
    if (end != v && (n < 0 || n >= set_language_count())) g_opt_lang = 1;
}

/* "game" in any case, or a number: 0 and below 1 unlimited, 10 to 1000 frames a second (clamped), and the values an
   older file meant the game's pace with (negative ones, and 1, its 60 FPS). Anything else leaves the limit */
static void fps_set(const IniOpt* o, const char* v) {
    char* end;
    double d;
    int fps;
    (void)o;
    while (*v == ' ' || *v == '\t') v++;
    if ((v[0] == 'g' || v[0] == 'G') && (v[1] == 'a' || v[1] == 'A') && (v[2] == 'm' || v[2] == 'M') &&
        (v[3] == 'e' || v[3] == 'E') && !v[4 + strspn(v + 4, " \t")]) {
        __atomic_store_n(&g_opt_fps, SNX_FPS_GAME, __ATOMIC_RELAXED);
        return;
    }
    d = strtod(v, &end);
    if (end == v || d != d) return;
    if (d < 0 || (d >= 1 && d < 2))
        fps = SNX_FPS_GAME;
    else if (d < 1)
        fps = 0;
    else
        fps = d < 10 ? 10 : d > 1000 ? 1000 : (int)d;
    __atomic_store_n(&g_opt_fps, fps, __ATOMIC_RELAXED);
}
void fps_limit_text(int fps, char* out, size_t n) {
    if (fps == SNX_FPS_GAME)
        snprintf(out, n, "game");
    else
        snprintf(out, n, "%d", fps);
}

int g_opt_ldn = SNX_OPT_LDN_LAN, g_opt_airplane;
char g_opt_room[256];
/* lan, room or off. Both keys are read once, at start: a value they don't take stops the game there, as a player
   who wrote ldn=rooom would otherwise play alone thinking they were in the room */
static void ldn_set(const IniOpt* o, const char* v) {
    static const char* const words[] = {"lan", "room", "off"};
    int i;
    (void)o;
    for (i = 0; i < 3; i++)
        if (!strcmp(v, words[i])) {
            g_opt_ldn = i;
            return;
        }
    fatal("settings: ldn=%s is not lan, room or off", v);
}
static void room_set(const IniOpt* o, const char* v) {
    (void)o;
    if (strlen(v) >= sizeof g_opt_room || v[strcspn(v, " \t\r")])
        fatal("settings: room=%.64s is not host or host:port", v);
    strcpy(g_opt_room, v);
}

/* MB: below 1 no cap but the heap region's own, which also caps a larger one */
static void heap_max_set(const IniOpt* o, const char* v) {
    char* end;
    double mb = strtod(v, &end);
    (void)o;
    if (end == v || mb != mb) return;
    if (mb < 1)
        g_heap_max = 0;
    else
        g_heap_max = mb * 1048576.0 >= (double)HEAP_SIZE ? HEAP_SIZE : (u64)(mb * 1048576.0);
}

#define END_ROW {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}
static const IniOpt g_display[] = {
    {"docked", "1", "1 = TV mode (the console in its dock), 0 = handheld mode (1280x720)", INI_INT, INI_STARTUP,
     &g_opt_docked, 0, 1, NULL, "TV mode", "Off|On", 0, 0},
    {"fullscreen", "0",
     "1 = starts fullscreen, except in timed test runs. -f or --fullscreen=0 sets it for one run, and F11 or\n"
     "Alt+Enter switches while the game runs without changing it",
     INI_INT, INI_STARTUP, &g_opt_fullscreen, 0, 1, NULL, "Fullscreen", "Off|On", 0, 0},
    {"language", "1",
     "The game's language: 0 Japanese, 1 English (US), 2 French, 3 German, 4 Italian, 5 Spanish, 6 Chinese, 7 Korean,\n"
     "8 Dutch, 9 Portuguese, 10 Russian, 11 Taiwanese, 12 English (UK), 13 French (Canada), 14 Spanish (Latin\n"
     "America), 15 Chinese (simplified), 16 Chinese (traditional), 17 Portuguese (Brazil)",
     INI_INT, INI_STARTUP, &g_opt_lang, 0, 17, language_set, "Language",
     "Japanese|English (US)|French|German|Italian|Spanish|Chinese|Korean|Dutch|Portuguese|Russian|Taiwanese|"
     "English (UK)|French (Canada)|Spanish (Latin America)|Chinese (simplified)|Chinese (traditional)|"
     "Portuguese (Brazil)",
     0, 0},
    END_ROW};
static const IniOpt g_graphics[] = {
    {"resolution", "1", "The scale of the game's 3D rendering, 0.25 to 4: 1 = the console's, 2 = double, 0.5 = half",
     INI_FLOAT, INI_STARTUP, &g_opt_resf, 0.25, 4, NULL, "Resolution scale", NULL, 0, 0},
    {"shadows", "1", "0 = skips shadow maps", INI_INT, 0, &g_opt_shadows, 0, 1, NULL, "Shadows", "Off|On", 0, 0},
    {"vsync", "1",
     "1 = images wait for the display's refresh, so nothing tears; 0 = the newest is shown at the next refresh\n"
     "the display allows, else at once",
     INI_INT, 0, &g_opt_vsync, 0, 1, NULL, "VSync", "Off|On", 0, 0},
    {"anisotropy", "0", "0 = textures filtered as the game asks, 2 to 16 = sharper surfaces seen at an angle", INI_INT,
     INI_STARTUP, &g_opt_aniso, 0, 16, NULL, "Anisotropic filtering", "Game's|#x", 0, 0},
    {"async_pipelines", "0",
     "1 = a draw whose shaders the GPU driver is still compiling is skipped until they are ready (the thing shows a\n"
     "moment late); 0 = the game waits for them, and stutters the first time it shows something new",
     INI_INT, 0, &g_opt_async_pipes, 0, 1, NULL, "Asynchronous shaders", "Off|On", 0, 0},
    END_ROW};
static const IniOpt g_performance[] = {
    {"fps_limit", "game",
     "game = the pace the game asks for, as on the console (60 FPS, or 30 for a game made for 30); 0 = no limit;\n"
     "10 to 1000 = that many frames a second. F4 turns the limit off and on",
     INI_TEXT, 0, NULL, 0, 0, fps_set, "FPS limit", NULL, 0, 0},
    {"memory_gb", "0",
     "The memory the game sees: below 6 the console's 3.2 GB, 6 = 4.8 GB, 8 = 6.8 GB. A game that runs out on\n"
     "the console's amount needs 6",
     INI_INT, INI_STARTUP, &g_hle.svcmem.opt_mem_gb, 0, 8, NULL, "Memory (GB)", NULL, 0, 0},
    {"heap_max_mb", "0", "The most heap the game can allocate, in MB; 0 = as much as its memory allows", INI_TEXT,
     INI_STARTUP, NULL, 0, 0, heap_max_set, "Heap limit (MB)", NULL, 0, 0},
    {"jit_threads", "0",
     "Threads compiling the code the recompiler missed: 0 = every CPU the game may use but two (at most 8).\n"
     ";   They yield to the game",
     INI_INT, INI_STARTUP, &g_opt_jit_threads, 0, 8, NULL, "JIT threads", "Auto|#", 0, 0}, /* SNX_JIT_THREADS */
    END_ROW};
static const IniOpt g_network[] = {
    {"network", "0",
     "1 = the game uses the computer's connection (known console service domains stay blocked); 0 = offline,\n"
     "with loopback only (not a sandbox)",
     INI_INT, 0, &g_opt_network, 0, 1, NULL, "Network", "Offline|Online", 0, 0},
    {"airplane_mode", "0",
     "1 = the console is in airplane mode: no network, whatever network says, and no local wireless (the game hears\n"
     "both are off)",
     INI_INT, INI_STARTUP, &g_opt_airplane, 0, 1, NULL, "Airplane mode", "Off|On", 0, 0},
    {"ldn", "lan",
     "Local wireless: lan = over the LAN with ldn_mitm's protocol (another ScarletNX, Ryujinx, a console running\n"
     "ldn_mitm); room = through the Eden room that room names (Eden, another ScarletNX); off = the game hears it\n"
     "is off",
     INI_TEXT, INI_STARTUP, NULL, 0, 0, ldn_set, "Local wireless", "lan=LAN|room=Eden room|off=Off", 0, 0},
    {"room", "",
     "The Eden room ldn=room joins: host or host:port (port 24872 without one). The nickname there is the user's,\n"
     "and a room's password is room_password in users/online.ini",
     INI_TEXT, INI_STARTUP, NULL, 0, 0, room_set, "Eden room", NULL, 0, 0},
    END_ROW};
static const IniOpt g_user[] = {
    {"user", "1",
     "The console's user the game plays as: 1 to 8, as users/users.ini of the data folder lists them (1, \"Player\",\n"
     "without one). A number with no user stops the game",
     INI_INT, INI_STARTUP, &g_opt_user, 1, SNX_USERS_MAX, NULL, NULL, NULL, 0, 0},
    END_ROW};
static const IniOpt g_debug[] = {
    {"log", "1", "The log's detail, 0 to 2: 2 logs everything, which slows the game down", INI_INT, 0, &g_verbose, 0,
     2, NULL, NULL, NULL, 0, 0},
    {"auto_stub_ipc", "1",
     "1 = a service command the runtime doesn't know answers success (logged as [undeclared]); 0 = it answers\n"
     "the console's UnknownCommandId, which shows the gap where the game makes the call",
     INI_INT, 0, &g_opt_auto_stub_ipc, 0, 1, NULL, "Answer unknown commands", "Off|On", 0, 0},
    {"diag", "0",
     "1 = saves each new compute shader's data to captures/compute, and each new shader to captures/shaders\n"
     "(snx-shaderc's corpus)",
     INI_INT, 0, &g_opt_diag, 0, 1, NULL, "Shader captures", "Off|On", 0, 0},
    {"profile", "0", "1 = writes profile.txt every 10 seconds (the CPU profiler is Windows only)", INI_INT,
     INI_STARTUP, &g_opt_prof, 0, 1, NULL, "Profiler", "Off|On", 0, 0},
    END_ROW};

/* The file's sections in its order. A section has up to three tables, and a table with a page has a page of its own
   in the menus, listed after the section's other keys. own: the tables here, which ini_read applies; the others are
   applied through backend_ini, which takes the on-screen controls' lock */
typedef struct {
    const char* name;
    int own;
    const IniOpt* table[3];
    const char* page[3];
} Section;
static const Section g_sections[] = {
    {"Display", 1, {g_display, NULL, NULL}, {NULL, NULL, NULL}},
    {"Graphics", 1, {g_graphics, NULL, NULL}, {NULL, NULL, NULL}},
    {"Performance", 1, {g_performance, NULL, NULL}, {NULL, NULL, NULL}},
    {SNX_VPAD_PAGE, 0, {g_vpad_ini, g_backend_ini, g_vpad_shown_ini}, {NULL, NULL, SNX_VPAD_BUTTONS}},
    {SNX_GAMEPAD_PAGE,
     0,
     {g_gamepad_ini, g_gamepad_map_ini, g_gamepad_players_ini},
     {NULL, SNX_GAMEPAD_MAP, SNX_GAMEPAD_PLAYERS}},
    {"Network", 1, {g_network, NULL, NULL}, {NULL, NULL, NULL}},
    {"User", 1, {g_user, NULL, NULL}, {NULL, NULL, NULL}},
    {"Debug", 1, {g_debug, NULL, NULL}, {NULL, NULL, NULL}}};
#define SECTIONS ((int)(sizeof g_sections / sizeof *g_sections))

/* key's row in a section, with whether it is one of the runtime's own; NULL for none */
static const IniOpt* find_row(const char* key, int* own) {
    int s, t;
    for (s = 0; s < SECTIONS; s++)
        for (t = 0; t < 3 && g_sections[s].table[t]; t++) {
            const IniOpt* o = ini_find(g_sections[s].table[t], key);
            if (o) {
                if (own) *own = g_sections[s].own;
                return o;
            }
        }
    return NULL;
}

int snx_setting_known(const char* key) { return find_row(key, NULL) || unattended_known(key); }

/* ---- the file's values, the command line's, and the menu's waiting to be written ---- */

#define KEY_MAX 64
#define STORE_MAX 128
#define VALUE_MAX 2048
#define OVERRIDE_MAX 64
#define PENDING_MAX 64
#define PENDING_VALUE 256 /* a gamepad map's text is up to 165 bytes (pad_map.h) */

/* The file as last read, with the menu's changes merged in: only the thread that opens and polls it touches it */
typedef struct {
    char key[KEY_MAX];
    char value[VALUE_MAX];
} Entry;
static Entry g_store[STORE_MAX];
static int g_nstore;
/* the command line's, set before ini_open and only read after */
typedef struct {
    char key[KEY_MAX];
    const char* value;
} Override;
static Override g_over[OVERRIDE_MAX];
static int g_nover;
/* the menu's, from any thread, until a write of the file has them. g_pending_lock is a leaf: nothing is called while
   it is held */
typedef struct {
    char key[KEY_MAX];
    char value[PENDING_VALUE];
} Pending;
static Pending g_pending[PENDING_MAX];
static int g_npending;
static PMutex g_pending_lock = PMUTEX_INIT;
static PMutex g_poll_lock = PMUTEX_INIT;

static char g_path[1400];
static int g_started; /* ini_open is done: the startup settings are kept from now on */
static u64 g_hash;    /* the file as this run last read or wrote it */
static u64 g_seen;    /* a changed file seen by the last poll, read once a second poll sees it unchanged */

static u64 fnv1a(const unsigned char* p, size_t n) {
    u64 h = SNX_U64C(0xCBF29CE4u, 0x84222325u);
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * SNX_U64C(0x100u, 0x000001B3u);
    return h;
}

static const char* override_of(const char* key) {
    int i;
    for (i = 0; i < g_nover; i++)
        if (!strcmp(g_over[i].key, key)) return g_over[i].value;
    return NULL;
}
static Entry* store_find(const char* key) {
    int i;
    for (i = 0; i < g_nstore; i++)
        if (!strcmp(g_store[i].key, key)) return &g_store[i];
    return NULL;
}
/* a value that doesn't fit is dropped, never cut */
static void store_put(const char* key, const char* value) {
    Entry* e = store_find(key);
    size_t n = strlen(value);
    if (strlen(key) >= KEY_MAX) {
        WARN_LIMIT(1, "settings: key %.40s... is too long and is left out\n", key);
        return;
    }
    if (n >= VALUE_MAX) {
        WARN_LIMIT(1, "settings: the value of %s is longer than %d bytes and is left out\n", key, VALUE_MAX - 1);
        return;
    }
    if (!e) {
        if (g_nstore == STORE_MAX) {
            WARN_LIMIT(1, "settings: more than %d keys, %s and those after it are left out\n", STORE_MAX, key);
            return;
        }
        e = &g_store[g_nstore++];
        memcpy(e->key, key, strlen(key) + 1);
    }
    memcpy(e->value, value, n + 1);
}
static void store_parse(char* text) {
    SnxIni ini;
    char *key, *value;
    g_nstore = 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) store_put(key, value);
}
static const char* value_of(const IniOpt* o) {
    const char* v = override_of(o->key);
    const Entry* e;
    if (v) return v;
    e = store_find(o->key);
    return e ? e->value : o->def;
}

void ini_override(const char* k, const char* v) {
    size_t n = strlen(k);
    int i;
    if (n >= KEY_MAX) return;
    for (i = 0; i < g_nover && strcmp(g_over[i].key, k); i++) continue;
    if (i == OVERRIDE_MAX) {
        WARN_LIMIT(1, "settings: more than %d settings on the command line, --%s and those after it do nothing\n",
                   OVERRIDE_MAX, k);
        return;
    }
    if (i == g_nover) memcpy(g_over[g_nover++].key, k, n + 1);
    g_over[i].value = v;
}

void ini_set(const char* k, const char* v) {
    size_t kn = strlen(k), vn = strlen(v);
    int i;
    if (override_of(k)) return; /* this run's value, not the file's */
    if (kn >= KEY_MAX || vn >= PENDING_VALUE) {
        WARN_LIMIT(1, "settings: %s's value is too long to save\n", k);
        return;
    }
    pm_lock(&g_pending_lock);
    for (i = 0; i < g_npending && strcmp(g_pending[i].key, k); i++) continue;
    if (i < PENDING_MAX) {
        if (i == g_npending) memcpy(g_pending[g_npending++].key, k, kn + 1);
        memcpy(g_pending[i].value, v, vn + 1);
    }
    pm_unlock(&g_pending_lock);
    if (i == PENDING_MAX) WARN_LIMIT(1, "settings: too many changes at once, %s is not saved\n", k);
}

/* the menu's changes into the store: their copy in taken, for pending_written once the file has them */
static int pending_take(Pending* taken) {
    int n, i;
    pm_lock(&g_pending_lock);
    n = g_npending;
    memcpy(taken, g_pending, (size_t)n * sizeof *taken);
    pm_unlock(&g_pending_lock);
    for (i = 0; i < n; i++) store_put(taken[i].key, taken[i].value);
    return n;
}
/* drops the changes the file now has; one set again meanwhile stays for the next write */
static void pending_written(const Pending* taken, int n) {
    int i, j;
    pm_lock(&g_pending_lock);
    for (i = 0; i < n; i++)
        for (j = 0; j < g_npending; j++)
            if (!strcmp(g_pending[j].key, taken[i].key)) {
                if (!strcmp(g_pending[j].value, taken[i].value)) g_pending[j] = g_pending[--g_npending];
                break;
            }
    pm_unlock(&g_pending_lock);
}

/* ---- applying ---- */

static void apply(const char* key, const char* value) {
    int own = 0;
    const IniOpt* o;
    if (ini_deprecated(key)) return; /* kept in the file, never read */
    o = find_row(key, &own);
    if (o && g_started && (o->flags & INI_STARTUP)) return;
    if (o && own)
        ini_read(o, value);
    else if (o || unattended_known(key))
        backend_ini(key, value);
}
/* every setting: the command line's value, else the file's, else its default; then the keys of no section the
   unattended options take */
static void apply_all(void) {
    int s, t, i;
    for (s = 0; s < SECTIONS; s++)
        for (t = 0; t < 3 && g_sections[s].table[t]; t++) {
            const IniOpt* o;
            for (o = g_sections[s].table[t]; o->key; o++) {
                const char* v = value_of(o);
                if (v) apply(o->key, v);
            }
        }
    for (i = 0; i < g_nstore; i++)
        if (!find_row(g_store[i].key, NULL) && !override_of(g_store[i].key)) apply(g_store[i].key, g_store[i].value);
    for (i = 0; i < g_nover; i++)
        if (!find_row(g_over[i].key, NULL)) apply(g_over[i].key, g_over[i].value);
}

/* ---- writing ---- */

static void put_help(FILE* f, const char* help) {
    const char* nl;
    fputs("; ", f);
    while ((nl = strchr(help, '\n')) != NULL) {
        fprintf(f, "%.*s\n;   ", (int)(nl - help), help);
        help = nl + 1;
    }
    fprintf(f, "%s\n", help);
}
static void put_header(FILE* f) {
    const char* title = g_mod && g_mod->title && g_mod->title[0] ? g_mod->title : g_exename;
    int s, t, col = 0;
    fprintf(f, "; ==================================================================================\n");
    fprintf(f, ";  %s: ScarletNX settings\n", title);
    fprintf(f, ";  Most settings apply while the game runs. These apply when it next starts:\n;   ");
    for (s = 0; s < SECTIONS; s++)
        for (t = 0; t < 3 && g_sections[s].table[t]; t++) {
            const IniOpt* o;
            for (o = g_sections[s].table[t]; o->key; o++) {
                if (!(o->flags & INI_STARTUP)) continue;
                if (col && col + (int)strlen(o->key) + 2 > 70) {
                    fprintf(f, ",\n;   ");
                    col = 0;
                } else if (col) {
                    fprintf(f, ", ");
                    col += 2;
                }
                fputs(o->key, f);
                col += (int)strlen(o->key);
            }
        }
    fprintf(f, "\n;  In the game: F1 help, F2 the game's menu, F3 screenshot, F4 FPS limit off and on, F5 thread\n");
    fprintf(f, ";  dump, F11 or Alt+Enter fullscreen\n");
    fprintf(f, "; ==================================================================================\n");
}
static void put_rows(FILE* f, const IniOpt* o) {
    for (; o->key; o++) {
        const Entry* e = store_find(o->key);
        const char* v = e ? e->value : o->def;
        if (!v) continue; /* no default and never set */
        if (o->help) put_help(f, o->help);
        fprintf(f, "%s=%s\n", o->key, v);
    }
}
/* the store into the file: beside it, then moved over it, so the file is always whole. 0, or -1 (warned once) */
static int write_file(void) {
    char tmp[sizeof g_path + 8];
    FILE* f;
    int s, t, i, other = 0, failed;
    unsigned char* text;
    size_t size;
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    f = fopen(tmp, "wb");
    if (!f) {
        WARN_LIMIT(1, "settings: could not write %s\n", tmp);
        return -1;
    }
    put_header(f);
    for (s = 0; s < SECTIONS; s++) {
        fprintf(f, "\n[%s]\n", g_sections[s].name);
        for (t = 0; t < 3 && g_sections[s].table[t]; t++) {
            if (g_sections[s].page[t]) fprintf(f, "; -- %s --\n", g_sections[s].page[t]);
            put_rows(f, g_sections[s].table[t]);
        }
    }
    for (i = 0; i < g_nstore; i++) /* a newer version's keys, typos, the unattended options: as they are */
        if (!find_row(g_store[i].key, NULL) && !ini_deprecated(g_store[i].key)) {
            if (!other++) fprintf(f, "\n[Other]\n; keys this version does not know, kept as they are\n");
            fprintf(f, "%s=%s\n", g_store[i].key, g_store[i].value);
        }
    for (i = 0, other = 0; i < g_nstore; i++) /* last, so the launcher's Clean old settings cuts to the end */
        if (ini_deprecated(g_store[i].key)) {
            if (!other++) ini_deprecated_head(f);
            ini_deprecated_key(f, g_store[i].key, g_store[i].value);
        }
    failed = ferror(f);
    if (fclose(f)) failed = 1;
    if (failed || plat_replace_file(tmp, g_path)) {
        remove(tmp);
        WARN_LIMIT(1, "settings: could not write %s, it stays as it was\n", g_path);
        return -1;
    }
    text = snx_read_file(g_path, &size);
    g_hash = g_seen = text ? fnv1a(text, size) : 0;
    free(text);
    return 0;
}

static void say_deprecated(void) {
    const char* keys[STORE_MAX];
    char line[2048];
    int i, n = 0;
    for (i = 0; i < g_nstore; i++)
        if (ini_deprecated(g_store[i].key)) keys[n++] = g_store[i].key;
    if (ini_deprecated_note(line, sizeof line, g_path, keys, n)) WARN("%s\n", line);
}

void ini_open(const char* path) {
    Pending taken[PENDING_MAX];
    unsigned char* text;
    size_t size;
    int i, n;
    snprintf(g_path, sizeof g_path, "%s", path);
    for (i = 0; i < g_nover; i++)
        if (!snx_setting_known(g_over[i].key)) WARN("--%s is not a setting, so it does nothing\n", g_over[i].key);
    text = snx_read_file(g_path, &size);
    if (!text && snx_path_exists(g_path)) {
        WARN("settings: %s cannot be read: the defaults apply, and it is left as it is\n", g_path);
        apply_all();
        g_started = 1;
        return;
    }
    if (text) {
        store_parse((char*)text);
        g_hash = g_seen = fnv1a(text, size);
        free(text);
    }
    n = pending_take(taken);
    say_deprecated();
    apply_all();
    if (!write_file()) pending_written(taken, n);
    g_started = 1;
}

void ini_poll(int final) {
    Pending taken[PENDING_MAX];
    unsigned char* text;
    size_t size;
    int pending, n;
    u64 h;
    if (!g_path[0]) return;
    pm_lock(&g_poll_lock);
    pm_lock(&g_pending_lock);
    pending = g_npending;
    pm_unlock(&g_pending_lock);
    text = snx_read_file(g_path, &size);
    if (!text) {
        /* unreadable: left alone. Gone: made again when there is something to save */
        if (!snx_path_exists(g_path) && pending) {
            n = pending_take(taken);
            if (!write_file()) pending_written(taken, n);
        }
        pm_unlock(&g_poll_lock);
        return;
    }
    h = fnv1a(text, size);
    if (h != g_hash && (final ? pending : h == g_seen)) {
        /* changed, and settled (seen the same a second before), or the menu's changes are to be saved at exit */
        store_parse((char*)text);
        n = pending_take(taken);
        apply_all();
        LOG("settings: %s reloaded\n", g_path);
        if (!write_file()) pending_written(taken, n);
    } else if (h != g_hash)
        g_seen = h; /* changed: maybe still being saved, read at the next poll if it stays so */
    else if (pending) {
        n = pending_take(taken);
        if (!write_file()) pending_written(taken, n);
    }
    free(text);
    pm_unlock(&g_poll_lock);
}

/* ---- the listing for the launcher ---- */

static void list_help(const char* help) {
    for (; *help; help++)
        if (*help == '\n') {
            while (help[1] == ' ' || help[1] == ';') help++;
            putchar(' ');
        } else
            putchar(*help);
}
static void list_values(const IniOpt* o) {
    int step = o->step > 0 ? o->step : 1, v, first = 1;
    char word[64];
    if (o->type == INI_TEXT && o->words) { /* value=word already */
        printf("%s", o->words);
        return;
    }
    if (o->type != INI_INT) return;
    if (!o->words && (o->lo < 0 || (o->hi - o->lo) / step + 1 > 16)) return; /* typed */
    for (v = (int)o->lo; v <= (int)o->hi; v += step) {
        if (o->words)
            ini_word(o, v, word, sizeof word);
        else
            snprintf(word, sizeof word, "%d", v);
        printf("%s%d=%s", first ? "" : "|", v, word);
        first = 0;
    }
}
void ini_list(void) {
    static const char* const types[] = {"int", "float", "text"};
    const char* help = "";
    int s, t;
    for (s = 0; s < SECTIONS; s++)
        for (t = 0; t < 3 && g_sections[s].table[t]; t++) {
            const IniOpt* o;
            for (o = g_sections[s].table[t]; o->key; o++) {
                if (o->help) help = o->help; /* none: the one before covers it */
                if (o->flags & INI_IN_GAME) continue;
                printf("%s%s%s\t%s\t%s\t%s\t%g\t%g\t", g_sections[s].name, g_sections[s].page[t] ? "/" : "",
                       g_sections[s].page[t] ? g_sections[s].page[t] : "", o->key, o->def ? o->def : "",
                       types[o->type], o->lo, o->hi);
                list_help(help);
                printf("\t%s\t", o->name ? o->name : "");
                list_values(o);
                putchar('\n');
            }
        }
}
