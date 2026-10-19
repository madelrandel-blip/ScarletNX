/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --extract (src/nx/runtime/driver/extract.c) with hactool in its process, on synthetic decrypted dumps:
   NSPs and gamecards of DNCAs (tests/hac_synth.h), read in place. Each scenario runs in a child, whose output is
   checked: what it says, that no key reaches it, and that a failure, an exit of hactool's included, leaves nothing.
   No dumps or console keys */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "nx/runtime/driver/extract.h"
#if defined(_WIN32) && defined(__clang__)
/* hactool's utils.h redeclares the MinGW CRT's imported fseeko64. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-dllimport"
#endif
#include "nx/runtime/driver/hactool/hfs0.h"
#include "nx/runtime/driver/hactool/nca.h"
#include "nx/runtime/driver/hactool/pfs0.h"
#include "nx/runtime/driver/hactool/sha.h"
#include "nx/runtime/driver/hactool/xci.h"
#if defined(_WIN32) && defined(__clang__)
#pragma clang diagnostic pop
#endif

static unsigned checks;

static void check(int good, const char* expression, int line) {
    checks++;
    if (!good) {
        fprintf(stderr, "extract.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

#include "hac_synth.h"

#define TITLE 0x0100000000AB0000ULL
#define SECRET "0123456789abcdef0123456789abcdef"

/* synthetic, not console keys, with lines extraction leaves out: a value of an odd length, and one no key has */
static const char prod_keys[] = "# synthetic\n"
                                "header_key = " SECRET SECRET "\n"
                                "key_area_key_application_00 = " SECRET "\n"
                                "not_a_key = " SECRET "\n"
                                "titlekek_00 = abc\n";
static const char user_titles[] = "A0A0A0A0A0A0A0A0A0A0A0A0A0A0A0A0 = " SECRET "\n";

static void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    CHECK(!_putenv_s(name, value));
#else
    CHECK(!setenv(name, value, 1));
#endif
}

static blob_t text_blob(const char* text) {
    blob_t b = {zalloc(strlen(text)), strlen(text)};
    memcpy(b.data, text, b.size);
    return b;
}

/* the game's NCAs, as named in a container, and a ticket */
typedef struct {
    romfs_file_t files[6];
    unsigned count;
} Game;

static void game_add(Game* game, const char* name, blob_t content) {
    CHECK(game->count < 6);
    game->files[game->count].name = name;
    game->files[game->count++].content = content;
}

static blob_t npdm(void) {
    blob_t b = {zalloc(0x80), 0x80};
    memcpy(b.data, "META", 4);
    return b;
}

static blob_t nacp(void) {
    blob_t b = {zalloc(0x4000), 0x4000};
    strcpy((char*)b.data, "Synthetic Game");
    return b;
}

/* a common ticket of the version that stores its title key as is */
static blob_t ticket(void) {
    blob_t b = {zalloc(0x400), 0x400};
    put32(b.data, 0x10004);
    memset(b.data + 4 + 0x100 + 0x3c + 0x40, 0x5A, 16);
    memset(b.data + 4 + 0x100 + 0x3c + 0x160, 0xA5, 16);
    return b;
}

static blob_t program(uint64_t title, int with_main) {
    blob_t code = text_blob("synthetic main"), npdm_file = npdm(), data = text_blob("synthetic romfs data");
    romfs_file_t exefs[2] = {{"main.npdm", npdm_file}, {"main", code}};
    romfs_file_t romfs[1] = {{"data.bin", data}};
    blob_t nca = dnca_build(NCACONTENTTYPE_PROGRAM, title, exefs, with_main ? 2 : 1, romfs, 1);
    free(code.data);
    free(npdm_file.data);
    free(data.data);
    return nca;
}

static blob_t control(uint64_t title) {
    blob_t file = nacp();
    romfs_file_t romfs[1] = {{"control.nacp", file}};
    blob_t nca = dnca_build(NCACONTENTTYPE_CONTROL, title, NULL, 0, romfs, 1);
    free(file.data);
    return nca;
}

/* an NCA whose header is encrypted, which the keys would have to open */
static blob_t encrypted_nca(void) {
    blob_t b = {zalloc(0xC00), 0xC00};
    memcpy(b.data + 0x200, "XXXX", 4);
    return b;
}

static void write_blob(const char* path, blob_t blob) {
    CHECK(!snx_write_file(path, blob.data, blob.size));
    free(blob.data);
}

static void write_container(const char* path, Game* game, const char* kind) {
    if (!strcmp(kind, "nsp"))
        write_blob(path, partition_build(MAGIC_PFS0, game->files, game->count, 0x203));
    else
        write_blob(path, gamecard_build(game->files, game->count, !strcmp(kind, "xci-keyarea")));
    for (unsigned i = 0; i < game->count; i++) free(game->files[i].content.data);
    game->count = 0;
}

static int file_is(const char* dir, const char* name, const void* content, size_t size) {
    char* path = snx_path_join(dir, name);
    size_t got_size = 0;
    unsigned char* got = path ? snx_read_file(path, &got_size) : NULL;
    int same = got && got_size == size && !memcmp(got, content, size);
    free(got);
    free(path);
    return same;
}

/* one scenario in this process, as scarletnx -x runs: its result */
static int scenario(const char* scratch, const char* mode) {
    SnxPaths paths;
    char *repo, *home, *switch_dir, *keys, *base, *update, *out, *exefs, *arguments[12];
    int count = 0, result;
    Game game = {{{0}}, 0};
    const char* kind = !strncmp(mode, "xci", 3) ? (strstr(mode, "keyarea") ? "xci-keyarea" : "xci") : "nsp";
    repo = snx_path_join(scratch, "repo space & ()");
    home = snx_path_join(scratch, "original home");
    switch_dir = snx_path_join(home, ".switch");
    CHECK(!snx_mkdirs(repo) && !snx_mkdirs(switch_dir));
    memset(&paths, 0, sizeof paths);
    paths.repo = repo;
    set_env("HOME", home);
    set_env("USERPROFILE", home);
    /* the keys from the data folder, from ~/.switch, or named */
    keys = snx_path_join(!strcmp(mode, "homekeys") || !strncmp(mode, "explicit", 8) ? switch_dir : repo, "prod.keys");
    if (strcmp(mode, "encrypted-no-keys") && strcmp(mode, "explicit-missing-keys"))
        CHECK(!snx_write_file(keys, prod_keys, strlen(prod_keys)));
    {
        char* titles = snx_path_join(repo, "title.keys");
        CHECK(!snx_write_file(titles, user_titles, strlen(user_titles)));
        free(titles);
    }
    base = snx_path_join(scratch, !strcmp(mode, "renamed") ? "Base Game (&).XCI" : "Base Game (&).NSP");
    update = snx_path_join(scratch, "Update (&).nsp");
    out = snx_path_join(scratch, "out put");
    exefs = snx_path_join(out, "exefs");
    if (!strcmp(mode, "unsupported"))
        CHECK(!snx_write_file(base, "not a container at all", 22));
    else {
        game_add(&game, "1b2c.cnmt.nca", text_blob("a content meta NCA, never read"));
        game_add(&game, "a0a0.tik", ticket());
        if (!strcmp(mode, "encrypted-no-keys"))
            game_add(&game, "enc.nca", encrypted_nca());
        else if (!strcmp(mode, "dlc"))
            game_add(&game, "dlc.nca", program(TITLE | 0x800, 1)); /* an update's alone */
        else
            game_add(&game, "program.nca", program(TITLE, strcmp(mode, "no-main")));
        if (strcmp(mode, "no-control")) game_add(&game, "control.nca", control(TITLE));
        write_container(base, &game, kind);
    }
    if (!strcmp(mode, "hactool-exit")) { /* a gamecard whose root partition is not what its header hashed */
        blob_t card = gamecard_build(NULL, 0, 0);
        card.data[0xF000 + 0xC] ^= 1;
        write_blob(base, card);
    }
    arguments[count++] = base;
    if (!strcmp(mode, "unmatched-update")) {
        game_add(&game, "patch.nca", program((TITLE + 0x1000) | 0x800, 1));
        write_container(update, &game, "nsp");
        arguments[count++] = "--update";
        arguments[count++] = update;
    }
    if (!strncmp(mode, "explicit", 8)) {
        arguments[count++] = "--keys";
        arguments[count++] = keys;
    }
    if (!strcmp(mode, "named")) {
        arguments[count++] = "--name";
        arguments[count++] = "Named";
        free(out);
        out = snx_path_join(repo, "work/Named");
        free(exefs);
        exefs = snx_path_join(out, "exefs");
    } else {
        arguments[count++] = "--out";
        arguments[count++] = out;
    }
    if (!strcmp(mode, "existing")) CHECK(!snx_mkdirs(exefs));
    result = snx_extract(count, arguments, &paths);
    if (!result) {
        char* romfs = snx_path_join(out, "romfs");
        char* control_romfs = snx_path_join(out, "control.romfs");
        blob_t want = nacp(), meta = npdm();
        CHECK(file_is(exefs, "main", "synthetic main", 14) && file_is(exefs, "main.npdm", meta.data, meta.size));
        free(meta.data);
        CHECK(file_is(romfs, "data.bin", "synthetic romfs data", 20));
        if (strcmp(mode, "no-control"))
            CHECK(file_is(out, "control.nacp", want.data, want.size));
        else
            CHECK(!file_is(out, "control.nacp", want.data, want.size));
        CHECK(!snx_path_exists(control_romfs)); /* where control.nacp came out, gone */
        free(want.data);
        free(romfs);
        free(control_romfs);
    } else if (!strcmp(mode, "existing"))
        CHECK(snx_is_dir(exefs)); /* the output was there before: left as it was */
    else
        CHECK(!snx_path_exists(out)); /* an incomplete extraction is removed */
    free(repo);
    free(home);
    free(switch_dir);
    free(keys);
    free(base);
    free(update);
    free(out);
    free(exefs);
    return result;
}

int main(int argc, char** argv) {
    /* name, whether it extracts, and what it must say */
    static const struct {
        const char *mode, *says;
        int fails;
    } scenarios[] = {
        {"nsp", "Left out 1 malformed key lines", 0},
        {"xci", "Done: ", 0},
        {"xci-keyarea", "Done: ", 0},
        {"renamed", "Done: ", 0}, /* an NSP named .XCI: the header decides */
        {"homekeys", "Left out 1 malformed key lines", 0},
        {"explicit", "Left out 1 malformed key lines", 0},
        {"named", "Done: ", 0},
        {"no-control", "The game keeps the name of its file: it has no Control NCA", 0},
        {"unsupported", "Unsupported container header", 1},
        {"encrypted-no-keys", "Encrypted content needs prod.keys", 1},
        {"explicit-missing-keys", "Specified prod.keys file is missing", 1},
        {"unmatched-update", "Update has no Program NCA matching the selected base game", 1},
        {"dlc", "Container has no base Program NCA", 1},
        {"no-main", "Extracted exefs has no main", 1},
        {"existing", "Output already contains exefs", 1},
        {"hactool-exit", "hactool stopped on the dump", 1},
    };
    const char* command[5];
    char *self, *scratch, *directory, *output;
    size_t i;
    int result;
    if (argc == 4 && !strcmp(argv[1], "--scenario")) return scenario(argv[2], argv[3]) ? 1 : 0;
    CHECK(argc == 1);
    self = snx_executable(argv[0]);
    scratch = snx_temp_dir(NULL);
    CHECK(self != NULL && scratch != NULL);
    for (i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        directory = snx_path_join(scratch, scenarios[i].mode);
        CHECK(!snx_mkdirs(directory));
        command[0] = self;
        command[1] = "--scenario";
        command[2] = directory;
        command[3] = scenarios[i].mode;
        command[4] = NULL;
        output = NULL;
        result = snx_run(command, NULL, NULL, &output);
        CHECK(output != NULL);
        if ((result != 0) != scenarios[i].fails || !strstr(output, scenarios[i].says))
            fprintf(stderr, "scenario %s exited %d:\n%s", scenarios[i].mode, result, output);
        CHECK((result != 0) == scenarios[i].fails);
        CHECK(strstr(output, scenarios[i].says) != NULL);
        CHECK(strstr(output, SECRET) == NULL); /* no key, whatever hactool said */
        CHECK(strstr(output, "extracted ") == NULL); /* a quick one says nothing */
        printf("PASS extraction scenario %s\n", scenarios[i].mode);
        free(output);
        free(directory);
    }
    CHECK(!snx_remove_tree(scratch));
    free(scratch);
    free(self);
    printf("PASS %u extraction driver checks; synthetic dumps, no console keys\n", checks);
    return 0;
}
