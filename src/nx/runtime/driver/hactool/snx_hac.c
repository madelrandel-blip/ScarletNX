/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_hac.h over hactool's own contexts, as its main.c drives them for -txci, -tpfs0, -i and --basenca, but with every
   NCA read where it lies in its container (nca_ctx_t.base_offset) instead of from a copy unpacked beside it */
#include "snx_hac.h"
#include <stdarg.h>
#include <string.h>
#include "extkeys.h"
#include "filepath.h"
#include "hfs0.h"
#include "nca.h"
#include "pfs0.h"
#include "pki.h"
#include "xci.h"

/* the C library's own here, under the names snx_io.h (included ahead of this file too) gives hactool */
#undef printf
#undef fprintf
#undef fwrite
#undef exit

#define SNX_HAC_CONTAINERS 2

typedef struct {
    FILE *file;
    xci_ctx_t *xci;   /* an XCI's contexts, NULL for an NSP */
    pfs0_ctx_t *pfs0; /* an NSP's */
    SnxHacEntry *entries;
    size_t count;
} Container;

struct SnxHac {
    hactool_ctx_t tool;
    Container containers[SNX_HAC_CONTAINERS];
    int open;
};

/* ------------------------------------------------------------------ hactool's output, exits and writes */

static struct {
    char message[256]; /* the last line on stderr */
    void (*tick)(uint64_t, void *);
    void *tick_data;
    uint64_t written;
    void (*cleanup)(void *);
    void *cleanup_data;
} io;

static void io_say(FILE *file, const char *format, va_list ap) {
    char line[sizeof io.message];
    size_t n;
    if (file != stderr) return; /* stdout is hactool's -i report, keys among it */
    vsnprintf(line, sizeof line, format, ap);
    n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) memcpy(io.message, line, n + 1);
}

int snx_hac_printf(const char *format, ...) {
    (void)format;
    return 0;
}

int snx_hac_fprintf(FILE *file, const char *format, ...) {
    va_list ap;
    int n = 0;
    va_start(ap, format);
    if (file == stdout || file == stderr)
        io_say(file, format, ap);
    else
        n = vfprintf(file, format, ap);
    va_end(ap);
    return n;
}

size_t snx_hac_fwrite(const void *data, size_t size, size_t count, FILE *file) {
    size_t n;
    if (file == stdout || file == stderr) return count;
    n = fwrite(data, size, count, file);
    io.written += (uint64_t)n * size;
    if (io.tick) io.tick(io.written, io.tick_data);
    return n;
}

_Noreturn void snx_hac_exit(int status) {
    void (*cleanup)(void *) = io.cleanup;
    io.cleanup = NULL; /* once, should the cleanup itself end in here */
    if (cleanup) cleanup(io.cleanup_data);
    exit(status ? status : EXIT_FAILURE);
}

void snx_hac_progress(void (*tick)(uint64_t written, void *data), void *data) {
    io.tick = tick;
    io.tick_data = data;
    io.written = 0;
}

void snx_hac_on_exit(void (*cleanup)(void *data), void *data) {
    io.cleanup = cleanup;
    io.cleanup_data = data;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

const char *snx_hac_message(void) {
    static char said[sizeof io.message];
    size_t i = 0, run, out = 0;
    while (io.message[i]) {
        for (run = 0; is_hex(io.message[i + run]); run++) {}
        if (run >= 16) { /* "..." is no longer than what it stands for */
            memcpy(said + out, "...", 3);
            out += 3;
        } else {
            if (!run) run = 1;
            memcpy(said + out, io.message + i, run);
            out += run;
        }
        i += run;
    }
    said[out] = 0;
    return said;
}

/* ------------------------------------------------------------------ keys */

SnxHac *snx_hac_new(void) {
    SnxHac *hac = calloc(1, sizeof *hac);
    if (!hac) return NULL;
    pki_initialize_keyset(&hac->tool.settings.keyset, KEYSET_RETAIL);
    hac->tool.settings.skip_key_warnings = 1;
    return hac;
}

int snx_hac_set_key(SnxHac *hac, const char *name, const char *value) {
    return extkeys_set_key(&hac->tool.settings, name, value);
}

void snx_hac_derive_keys(SnxHac *hac) {
    hactool_settings_t *settings = &hac->tool.settings;
    if (settings->has_sdseed)
        for (unsigned int key = 0; key < 2; key++)
            for (unsigned int i = 0; i < 0x20; i++) settings->keyset.sd_card_key_sources[key][i] ^= settings->sdseed[i & 0xF];
    pki_derive_keys(&settings->keyset);
}

void snx_hac_add_title_key(SnxHac *hac, const unsigned char rights_id[16], const unsigned char key[16]) {
    if (!settings_has_titlekey(&hac->tool.settings, rights_id))
        settings_add_titlekey(&hac->tool.settings, rights_id, key);
}

/* ------------------------------------------------------------------ containers */

static void container_close(Container *c) {
    if (c->xci) {
        hfs0_ctx_t *parts[5] = {&c->xci->partition_ctx, &c->xci->normal_ctx, &c->xci->update_ctx, &c->xci->secure_ctx,
                                &c->xci->logo_ctx};
        for (unsigned int i = 0; i < 5; i++) free(parts[i]->header);
        free(c->xci);
    }
    if (c->pfs0) {
        free(c->pfs0->header);
        free(c->pfs0->npdm);
        free(c->pfs0);
    }
    free(c->entries);
    if (c->file) fclose(c->file);
    memset(c, 0, sizeof *c);
}

void snx_hac_free(SnxHac *hac) {
    if (!hac) return;
    for (int i = 0; i < hac->open; i++) container_close(&hac->containers[i]);
    free(hac->tool.settings.known_titlekeys.titlekeys);
    memset(hac, 0, sizeof *hac); /* the keys */
    free(hac);
}

int snx_hac_open(SnxHac *hac, const char *path, int xci) {
    Container *c;
    hactool_ctx_t parse;
    if (hac->open == SNX_HAC_CONTAINERS) return -1;
    c = &hac->containers[hac->open];
    filepath_t name;
    filepath_init(&name);
    filepath_set(&name, path);
    if (name.valid != VALIDITY_VALID || !(c->file = os_fopen(name.os_path, OS_MODE_READ))) return -1;
    parse = hac->tool;
    parse.action = 0; /* read the tables, print and save nothing */
    parse.file = c->file;
    if (xci) {
        hfs0_header_t *h;
        uint64_t data;
        if (!(c->xci = calloc(1, sizeof *c->xci))) goto fail;
        c->xci->file = c->file;
        c->xci->tool_ctx = &parse;
        xci_process(c->xci);
        c->xci->tool_ctx = NULL;
        h = c->xci->secure_ctx.header;
        if (!h) goto fail; /* no secure partition: nothing of a game in it */
        data = c->xci->secure_ctx.offset + hfs0_get_header_size(h);
        if (!(c->entries = calloc(h->num_files ? h->num_files : 1, sizeof *c->entries))) goto fail;
        for (uint32_t i = 0; i < h->num_files; i++) {
            hfs0_file_entry_t *e = hfs0_get_file_entry(h, i);
            c->entries[i].name = hfs0_get_file_name(h, i);
            c->entries[i].offset = data + e->offset;
            c->entries[i].size = e->size;
        }
        c->count = h->num_files;
    } else {
        pfs0_header_t *h;
        uint64_t data;
        if (!(c->pfs0 = calloc(1, sizeof *c->pfs0))) goto fail;
        c->pfs0->file = c->file;
        c->pfs0->tool_ctx = &parse;
        pfs0_process(c->pfs0);
        c->pfs0->tool_ctx = NULL;
        h = c->pfs0->header;
        data = pfs0_get_header_size(h);
        if (!(c->entries = calloc(h->num_files ? h->num_files : 1, sizeof *c->entries))) goto fail;
        for (uint32_t i = 0; i < h->num_files; i++) {
            pfs0_file_entry_t *e = pfs0_get_file_entry(h, i);
            c->entries[i].name = pfs0_get_file_name(h, i);
            c->entries[i].offset = data + e->offset;
            c->entries[i].size = e->size;
        }
        c->count = h->num_files;
    }
    return hac->open++;
fail:
    container_close(c);
    return -1;
}

size_t snx_hac_entries(SnxHac *hac, int container, const SnxHacEntry **entries) {
    *entries = hac->containers[container].entries;
    return hac->containers[container].count;
}

int snx_hac_read(SnxHac *hac, int container, size_t entry, void *buffer, size_t size) {
    Container *c = &hac->containers[container];
    if (size > c->entries[entry].size || fseeko64(c->file, c->entries[entry].offset, SEEK_SET) ||
        fread(buffer, 1, size, c->file) != size)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ NCAs */

static void nca_open(SnxHac *hac, hactool_ctx_t *tool, nca_ctx_t *nca, int container, size_t entry) {
    Container *c = &hac->containers[container];
    nca_init(nca);
    nca->tool_ctx = tool;
    nca->file = c->file;
    nca->base_offset = c->entries[entry].offset;
    nca->file_size = (size_t)c->entries[entry].size;
}

int snx_hac_nca(SnxHac *hac, int container, size_t entry, SnxHacNca *out) {
    nca_ctx_t nca;
    memset(out, 0, sizeof *out);
    nca_open(hac, &hac->tool, &nca, container, entry);
    if (!nca_decrypt_header(&nca)) return -1;
    out->type = nca.header.content_type == NCACONTENTTYPE_PROGRAM   ? SNX_HAC_PROGRAM
                : nca.header.content_type == NCACONTENTTYPE_CONTROL ? SNX_HAC_CONTROL
                                                                    : SNX_HAC_OTHER;
    out->title_id = nca.header.title_id;
    /* a Patch RomFS, as nca_process tells one: BKTR encryption, or a decrypted dump's kept indirect tables */
    for (unsigned int i = 0; i < 4; i++) {
        nca_fs_header_t *fs = &nca.header.fs_headers[i];
        if (!nca.header.section_entries[i].media_start_offset || fs->partition_type != PARTITION_ROMFS ||
            fs->fs_type != FS_TYPE_ROMFS)
            continue;
        if (fs->crypt_type == CRYPT_BKTR ||
            (nca.is_decrypted && fs->bktr_superblock.relocation_header.magic == MAGIC_BKTR &&
             fs->bktr_superblock.subsection_header.magic == MAGIC_BKTR))
            out->patch = 1;
    }
    return 0;
}

static int has_romfs(const nca_ctx_t *nca) {
    for (unsigned int i = 0; i < 4; i++)
        if (nca->section_contexts[i].is_present && nca->section_contexts[i].type == ROMFS) return 1;
    return 0;
}

int snx_hac_extract(SnxHac *hac, int container, size_t entry, int base_container, size_t base_entry,
                    const char *exefs_dir, const char *romfs_dir) {
    hactool_ctx_t *tool, *base_tool = NULL;
    nca_ctx_t *nca, *base = NULL;
    int rc = -1;
    tool = calloc(1, sizeof *tool);
    nca = calloc(1, sizeof *nca);
    if (!tool || !nca) goto done;
    tool->settings = hac->tool.settings;
    tool->action = ACTION_EXTRACT;
    if (exefs_dir) {
        tool->settings.exefs_dir_path.enabled = 1;
        filepath_set(&tool->settings.exefs_dir_path.path, exefs_dir);
    }
    if (romfs_dir) {
        tool->settings.romfs_dir_path.enabled = 1;
        filepath_set(&tool->settings.romfs_dir_path.path, romfs_dir);
    }
    if (base_container >= 0) { /* --basenca: the base's RomFS under the update's */
        base_tool = calloc(1, sizeof *base_tool);
        base = calloc(1, sizeof *base);
        if (!base_tool || !base) goto done;
        base_tool->settings.keyset = hac->tool.settings.keyset;
        base_tool->settings.known_titlekeys = hac->tool.settings.known_titlekeys;
        nca_open(hac, base_tool, base, base_container, base_entry);
        base->is_cli_target = false;
        if (!nca_decrypt_header(base)) goto done;
        tool->base_file = base->file;
        tool->base_file_type = BASEFILE_NCA;
        tool->base_nca_ctx = base;
        nca_process(base);
        if (!has_romfs(base)) {
            snx_hac_fprintf(stderr, "Unable to locate RomFS in base NCA!\n");
            goto done;
        }
    }
    nca_open(hac, tool, nca, container, entry);
    nca->is_cli_target = true;
    if (!nca_decrypt_header(nca)) goto done;
    nca_process(nca);
    rc = 0;
done:
    if (nca) nca_free_section_contexts(nca); /* the update's first: its storages read through the base's */
    if (base) nca_free_section_contexts(base);
    free(base);
    free(base_tool);
    free(nca);
    free(tool);
    return rc;
}
