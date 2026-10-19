/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The file of translated shaders and pipelines made (vk.h's pipeline_file.c). Records are appended as they are made
   and flushed, so a run that crashes keeps what it made, and read once at startup. It calls no Vulkan function:
   pipeline.c makes the modules and pipelines from what it hands back */
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pipeline_file.h"

#define PF_MAGIC   "SNXVKSH\n"
#define PF_VERSION 1
#define PF_REC     16 /* a record's header: type, payload bytes, check */

/* the check of a payload: words of it after its type and size (FNV-1a over 32-bit words, with a last mix) */
static u64 pf_mix(u64 h, const void* p, size_t n) {
    const u8* b = (const u8*)p;
    size_t i;
    for (i = 0; i + 4 <= n; i += 4) {
        u32 w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}
static u64 pf_start(u32 type, u32 size) {
    u64 h = 0xCBF29CE484222325ull;
    h = (h ^ type) * 0x100000001B3ull;
    return (h ^ size) * 0x100000001B3ull;
}
static u64 pf_end(u64 h) {
    h ^= h >> 31;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}
static u32 pf_get32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static u64 pf_get64(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

/* the module's slot in the map: its own, or the free one where it would go */
static PfMod* pf_slot(PfMod* map, u32 cap, u64 module) {
    u32 i = (u32)((module * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
    while (map[i].module && map[i].module != module) i = (i + 1) & (cap - 1);
    return &map[i];
}
static void pf_map_add(PFile* pf, u64 module, u32 rec, u64 check) {
    PfMod* m;
    if (!module) return;
    if ((pf->nmap + 1) * 2 > pf->cap) { /* at most half full */
        u32 cap = pf->cap ? pf->cap * 2 : 1024, i;
        PfMod* grown = (PfMod*)calloc(cap, sizeof *grown);
        if (!grown) return; /* pipelines with this module are not recorded */
        for (i = 0; i < pf->cap; i++)
            if (pf->map[i].module) *pf_slot(grown, cap, pf->map[i].module) = pf->map[i];
        free(pf->map);
        pf->map = grown;
        pf->cap = cap;
    }
    m = pf_slot(pf->map, pf->cap, module);
    if (!m->module) pf->nmap++;
    m->module = module;
    m->check = check;
    m->rec = rec;
}

/* the header for key, malloc'd, its size a multiple of 8 */
static u8* pf_header(const char* key, size_t* size) {
    size_t n = strlen(key), total = (16 + n + 7) & ~(size_t)7;
    u8* h = (u8*)calloc(1, total);
    u32 v = PF_VERSION, len = (u32)n;
    if (!h) return NULL;
    memcpy(h, PF_MAGIC, 8);
    memcpy(h + 8, &v, 4);
    memcpy(h + 12, &len, 4);
    memcpy(h + 16, key, n);
    *size = total;
    return h;
}

/* appends a record of pieces (each a multiple of 4 bytes), padded to 8: 0, or -1 when it can't be (the file is full or
   a write failed; nothing more is appended then). Called with pf->lock */
static int pf_append(PFile* pf, u32 type, const void* const* piece, const size_t* len, int pieces, u64* check) {
    static const u8 zero[8] = {0};
    size_t payload = 0, pad;
    u8 head[PF_REC];
    u64 h;
    u32 size;
    int i, ok;
    if (!pf->f) return -1;
    for (i = 0; i < pieces; i++) payload += len[i];
    pad = (8 - payload % 8) % 8;
    if (payload + pad > PF_MAX || pf->size + PF_REC + payload + pad > PF_MAX) {
        LOG("vulkan: the shader and pipeline file reached %u MiB: what is made from now on is not kept for the next "
            "run\n",
            PF_MAX >> 20);
        fclose(pf->f);
        pf->f = NULL;
        return -1;
    }
    size = (u32)(payload + pad);
    h = pf_start(type, size);
    for (i = 0; i < pieces; i++) h = pf_mix(h, piece[i], len[i]);
    h = pf_end(pf_mix(h, zero, pad));
    memcpy(head, &type, 4);
    memcpy(head + 4, &size, 4);
    memcpy(head + 8, &h, 8);
    ok = fwrite(head, 1, PF_REC, pf->f) == PF_REC;
    for (i = 0; i < pieces && ok; i++) ok = fwrite(piece[i], 1, len[i], pf->f) == len[i];
    if (ok && pad) ok = fwrite(zero, 1, pad, pf->f) == pad;
    if (ok) ok = fflush(pf->f) == 0;
    if (!ok) { /* a record cut short ends what the next run reads, which is all it can tell */
        WARN("vulkan: cannot write the shader and pipeline file: what is made from now on is not kept\n");
        fclose(pf->f);
        pf->f = NULL;
        return -1;
    }
    pf->size += PF_REC + size;
    *check = h;
    return 0;
}

void pf_shader(PFile* pf, u64 module, const PfShader* s, const u32* cbr, const u32* spv) {
    const void* piece[3];
    size_t len[3];
    u64 check;
    piece[0] = s;
    len[0] = sizeof *s;
    piece[1] = cbr;
    len[1] = (size_t)s->ncbr * 16;
    piece[2] = spv;
    len[2] = (size_t)s->nspv * 4;
    pm_lock(&pf->lock);
    if (!pf_append(pf, PF_SHADER, piece, len, 3, &check)) {
        pf->nsh++;
        pf_map_add(pf, module, pf->nsh, check);
    }
    pm_unlock(&pf->lock);
}

void pf_pipe(PFile* pf, const u64 modules[5], const void* key, u32 keylen) {
    const void* piece[2];
    size_t len[2];
    PfPipe p;
    u64 check;
    int i;
    memset(&p, 0, sizeof p);
    p.keylen = keylen;
    piece[0] = &p;
    len[0] = sizeof p;
    piece[1] = key;
    len[1] = keylen;
    pm_lock(&pf->lock);
    for (i = 0; i < 5; i++) {
        const PfMod* m;
        if (!modules[i]) continue;
        m = pf->cap ? pf_slot(pf->map, pf->cap, modules[i]) : NULL;
        if (!m || !m->module) { /* not a shader of the file (a clear's), or its record failed */
            pm_unlock(&pf->lock);
            return;
        }
        p.sh[i] = m->rec;
        p.check[i] = m->check;
    }
    pf_append(pf, PF_PIPE, piece, len, 2, &check);
    pm_unlock(&pf->lock);
}

/* the records in buf[at, size) in order, to load: how far they go while they check out */
static size_t pf_read(PFile* pf, const u8* buf, size_t at, size_t size, const PfLoad* load, u32 loaded[2]) {
    /* by shader record number (from 1): its check and module, for the pipeline records after it */
    u64 *check = NULL, *module = NULL;
    u32 cap = 0, n = 0;
    while (size - at >= PF_REC) {
        u32 type = pf_get32(buf + at), len = pf_get32(buf + at + 4);
        const u8* p = buf + at + PF_REC;
        u64 h;
        if (len % 8 || len > size - at - PF_REC) break;
        h = pf_end(pf_mix(pf_start(type, len), p, len));
        if (h != pf_get64(buf + at + 8)) break;
        if (type == PF_SHADER) {
            PfShader s;
            u64 need, made;
            if (len < sizeof s) break;
            memcpy(&s, p, sizeof s);
            need = sizeof s + (u64)s.ncbr * 16 + (u64)s.nspv * 4;
            if (need > len || len - need >= 8 || s.stage > ST_FS || s.nspv < 5) break;
            if (n + 1 >= cap) {
                u32 c = cap ? cap * 2 : 1024;
                u64* a = (u64*)realloc(check, c * sizeof *a);
                if (!a) break;
                check = a;
                a = (u64*)realloc(module, c * sizeof *a);
                if (!a) break;
                module = a;
                cap = c;
            }
            /* the reads and the words are 8-byte aligned in buf (malloc'd), past the fixed part's multiple of 8 */
            made = load->shader(load->ctx, &s, (const u32*)(const void*)(p + sizeof s),
                                (const u32*)(const void*)(p + sizeof s + (size_t)s.ncbr * 16));
            n++;
            check[n] = h;
            module[n] = made;
            if (made) {
                pf_map_add(pf, made, n, h);
                loaded[0]++;
            }
        } else if (type == PF_PIPE) {
            PfPipe q;
            u64 mods[5];
            int i, ok = 1;
            if (len < sizeof q) break;
            memcpy(&q, p, sizeof q);
            if (q.keylen > len - sizeof q || len - sizeof q - q.keylen >= 8) break;
            for (i = 0; i < 5; i++) {
                mods[i] = 0;
                if (!q.sh[i]) continue;
                /* a shader record before it, the one it names (by its check), that was made */
                if (q.sh[i] > n || check[q.sh[i]] != q.check[i] || !module[q.sh[i]])
                    ok = 0;
                else
                    mods[i] = module[q.sh[i]];
            }
            if (ok) {
                load->pipe(load->ctx, mods, p + sizeof q, q.keylen);
                loaded[1]++;
            }
        } else
            break;
        at += PF_REC + len;
    }
    pf->nsh = n;
    free(check);
    free(module);
    return at;
}

void pf_open(PFile* pf, const char* path, const char* key, const PfLoad* load, u32 loaded[2]) {
    size_t hsize = 0, size = 0, used = 0;
    u8 *head = pf_header(key, &hsize), *buf = NULL;
    FILE* f;
    long length = -1;
    loaded[0] = loaded[1] = 0;
    pm_lock(&pf->lock);
    if (!head) {
        pm_unlock(&pf->lock);
        return;
    }
    f = fopen(path, "rb");
    if (f && !fseek(f, 0, SEEK_END)) length = ftell(f);
    /* a file of another key, or of more than a full one and a record, is of no use */
    if (f && length >= (long)hsize && (unsigned long)length <= 2ul * PF_MAX && !fseek(f, 0, SEEK_SET)) {
        size = (size_t)length;
        buf = (u8*)malloc(size);
        if (buf && fread(buf, 1, size, f) != size) {
            free(buf);
            buf = NULL;
        }
    }
    if (f) fclose(f);
    if (buf && !memcmp(buf, head, hsize)) {
        used = pf_read(pf, buf, hsize, size, load, loaded);
        if (used < size)
            LOG("vulkan: %s: the records past %lu of its %lu bytes were cut short or changed, and are dropped\n", path,
                (unsigned long)used, (unsigned long)size);
    } else if (length > 0)
        LOG("vulkan: %s is of another build, GPU or driver, or unreadable: started over\n", path);
    if (used && used == size)
        pf->f = fopen(path, "ab");
    else { /* started over, or rewritten with the records that checked out (what a crash cut short goes) */
        int ok;
        pf->f = fopen(path, "wb");
        ok = pf->f && fwrite(used ? buf : head, 1, used ? used : hsize, pf->f) == (used ? used : hsize) &&
             !fflush(pf->f);
        if (!ok && pf->f) {
            fclose(pf->f);
            pf->f = NULL;
        }
        if (!used) {
            pf->nsh = 0;
            used = hsize;
        }
    }
    pf->size = used;
    if (!pf->f) WARN("vulkan: cannot write %s: shaders and pipelines made are not kept for the next run\n", path);
    free(buf);
    free(head);
    pm_unlock(&pf->lock);
}
