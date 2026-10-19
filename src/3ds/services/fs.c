/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Initial FS session wire shapes referenced against Azahar service/fs/fs_user.cpp
   and libctru services/fs.c. The extracted title directory is the NX-style
   shared tree: romfs/exefs read-only and always open, sdmc read-write. */
#include "3ds/runtime/internal.h"
#include "3ds/services/fs.h"
#include "services/fs.swipc.inc"

/* 3DS FS result codes: 0xc8804400 | the Azahar ErrCodes value. */
static uint32_t tree_result(FsTreeResult r) {
    switch (r) {
        case FS_TREE_OK: return 0;
        case FS_TREE_NOTFOUND: return 0xc8804478u;
        case FS_TREE_EXISTS: return 0xc88044beu;
        case FS_TREE_NOTEMPTY: return 0xc88044f0u;
        case FS_TREE_INVALID: return 0xc8804702u;
        case FS_TREE_TOOLONG: return 0xc88046beu;
        case FS_TREE_READONLY: return 0xd8e01beeu;
        default: return 0xc88044d2u;
    }
}
static bool initialized(SnxCtrIpc* c) {
    return c->process->objects[c->object].data.session.client_pid == CTR_PROCESS_ID;
}
static uint32_t snx_ctr_fs_wire_InitializeWithSdkVersion(SnxCtrIpc* c, uint32_t version, uint32_t process) {
    if (process != CTR_PROCESS_ID) return SNX_CTR_IPC_INVALID_STATE;
    CtrObject* object = &c->process->objects[c->object];
    object->data.session.client_pid = process;
    object->data.session.sdk_version = version;
    return 0;
}
static uint32_t snx_ctr_fs_wire_Initialize(SnxCtrIpc* c, uint32_t process) {
    return snx_ctr_fs_wire_InitializeWithSdkVersion(c, 0, process);
}
static CtrSelfArchive* archive(SnxCtrProcess* p, uint64_t token) {
    if (token)
        for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i)
            if (p->self_archives[i].token == token) return &p->self_archives[i];
    return NULL;
}
static uint32_t snx_ctr_fs_wire_CloseArchive(SnxCtrIpc* c, uint64_t token) {
    if (!initialized(c)) return SNX_CTR_IPC_INVALID_STATE;
    CtrSelfArchive* a = archive(c->process, token);
    if (!a) return 0xc8804465u;
    a->token = 0;
    a->kind = 0;
    return 0;
}
/* A new typed file/directory object; duplicates share its closed state. */
static bool new_object(SnxCtrIpc* c, uint32_t kind, uint32_t writable, void* host, void* node) {
    SnxCtrProcess* p = c->process;
    unsigned count = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) count += p->objects[i].type == CTR_OBJ_FILE;
    if (count >= 64) {
        snx_ctr_ipc_error_reply(c, 0xd0401834u);
        return false;
    }
    uint32_t handle = 0, result = snx_ctr_object_create(p, CTR_OBJ_FILE, &handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return false;
    }
    CtrObject* object = &p->objects[snx_ctr_handle_object(p, handle)];
    object->data.file.kind = kind;
    object->data.file.writable = writable;
    object->data.file.host = host;
    object->data.file.node = node;
    if (node) fs_tree_retain((FsNode*)node);
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x42;
    c->out[2] = 0x10;
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) {
        if (node) fs_tree_release((FsNode*)node);
        snx_ctr_ipc_drop_handle(c, handle);
        return false;
    }
    return true;
}
/* Bounded path decode shared by every path-carrying command. PATH_BINARY and
   PATH_UTF16 carry UTF-16LE; PATH_ASCII carries ASCII bytes. */
/* The path's descriptor, the word before its pointer: static buffer 0 of the path's size (libctru's FSUSER_*) */
static bool path_descriptor(SnxCtrIpc* c, uint32_t size_word, uint32_t ptr_word) {
    if (c->in[ptr_word - 1] == ((c->in[size_word] << 14) | 2)) return true;
    snx_ctr_ipc_bad(c);
    return false;
}
static bool read_path(SnxCtrIpc* c, uint32_t type_word, uint32_t size_word, uint32_t ptr_word, char out[768],
                      const char** relative) {
    uint32_t type = c->in[type_word], size = c->in[size_word];
    if (!size || size > 256) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return false;
    }
    unsigned char raw[256];
    if (snx_ctr_read_ram(c->process->space, c->in[ptr_word], raw, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return false;
    }
    if (type == 3) { /* PATH_ASCII */
        unsigned n = 0;
        bool terminated = false;
        for (; n < size; ++n) {
            if (!raw[n]) {
                terminated = true;
                break;
            }
            if (raw[n] > 0x7f) {
                c->state = SNX_CTR_IPC_UNSUPPORTED;
                return false;
            }
            out[n] = (char)raw[n];
        }
        if (!terminated) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return false;
        }
        out[n] = 0;
    } else if (type == 2 || type == 4) {
        if (size < 2 || (size & 1) || !snx_ctr_utf16_path(raw, size, out, 768)) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return false;
        }
    } else {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return false;
    }
    *relative = out;
    while (**relative == '/') ++*relative;
    return true;
}
/* The read-write sdmc area: resolve an indexed host file and open it. */
static void open_host(SnxCtrIpc* c, uint32_t flags, const char* relative) {
    SnxCtrProcess* p = c->process;
    FsNode* node = NULL;
    FILE* host = NULL;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_lookup(p->tree, p->sd, relative, &node);
    if (rc == FS_TREE_NOTFOUND && (flags & 4)) rc = fs_tree_create(p->tree, p->sd, relative, 0, 0, &node);
    if (!rc && (node->is_dir || node->is_virtual)) rc = FS_TREE_INVALID;
    if (!rc) {
        host = fs_tree_open(p->tree, node, (flags & 2) ? "r+b" : "rb");
        if (!host) rc = FS_TREE_IO;
    }
    pm_unlock(&p->tree->lock);
    if (rc) {
        if (host) fclose(host);
        snx_ctr_ipc_error_reply(c, tree_result(rc));
        return;
    }
    if (!new_object(c, CTR_FILE_HOST, !!(flags & 2), host, node)) fclose(host);
}
/* SelfNCCH, empty archive path, binary selector for this process' RomFS. */
static void open_romfs(SnxCtrIpc* c, const unsigned char path[12]) {
    SnxCtrProcess* p = c->process;
    for (unsigned i = 0; i < 12; ++i)
        if (path[i]) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
    if (!p->romfs) {
        snx_ctr_ipc_error_reply(c, 0xc8804464u);
        return;
    }
    new_object(c, CTR_FILE_VIEW, 0, NULL, NULL);
}
/* FSUSER OpenFileDirectly: SelfNCCH with the 12-byte binary selector for the
   read-only RomFS view, or a direct read(-write) open on the sdmc area. */
static void open_direct(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 8, 4)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (c->in[1]) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[2] == 3) {
        if (c->in[3] != 1 || c->in[4] != 1 || c->in[5] != 2 || c->in[6] != 12 || c->in[7] != 1 || c->in[8]) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        if (c->in[9] != 0x4802 || c->in[11] != 0x30002) {
            snx_ctr_ipc_bad(c);
            return;
        }
        unsigned char path[12], empty;
        if (snx_ctr_read_ram(p->space, c->in[10], &empty, 1) != SNX_CTR_MEM_OK ||
            snx_ctr_read_ram(p->space, c->in[12], path, sizeof path) != SNX_CTR_MEM_OK) {
            c->transport_result = 0xe0e01bf5u;
            return;
        }
        if (empty) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        open_romfs(c, path);
        return;
    }
    /* SDMC direct open: empty archive path plus an indexed area path. */
    if (c->in[2] != 9 || c->in[3] != 1 || c->in[4] != 1 || c->in[9] != 0x4802 || c->in[11] != ((c->in[6] << 14) | 2)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    uint32_t flags = c->in[7];
    if (c->in[8] || (flags & ~7u) || !(flags & 3)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    unsigned char empty;
    if (snx_ctr_read_ram(p->space, c->in[10], &empty, 1) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (empty) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (!p->sd) {
        snx_ctr_ipc_error_reply(c, 0xc8804478u);
        return;
    }
    char path[768];
    const char* relative = NULL;
    if (!read_path(c, 5, 6, 12, path, &relative)) return;
    open_host(c, flags, relative);
}
static void open_file(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 7, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (c->in[1] || c->in[7] || c->in[8] != ((c->in[5] << 14) | 2)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    uint32_t size = c->in[5], flags = c->in[6];
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32);
    if (!a) {
        snx_ctr_ipc_error_reply(c, 0xc8804465u);
        return;
    }
    unsigned char raw[256];
    if (size > sizeof raw) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (snx_ctr_read_ram(p->space, c->in[9], raw, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (a->kind == 0) {
        if (size != 12 || flags != 1) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        open_romfs(c, raw);
        return;
    }
    /* SDMC: read(-write) opens of indexed files. FS_OPEN_READ/WRITE/CREATE. */
    if ((flags & ~7u) || !(flags & 3)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    char path[768];
    const char* relative = NULL;
    if (!read_path(c, 4, 5, 9, path, &relative)) return;
    open_host(c, flags, relative);
}
static void open_directory(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 4, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32);
    if (!a) {
        snx_ctr_ipc_error_reply(c, 0xc8804465u);
        return;
    }
    if (c->in[3] != 2 && c->in[3] != 4) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    char path[768];
    const char* relative = NULL;
    if (!path_descriptor(c, 4, 6) || !read_path(c, 3, 4, 6, path, &relative)) return;
    if (a->kind != 1) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    FsNode* node = NULL;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_lookup(p->tree, p->sd, relative, &node);
    if (!rc && !node->is_dir) rc = FS_TREE_INVALID;
    pm_unlock(&p->tree->lock);
    if (rc) {
        snx_ctr_ipc_error_reply(c, tree_result(rc));
        return;
    }
    new_object(c, CTR_FILE_DIR, 0, NULL, node);
}
static void create_entry(SnxCtrIpc* c, int is_dir) {
    if (!snx_ctr_ipc_expect(c, is_dir ? 6 : 8, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32);
    if (!a || a->kind != 1) {
        snx_ctr_ipc_error_reply(c, a ? 0xd8e01beeu : 0xc8804465u);
        return;
    }
    if (c->in[4] != 2 && c->in[4] != 4) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    uint64_t file_size = is_dir ? 0 : (uint64_t)c->in[7] | (uint64_t)c->in[8] << 32;
    char path[768];
    const char* relative = NULL;
    if (!path_descriptor(c, 5, is_dir ? 8 : 10) || !read_path(c, 4, 5, is_dir ? 8 : 10, path, &relative)) return;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_create(p->tree, p->sd, relative, is_dir, file_size, NULL);
    pm_unlock(&p->tree->lock);
    snx_ctr_ipc_error_reply(c, tree_result(rc));
}
static void remove_entry(SnxCtrIpc* c, int is_dir) {
    if (!snx_ctr_ipc_expect(c, 5, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32);
    if (!a || a->kind != 1) {
        snx_ctr_ipc_error_reply(c, a ? 0xd8e01beeu : 0xc8804465u);
        return;
    }
    if (c->in[4] != 2 && c->in[4] != 4) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    char path[768];
    const char* relative = NULL;
    if (!path_descriptor(c, 5, 7) || !read_path(c, 4, 5, 7, path, &relative)) return;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_remove(p->tree, p->sd, relative, is_dir, 0, 0);
    pm_unlock(&p->tree->lock);
    snx_ctr_ipc_error_reply(c, tree_result(rc));
}
static void rename_entry(SnxCtrIpc* c, int is_dir) {
    if (!snx_ctr_ipc_expect(c, 9, 4)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* src = archive(p, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32);
    CtrSelfArchive* dst = archive(p, (uint64_t)c->in[6] | (uint64_t)c->in[7] << 32);
    if (!src || !dst || src->kind != 1 || dst != src) {
        snx_ctr_ipc_error_reply(c, 0xc8804465u);
        return;
    }
    if ((c->in[4] != 2 && c->in[4] != 4) || (c->in[8] != 2 && c->in[8] != 4)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[10] != ((c->in[5] << 14) | 2) || c->in[12] != ((c->in[9] << 14) | (1u << 11) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    char from[768], to[768];
    const char *a_path = NULL, *b_path = NULL;
    if (!read_path(c, 4, 5, 11, from, &a_path)) return;
    if (!read_path(c, 8, 9, 13, to, &b_path)) return;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_rename(p->tree, p->sd, a_path, b_path, is_dir);
    pm_unlock(&p->tree->lock);
    snx_ctr_ipc_error_reply(c, tree_result(rc));
}
/* FSUSER ControlArchive: GET_TIMESTAMP returns the backing host file's real
   modification time; COMMIT_SAVE_DATA on the SD area is genuine: every write
   already landed on the host. Other actions stop. */
static void control_archive(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 5, 4)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32);
    if (!a) {
        snx_ctr_ipc_error_reply(c, 0xc8804465u);
        return;
    }
    if (!c->in[3]) { /* COMMIT_SAVE_DATA */
        if (a->kind == 1) {
            c->out[0] = 0x080d0040;
            return;
        }
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[3] != 1 || c->in[5] != 8 || c->in[4] < 2 || c->in[4] > 256 || (c->in[4] & 1) ||
        c->in[6] != ((c->in[4] << 4) | 10) || c->in[8] != ((8u << 4) | 12)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (a->kind == 0 && !p->romfs) {
        snx_ctr_ipc_error_reply(c, 0xc8804464u);
        return;
    }
    unsigned char path[256], out[8];
    if (snx_ctr_read_ram(p->space, c->in[7], path, c->in[4]) != SNX_CTR_MEM_OK ||
        snx_ctr_check_ram(p->space, c->in[9], sizeof out, SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint64_t datetime = 0;
    if (a->kind == 0) {
        SnxCtrTimeResult result = snx_ctr_romfs_time(p->romfs, path, c->in[4], &datetime);
        if (result == SNX_CTR_TIME_NOTFOUND) {
            snx_ctr_ipc_error_reply(c, 0xc8804478u);
            return;
        }
        if (result != SNX_CTR_TIME_OK) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
    } else {
        char decoded[768];
        const char* relative = NULL;
        if (!snx_ctr_utf16_path(path, c->in[4], decoded, sizeof decoded)) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        relative = decoded;
        while (*relative == '/') ++relative;
        FsNode* node = NULL;
        pm_lock(&p->tree->lock);
        FsTreeResult rc = fs_tree_lookup(p->tree, p->sd, relative, &node);
        uint64_t unix_ms = 0;
        if (!rc) rc = fs_tree_mtime(p->tree, node, &unix_ms);
        pm_unlock(&p->tree->lock);
        if (rc) {
            snx_ctr_ipc_error_reply(c, tree_result(rc));
            return;
        }
        datetime = unix_ms + UINT64_C(2208988800000);
    }
    for (unsigned i = 0; i < 8; ++i) out[i] = (unsigned char)(datetime >> (i * 8));
    if (snx_ctr_write_ram(p->space, c->in[9], out, sizeof out) != SNX_CTR_MEM_OK) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    c->out[0] = 0x080d0040;
}
static void open_archive(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if ((c->in[1] != 9 && c->in[1] != 3) || c->in[2] != 1 || c->in[3] != 1) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[4] != 0x4002) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char empty;
    if (snx_ctr_read_ram(p->space, c->in[5], &empty, 1) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (empty) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[1] == 3 && !p->romfs) {
        snx_ctr_ipc_error_reply(c, 0xc8804464u);
        return;
    }
    if (c->in[1] == 9 && !p->sd) {
        snx_ctr_ipc_error_reply(c, 0xc8804478u);
        return;
    }
    if (p->archive_sequence != UINT64_MAX)
        for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
            if (p->self_archives[i].token) continue;
            uint64_t token = ++p->archive_sequence;
            p->self_archives[i].token = token;
            p->self_archives[i].kind = c->in[1] == 9 ? 1u : 0u;
            c->out[0] = 0x080c00c0;
            c->out[2] = (uint32_t)token;
            c->out[3] = (uint32_t)(token >> 32);
            return;
        }
    snx_ctr_ipc_error_reply(c, 0xd0401834u);
}
void snx_ctr_fs_dispatch(SnxCtrIpc* c) {
    switch (c->in[0] >> 16) {
        case 0x802: open_file(c); break;
        case 0x803: open_direct(c); break;
        case 0x804: remove_entry(c, 0); break;
        case 0x805: rename_entry(c, 0); break;
        case 0x806: remove_entry(c, 1); break;
        case 0x808: create_entry(c, 0); break;
        case 0x809: create_entry(c, 1); break;
        case 0x80a: rename_entry(c, 1); break;
        case 0x80b: open_directory(c); break;
        case 0x80c: open_archive(c); break;
        case 0x80d: control_archive(c); break;
        default: snx_ctr_fs_wire_dispatch(c); break;
    }
}
