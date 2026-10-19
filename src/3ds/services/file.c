/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR FSFILE wire facts: libctru services/fs.c and Azahar service/fs/file.cpp.
   Two real file kinds: the immutable shared RomFS view and read-write host
   files in the sdmc tree. Directory objects share close state only. */
#include "3ds/runtime/internal.h"
#include "3ds/services/file.h"
#include "services/file.swipc.inc"
#include <stdlib.h>
static uint32_t snx_ctr_file_wire_GetSize(SnxCtrIpc* c, uint64_t* size) {
    CtrObject* object = &c->process->objects[c->object];
    if (object->data.file.closed) return SNX_CTR_IPC_INVALID_STATE;
    SnxCtrProcess* p = c->process;
    if (object->data.file.kind == CTR_FILE_VIEW) {
        *size = snx_ctr_romfs_size(p->romfs);
        return 0;
    }
    pm_lock(&p->tree->lock);
    FsNode* node = object->data.file.node;
    *size = node ? node->size : 0;
    pm_unlock(&p->tree->lock);
    return 0;
}
void snx_ctr_file_release(SnxCtrProcess* p, uint32_t id) {
    CtrObject* object = &p->objects[id];
    object->data.file.closed = 1;
    if (object->data.file.kind == CTR_FILE_HOST && object->data.file.host) {
        fclose((FILE*)object->data.file.host);
        object->data.file.host = NULL;
    }
    if (object->data.file.node) {
        fs_tree_release((FsNode*)object->data.file.node);
        object->data.file.node = NULL;
    }
}
static uint32_t snx_ctr_file_wire_Close(SnxCtrIpc* c) {
    CtrObject* object = &c->process->objects[c->object];
    if (object->data.file.closed) return SNX_CTR_IPC_INVALID_STATE;
    snx_ctr_file_release(c->process, c->object);
    return 0;
}
void snx_ctr_file_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    uint32_t command = c->in[0] >> 16;
    if (command != 0x802 && command != 0x803 && command != 0x805) {
        snx_ctr_file_wire_dispatch(c);
        return;
    }
    CtrObject* object = &p->objects[c->object];
    if (object->data.file.closed) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (command == 0x805) { /* SetSize: read-write host files only */
        if (!snx_ctr_ipc_expect(c, 2, 0)) return;
        if (object->data.file.kind != CTR_FILE_HOST || !object->data.file.writable) {
            snx_ctr_ipc_error_reply(c, 0xd8e01beeu);
            return;
        }
        uint64_t size = (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32;
        pm_lock(&p->tree->lock);
        FsTreeResult rc = fs_tree_resize(p->tree, object->data.file.node, size);
        pm_unlock(&p->tree->lock);
        if (rc) {
            snx_ctr_ipc_error_reply(c, 0xe0e01bf4u);
            return;
        }
        c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
        return;
    }
    if (!snx_ctr_ipc_expect(c, command == 0x802 ? 3 : 4, 2)) return;
    uint32_t size = c->in[3], address = c->in[command == 0x802 ? 5 : 6];
    uint64_t offset = (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32;
    if (command == 0x803 && object->data.file.kind != CTR_FILE_HOST) {
        snx_ctr_ipc_error_reply(c, 0xd8e01beeu); /* the RomFS view is immutable */
        return;
    }
    if (size > 16u * 1024 * 1024) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[command == 0x802 ? 4 : 5] != (size << 4 | (command == 0x802 ? 12 : 10))) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (command == 0x802) {
        if (snx_ctr_check_ram(p->space, address, size, SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
            c->transport_result = 0xe0e01bf5u;
            return;
        }
    } else if (snx_ctr_check_ram(p->space, address, size, SNX_CTR_READ) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint64_t total = object->data.file.kind == CTR_FILE_VIEW ? snx_ctr_romfs_size(p->romfs) : 0;
    if (object->data.file.kind != CTR_FILE_VIEW) {
        pm_lock(&p->tree->lock);
        total = ((FsNode*)object->data.file.node)->size;
        pm_unlock(&p->tree->lock);
    }
    uint32_t n = size;
    if (command == 0x802 && offset >= total)
        n = 0;
    else if (command == 0x802 && total - offset < size)
        n = (uint32_t)(total - offset);
    unsigned char* data = malloc(n ? n : 1);
    if (!data) {
        snx_ctr_ipc_error_reply(c, 0xd86007f3u);
        return;
    }
    if (command == 0x802) { /* Read: stage the complete result before guest writes */
        bool ok;
        if (object->data.file.kind == CTR_FILE_VIEW) {
            ok = !n || snx_ctr_romfs_read(p->romfs, offset, data, n);
        } else {
            FILE* host = object->data.file.host;
            pm_lock(&p->tree->lock);
            ok = !n || (!fseek(host, (long)offset, SEEK_SET) && fread(data, 1, n, host) == n);
            pm_unlock(&p->tree->lock);
        }
        if (ok && n) ok = snx_ctr_write_ram(p->space, address, data, n) == SNX_CTR_MEM_OK;
        free(data);
        if (!ok) {
            c->state = SNX_CTR_IPC_FAULT;
            return;
        }
        c->out[0] = 0x08020082;
        c->out[2] = n;
        c->out[3] = c->in[4];
        c->out[4] = address;
        return;
    }
    /* Write: the guest bytes are staged first, then committed to the host. */
    if (n && snx_ctr_read_ram(p->space, address, data, n) != SNX_CTR_MEM_OK) {
        free(data);
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    FILE* host = object->data.file.host;
    pm_lock(&p->tree->lock);
    bool ok = !n || (!fseek(host, (long)offset, SEEK_SET) && fwrite(data, 1, n, host) == n && !fflush(host));
    if (ok) {
        FsNode* node = object->data.file.node;
        if (offset + n > node->size) node->size = offset + n;
    }
    pm_unlock(&p->tree->lock);
    free(data);
    if (!ok) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    c->out[0] = 0x08030082;
    c->out[2] = n;
    c->out[3] = c->in[5];
    c->out[4] = address;
}
