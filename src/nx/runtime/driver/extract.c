/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Switch container extraction with hactool in this process (hactool/snx_hac.h): each NCA read where it lies in the
   XCI or NSP, the ExeFS and RomFS written straight into the output. Keys go from their files into hactool's keyset in
   memory, and nothing hactool prints reaches the console but its last error, keys left out */
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/hactool/snx_hac.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *base, *update, *name, *out, *keys, *title_keys;
} ExtractOptions;

typedef struct {
    const char *name, *value;
    size_t name_size, value_size;
} KeyLine;

typedef struct {
    char *base, *update, *out, *keys, *title_keys;
    char *exefs, *romfs;
    int made_out, base_xci, update_xci, has_keys;
    SnxHac* hac;
    /* the progress row: what the extraction will have written, and when it was said last */
    SnxU64 total, written;
    double said_at;
    int said;
} ExtractState;

/* an NCA where it lies: a container's number (snx_hac_open) and its entry, found 0 for none */
typedef struct {
    int found, container;
    size_t entry;
    SnxU64 size, title_id;
} NcaRef;

static void extract_help(void) {
    puts("Usage: scarletnx -x BASE.xci|BASE.nsp|BASE.dxci|BASE.dnsp [options]\n"
         "  --update FILE       Apply a same-title update during extraction\n"
         "  --name NAME         Output name (default: base dump stem)\n"
         "  --out DIR           Output directory (default: REPO/work/NAME)\n"
         "  --keys FILE         prod.keys (default: REPO, then ~/.switch)\n"
         "  --title-keys FILE   title.keys (same discovery order; optional)\n"
         "  --help              Show this help");
    puts("REPO is the data folder: --repo, else a portable copy's or the user's (scarletnx --help).");
#ifdef SNX_XDG
    puts("The user's data folder keeps work/ in its cache folder, $XDG_CACHE_HOME/scarletnx (~/.cache/scarletnx).");
#endif
    puts("Decrypted content needs no keys. Refuses output containing exefs.");
}

static char* text_copy(const char* text, size_t size) {
    char* copy;
    if (size == (size_t)-1) return NULL;
    copy = (char*)malloc(size + 1);
    if (copy) {
        memcpy(copy, text, size);
        copy[size] = 0;
    }
    return copy;
}

static int option_is(const char* arg, const char* name) {
    size_t size;
    size = strlen(name);
    return !strncmp(arg, name, size) && (!arg[size] || arg[size] == '=');
}

static int parse_options(int argc, char** argv, ExtractOptions* options) {
    int i, positional;
    const char **slot, *value;
    memset(options, 0, sizeof *options);
    positional = 0;
    for (i = 0; i < argc; i++) {
        if (!positional && (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))) {
            extract_help();
            return 2;
        }
        if (!positional && !strcmp(argv[i], "--")) {
            positional = 1;
            continue;
        }
        if (positional || argv[i][0] != '-' || !argv[i][1]) {
            if (options->base) return snx_error("Extraction accepts one base dump");
            options->base = argv[i];
            continue;
        }
        slot = NULL;
        if (option_is(argv[i], "--update"))
            slot = &options->update;
        else if (option_is(argv[i], "--name"))
            slot = &options->name;
        else if (option_is(argv[i], "--out"))
            slot = &options->out;
        else if (option_is(argv[i], "--keys"))
            slot = &options->keys;
        else if (option_is(argv[i], "--title-keys"))
            slot = &options->title_keys;
        if (!slot) return snx_error("Unknown extraction option: %s", argv[i]);
        value = strchr(argv[i], '=');
        if (value)
            value++;
        else {
            if (i + 1 == argc || !strncmp(argv[i + 1], "--", 2))
                return snx_error("Extraction option requires a value: %s", argv[i]);
            value = argv[++i];
        }
        if (!value[0]) return snx_error("Extraction option has an empty value");
        *slot = value;
    }
    if (!options->base) {
        extract_help();
        return 1;
    }
    return 0;
}

static int ascii_name(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int ascii_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int has_suffix(const char* text, const char* suffix) {
    size_t a, b, i;
    a = strlen(text);
    b = strlen(suffix);
    if (a < b) return 0;
    for (i = 0; i < b; i++)
        if (ascii_lower(text[a - b + i]) != ascii_lower(suffix[i])) return 0;
    return 1;
}

/* A gamecard dump can start with the card's 0x1000-byte key area, its header after it (LibHac's XciHeader) */
static int container_kind(const char* path, int* xci) {
    unsigned char header[0x1104];
    size_t size;
    FILE* file;
    int failed;
    file = snx_fopen(path, "rb");
    if (!file) return snx_error("Cannot read container header: %s", path);
    size = fread(header, 1, sizeof header, file);
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) return snx_error("Cannot read container header: %s", path);
    if (size >= 4 && !memcmp(header, "PFS0", 4)) {
        *xci = 0;
        return 0;
    }
    if ((size >= 0x104 && (!memcmp(header + 0x100, "HEAD", 4) || !memcmp(header + 0x100, "DXCI", 4))) ||
        (size == sizeof header && (!memcmp(header + 0x1100, "HEAD", 4) || !memcmp(header + 0x1100, "DXCI", 4)))) {
        *xci = 1;
        return 0;
    }
    return snx_error("Unsupported container header. Expected NSP/DNSP (PFS0) or XCI/DXCI (HEAD/DXCI)");
}

static char* dump_name(const char* path) {
    const char *p, *base, *dot;
    base = path;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    if (!dot || dot == base) dot = base + strlen(base);
    return text_copy(base, (size_t)(dot - base));
}

static char* find_keys(const char* given, const char* name, const SnxPaths* paths) {
    const char* home;
    char *in_repo, *directory, *in_home, *absolute;
    if (given) return snx_absolute(given);
    in_repo = snx_path_join(paths->repo, name);
    if (!in_repo || snx_is_file(in_repo)) return in_repo;
#if defined(_WIN32)
    home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home || !home[0]) return in_repo;
    directory = snx_path_join(home, ".switch");
    if (!directory) {
        free(in_repo);
        return NULL;
    }
    in_home = snx_path_join(directory, name);
    free(directory);
    if (!in_home) {
        free(in_repo);
        return NULL;
    }
    if (!snx_is_file(in_home)) {
        free(in_home);
        return in_repo;
    }
    absolute = snx_absolute(in_home);
    free(in_home);
    free(in_repo);
    return absolute;
}

static int key_line(const char* text, size_t size, KeyLine* line) {
    size_t i, start;
    i = 0;
    while (i < size && isspace((unsigned char)text[i])) i++;
    start = i;
    while (i < size && ascii_name((unsigned char)text[i])) i++;
    if (i == start) return 0;
    line->name = text + start;
    line->name_size = i - start;
    while (i < size && isspace((unsigned char)text[i])) i++;
    if (i == size || text[i++] != '=') return 0;
    while (i < size && isspace((unsigned char)text[i])) i++;
    start = i;
    while (i < size && ascii_hex((unsigned char)text[i])) i++;
    if (i == start) return 0;
    line->value = text + start;
    line->value_size = i - start;
    while (i < size && isspace((unsigned char)text[i])) i++;
    return i == size;
}

static void secret_free(void* memory, size_t size) {
    volatile unsigned char* p;
    p = (volatile unsigned char*)memory;
    if (!p) return;
    while (size) {
        *p++ = 0;
        size--;
    }
    free(memory);
}

static SnxU64 le_bytes(const unsigned char* p, unsigned n) {
    SnxU64 value = 0;
    while (n--) value = value << 8 | p[n];
    return value;
}

static int prepare_paths(ExtractState* state, const ExtractOptions* options, const SnxPaths* paths) {
    char *name, *work, *target;
    int made_out;
    state->base = snx_absolute(options->base);
    if (options->update) state->update = snx_absolute(options->update);
    state->keys = find_keys(options->keys, "prod.keys", paths);
    state->title_keys = find_keys(options->title_keys, "title.keys", paths);
    if (!state->base || (options->update && !state->update) || !state->keys || !state->title_keys)
        return snx_error("Cannot resolve extraction input paths");
    if (!snx_is_file(state->base) || (state->update && !snx_is_file(state->update)))
        return snx_error("Base or update dump is missing or is not a file");
    if (container_kind(state->base, &state->base_xci) ||
        (state->update && container_kind(state->update, &state->update_xci)))
        return 1;
    state->has_keys = snx_is_file(state->keys);
    if (options->keys && !state->has_keys) return snx_error("Specified prod.keys file is missing");
    name = options->name ? text_copy(options->name, strlen(options->name)) : dump_name(state->base);
    if (!name) return snx_error("Out of memory resolving extraction output");
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strpbrk(name, "/\\:")) {
        free(name);
        return snx_error("Extraction name must be a single nonempty filename");
    }
    /* only when --out names none: play_dump names one, and has said where an older build's extractions are */
    work = options->out ? NULL : snx_work_dir(paths->repo);
    target = work ? snx_path_join(work, name) : NULL;
    free(work);
    free(name);
    if (!options->out && !target) return snx_error("Out of memory resolving extraction output");
    state->out = snx_absolute(options->out ? options->out : target);
    free(target);
    if (!state->out) return snx_error("Cannot resolve extraction output");
    state->exefs = snx_path_join(state->out, "exefs");
    state->romfs = snx_path_join(state->out, "romfs");
    if (!state->exefs || !state->romfs) return snx_error("Out of memory resolving extraction output");
    if (snx_is_file(state->exefs) || snx_is_dir(state->exefs) || snx_is_link(state->exefs))
        return snx_error("Output already contains exefs: %s", state->out);
    if (snx_is_file(state->out)) return snx_error("Extraction output is a file");
    made_out = !snx_is_dir(state->out);
    if (snx_mkdirs(state->out)) return snx_error("Cannot create extraction output");
    state->made_out = made_out;
    return 0;
}


/* hactool's last error after what failed, when it said one */
static int hac_error(const char* what) {
    const char* said = snx_hac_message();
    return said[0] ? snx_error("%s (hactool: %s)", what, said) : snx_error("%s", what);
}

static int hex_bytes(const char* text, size_t size, unsigned char* out) {
    size_t i;
    unsigned digit;
    for (i = 0; i < size; i++) {
        if (!ascii_hex((unsigned char)text[i])) return -1;
        digit = text[i] <= '9' ? (unsigned)(text[i] - '0') : (unsigned)(ascii_lower(text[i]) - 'a' + 10);
        if (i & 1)
            out[i / 2] = (unsigned char)(out[i / 2] | digit);
        else
            out[i / 2] = (unsigned char)(digit << 4);
    }
    return 0;
}

/* prod.keys into hactool's keyset. A value of a length no key has is left out, as hactool would stop on it */
static int load_prod_keys(ExtractState* state) {
    unsigned char* data;
    size_t size, start, end;
    unsigned long dropped;
    char *name, *value;
    KeyLine line;
    int failed;
    data = snx_read_file(state->keys, &size);
    if (!data) return snx_error("Cannot read prod.keys");
    start = 0;
    dropped = 0;
    failed = 0;
    while (start < size && !failed) {
        end = start;
        while (end < size && data[end] != '\n') end++;
        if (key_line((const char*)data + start, end - start, &line)) {
            if (line.value_size % 32)
                dropped++;
            else {
                name = text_copy(line.name, line.name_size);
                value = text_copy(line.value, line.value_size);
                if (!name || !value)
                    failed = snx_error("Out of memory reading prod.keys");
                else
                    snx_hac_set_key(state->hac, name, value);
                if (name) secret_free(name, line.name_size);
                if (value) secret_free(value, line.value_size);
            }
        }
        start = end < size ? end + 1 : end;
    }
    secret_free(data, size);
    if (failed) return 1;
    snx_hac_derive_keys(state->hac);
    if (dropped) printf("Left out %lu malformed key lines\n", dropped);
    return 0;
}

/* title.keys, when there is one: a rights ID and its title key a line */
static int load_title_keys(ExtractState* state) {
    unsigned char *data, rights[16], key[16];
    size_t size, start, end;
    KeyLine line;
    if (!snx_is_file(state->title_keys)) return 0;
    data = snx_read_file(state->title_keys, &size);
    if (!data) return snx_error("Cannot read title.keys");
    start = 0;
    while (start < size) {
        end = start;
        while (end < size && data[end] != '\n') end++;
        if (key_line((const char*)data + start, end - start, &line) && line.name_size == 32 && line.value_size == 32 &&
            !hex_bytes(line.name, 32, rights) && !hex_bytes(line.value, 32, key))
            snx_hac_add_title_key(state->hac, rights, key);
        start = end < size ? end + 1 : end;
    }
    memset(key, 0, sizeof key);
    secret_free(data, size);
    return 0;
}

/* the title keys of a container's tickets: common tickets, whose key is stored as is */
static int read_tickets(ExtractState* state, int container) {
    unsigned char data[1024];
    const SnxHacEntry* entries;
    size_t count, i, size, offset;
    count = snx_hac_entries(state->hac, container, &entries);
    for (i = 0; i < count; i++) {
        if (!has_suffix(entries[i].name, ".tik")) continue;
        size = entries[i].size < sizeof data ? (size_t)entries[i].size : sizeof data;
        if (snx_hac_read(state->hac, container, i, data, size)) return snx_error("Cannot read a ticket");
        if (size < 4) continue;
        switch ((SnxU32)le_bytes(data, 4)) {
            case 0x10000UL:
            case 0x10003UL: offset = 4 + 0x200 + 0x3c; break;
            case 0x10001UL:
            case 0x10004UL: offset = 4 + 0x100 + 0x3c; break;
            case 0x10002UL:
            case 0x10005UL: offset = 4 + 0x3c + 0x40; break;
            case 0x10006UL: offset = 4 + 0x14 + 0x28; break;
            default: continue;
        }
        if (size < offset + 0x170 || data[offset + 0x141]) continue; /* a personalized ticket's key is encrypted */
        snx_hac_add_title_key(state->hac, data + offset + 0x160, data + offset + 0x40);
    }
    memset(data, 0, sizeof data);
    return 0;
}

/* 1 when an NCA's header is encrypted: a decrypted one (DNCA) or a plain NCA3 or NCA2 header needs no keys */
static int nca_needs_keys(ExtractState* state, int container, size_t entry) {
    unsigned char header[0x204];
    if (snx_hac_read(state->hac, container, entry, header, sizeof header)) return 1;
    return memcmp(header + 0x200, "DNCA", 4) && memcmp(header + 0x200, "NCA3", 4) && memcmp(header + 0x200, "NCA2", 4);
}

static void take(NcaRef* ref, int container, size_t entry, const SnxHacEntry* e, SnxU64 title_id) {
    ref->found = 1;
    ref->container = container;
    ref->entry = entry;
    ref->size = e->size;
    ref->title_id = title_id;
}

/* the Program NCA of a container to extract: the base's, or with update the patch of expected_title; the patch an NSP of
   both has (embedded_patch), and the Control NCA of the same title (control). The biggest when there are several */
static int select_ncas(ExtractState* state, int container, int update, SnxU64 expected_title, NcaRef* selected,
                       NcaRef* embedded_patch, NcaRef* control) {
    const SnxHacEntry* entries;
    SnxHacNca* ncas;
    size_t count, i;
    unsigned long unreadable;
    int patch;
    const SnxU64 update_bit = 0x800UL;
    memset(selected, 0, sizeof *selected);
    if (embedded_patch) memset(embedded_patch, 0, sizeof *embedded_patch);
    memset(control, 0, sizeof *control);
    count = snx_hac_entries(state->hac, container, &entries);
    ncas = (SnxHacNca*)calloc(count ? count : 1, sizeof *ncas);
    if (!ncas) return snx_error("Out of memory selecting Program NCA");
    unreadable = 0;
    for (i = 0; i < count; i++) {
        ncas[i].type = SNX_HAC_OTHER;
        if (!has_suffix(entries[i].name, ".nca") || has_suffix(entries[i].name, ".cnmt.nca")) continue;
        if (!state->has_keys && nca_needs_keys(state, container, i)) {
            free(ncas);
            return snx_error("Encrypted content needs prod.keys. Provide --keys, or put it at %s or in ~/.switch",
                             state->keys);
        }
        if (snx_hac_nca(state->hac, container, i, &ncas[i])) {
            ncas[i].type = SNX_HAC_OTHER;
            unreadable++;
        }
    }
    for (i = 0; i < count; i++) {
        patch = (ncas[i].title_id & update_bit) || ncas[i].patch;
        if (ncas[i].type != SNX_HAC_PROGRAM || (selected->found && entries[i].size <= selected->size)) continue;
        if (update ? patch && (ncas[i].title_id & ~update_bit) == expected_title : !patch)
            take(selected, container, i, &entries[i], ncas[i].title_id);
    }
    for (i = 0; selected->found && i < count; i++) {
        patch = (ncas[i].title_id & update_bit) || ncas[i].patch;
        if (embedded_patch && ncas[i].type == SNX_HAC_PROGRAM && patch &&
            (ncas[i].title_id & ~update_bit) == selected->title_id &&
            (!embedded_patch->found || entries[i].size > embedded_patch->size))
            take(embedded_patch, container, i, &entries[i], ncas[i].title_id);
        if (!control->found && ncas[i].type == SNX_HAC_CONTROL && /* the same game's, base or update */
            (ncas[i].title_id & ~update_bit) == (selected->title_id & ~update_bit))
            take(control, container, i, &entries[i], ncas[i].title_id);
    }
    free(ncas);
    if (selected->found) return 0;
    if (unreadable) return hac_error("The NCAs' headers cannot be decrypted. Check that prod.keys is current");
    return snx_error(update ? "Update has no Program NCA matching the selected base game"
                            : "Container has no base Program NCA (possibly an update or DLC). Supply a base game");
}

/* "  extracted 1229 of 4096 MB (30%)", ending in a carriage return while hactool writes: a terminal and the launcher's
   console write the next over it, the last one ends the row */
static void progress_say(ExtractState* state, int last) {
    SnxU64 done = state->written > state->total ? state->total : state->written;
    printf("  extracted %lu of %lu MB (%d%%)%s", (unsigned long)(done >> 20), (unsigned long)(state->total >> 20),
           state->total ? (int)(done * 100 / state->total) : 100, last ? "\n" : "\r");
    fflush(stdout);
    state->said = 1;
}

/* once a second at most: hactool writes in pieces of a few megabytes */
static void progress_tick(uint64_t written, void* data) {
    ExtractState* state = (ExtractState*)data;
    double now = snx_seconds();
    state->written = written;
    if (now - state->said_at < 1.0) return;
    state->said_at = now;
    progress_say(state, 0);
}

/* the game's control.nacp beside exefs/, where the loader reads the game's name from: out of the Control NCA's RomFS,
   unpacked into the output for a moment. A game without one, or one that cannot be read, keeps the name of its file, a
   line saying why */
static void control_nacp(ExtractState* state, const NcaRef* control) {
    char *dir = snx_path_join(state->out, "control.romfs"), *from = dir ? snx_path_join(dir, "control.nacp") : NULL;
    char* to = snx_path_join(state->out, "control.nacp");
    const char* why = NULL;
    if (!control->found)
        why = "it has no Control NCA";
    else if (!dir || !from || !to)
        why = "out of memory";
    else if (snx_hac_extract(state->hac, control->container, control->entry, -1, 0, NULL, dir))
        why = "its Control NCA cannot be read";
    else if (!snx_is_file(from))
        why = "its Control NCA holds no control.nacp";
    else if (snx_replace(from, to))
        why = "its control.nacp could not be moved beside exefs/";
    if (dir && snx_is_dir(dir) && snx_remove_tree(dir)) snx_error("Could not remove %s", dir);
    if (why) printf("The game keeps the name of its file: %s\n", why);
    fflush(stdout);
    free(dir);
    free(from);
    free(to);
}

static int open_container(ExtractState* state, const char* path, int xci) {
    int container = snx_hac_open(state->hac, path, xci);
    if (container < 0) hac_error(xci ? "Cannot read the XCI's secure partition" : "Cannot read the NSP");
    return container;
}

static int extract_files(ExtractState* state) {
    NcaRef program, patch, control, newer;
    char* main_path;
    int base, update, failed;
    if (state->has_keys && load_prod_keys(state)) return 1;
    if (load_title_keys(state)) return 1;
    printf("Reading %s\n", state->base);
    fflush(stdout);
    base = open_container(state, state->base, state->base_xci);
    if (base < 0 || read_tickets(state, base)) return 1;
    update = -1;
    if (state->update) {
        printf("Reading %s\n", state->update);
        fflush(stdout);
        update = open_container(state, state->update, state->update_xci);
        if (update < 0 || read_tickets(state, update)) return 1;
    }
    if (select_ncas(state, base, 0, 0, &program, update < 0 ? &patch : NULL, &control)) return 1;
    if (update >= 0) {
        if (select_ncas(state, update, 1, program.title_id, &patch, NULL, &newer)) return 1;
        if (newer.found) control = newer; /* the update's, which a name changed in it comes with */
    }
    /* the base's program NCA: its RomFS is most of it, and an update's RomFS is about as big */
    state->total = program.size;
    state->said_at = snx_seconds(); /* a quick one says nothing */
    snx_hac_progress(progress_tick, state);
    puts(patch.found ? "Extracting the base with the update applied" : "Extracting the base game");
    fflush(stdout);
    failed = patch.found ? snx_hac_extract(state->hac, patch.container, patch.entry, program.container, program.entry,
                                           state->exefs, state->romfs)
                         : snx_hac_extract(state->hac, program.container, program.entry, -1, 0, state->exefs,
                                           state->romfs);
    snx_hac_progress(NULL, NULL);
    if (state->said) progress_say(state, 1);
    if (failed) return hac_error("The Program NCA cannot be extracted");
    main_path = snx_path_join(state->exefs, "main");
    if (!main_path) return snx_error("Out of memory checking the extraction");
    failed = !snx_is_file(main_path);
    free(main_path);
    if (failed) return hac_error("Extracted exefs has no main; verify that the update matches the base game");
    control_nacp(state, &control);
    return 0;
}

static int cleanup(ExtractState* state, int result) {
    if (result && state->made_out && state->out && snx_remove_tree(state->out))
        snx_error("Could not remove incomplete extraction output: %s", state->out);
    if (!result && state->out) printf("Done: %s\n", state->out);
    snx_hac_free(state->hac);
    free(state->base);
    free(state->update);
    free(state->out);
    free(state->keys);
    free(state->title_keys);
    free(state->exefs);
    free(state->romfs);
    memset(state, 0, sizeof *state);
    return result;
}

/* hactool exits on a dump it cannot read: what it said, and the incomplete output removed first */
static void hactool_exit(void* data) {
    ExtractState* state = (ExtractState*)data;
    if (state->said) puts("");
    hac_error("hactool stopped on the dump");
    cleanup(state, 1);
}

int snx_extract(int argc, char** argv, const SnxPaths* paths) {
    ExtractOptions options;
    ExtractState state;
    int result;
    result = parse_options(argc, argv, &options);
    if (result) return result == 2 ? 0 : 1;
    if (!paths || !paths->repo) return snx_error("Extraction requires the repository path");
    memset(&state, 0, sizeof state);
    result = prepare_paths(&state, &options, paths);
    if (!result) {
        state.hac = snx_hac_new();
        if (!state.hac) result = snx_error("Out of memory preparing extraction");
    }
    if (!result) {
        snx_hac_on_exit(hactool_exit, &state);
        result = extract_files(&state);
        snx_hac_on_exit(NULL, NULL);
    }
    return cleanup(&state, result);
}
int snx_extract_title(const char* dump, const SnxPaths* paths, SnxU64* title) {
    ExtractState state;
    NcaRef program, control;
    int base, result = 0;
    memset(&state, 0, sizeof state);
    state.base = snx_absolute(dump);
    state.keys = find_keys(NULL, "prod.keys", paths);
    state.title_keys = find_keys(NULL, "title.keys", paths);
    if (!state.base || !state.keys || !state.title_keys)
        result = snx_error("Cannot resolve extraction input paths");
    else if (!snx_is_file(state.base))
        result = snx_error("Base dump is missing or is not a file");
    else
        result = container_kind(state.base, &state.base_xci);
    if (!result && !(state.hac = snx_hac_new())) result = snx_error("Out of memory reading the dump");
    if (!result) {
        state.has_keys = snx_is_file(state.keys);
        snx_hac_on_exit(hactool_exit, &state);
        result = (state.has_keys && load_prod_keys(&state)) || load_title_keys(&state) ||
                 (base = open_container(&state, state.base, state.base_xci)) < 0 || read_tickets(&state, base) ||
                 select_ncas(&state, base, 0, 0, &program, NULL, &control);
        snx_hac_on_exit(NULL, NULL);
        if (!result) *title = program.title_id;
    }
    return cleanup(&state, result);
}
