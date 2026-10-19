/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from LibHac (https://github.com/Thealexbarney/LibHac, MIT, Copyright (c) 2018 Alex Barney), as maintained by
   Ryubing: BucketTree, IndirectStorage, SparseStorage, AesCtrCounterExtendedStorage and CompressedStorage. Its
   license notice is in storage.c. */
#ifndef HACTOOL_STORAGE_H
#define HACTOOL_STORAGE_H

/* A section's bytes reach hactool through a chain of storages, the way FS (and LibHac) builds it: the file, then the
   sparse layer, the decryption, the indirect (patch) layer and the compressed layer, each reading from the one below.
   A storage does not own the storages it reads from: whoever builds a chain frees each link with storage_free. */

#include <stdio.h>
#include "types.h"
#include "aes.h"

#define BUCKET_TREE_MAGIC 0x52544B42 /* "BKTR" */
#define BUCKET_TREE_NODE_SIZE 0x4000

/* Entry sizes, as the tables store them. */
#define INDIRECT_ENTRY_SIZE 0x14   /* virtual offset, physical offset, storage index */
#define AES_CTR_EX_ENTRY_SIZE 0x10 /* offset, encryption (0 encrypted, 1 not), reserved, generation */
#define COMPRESSED_ENTRY_SIZE 0x18 /* virtual offset, physical offset, type, level, physical size */

enum compression_type {
    COMPRESSION_NONE = 0,
    COMPRESSION_ZEROED = 1,
    COMPRESSION_LZ4 = 3
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t entry_count;
    int32_t reserved;
} bucket_tree_header_t;

/* Every entry of a tree, in offset order, with the end of the last one. */
typedef struct {
    unsigned char *entries;
    uint64_t entry_size;
    uint32_t entry_count;
    uint64_t end_offset;
} bucket_tree_t;

typedef struct storage storage_t;
struct storage {
    /* Reads [offset, offset + size), which storage_read has checked is inside the storage. 1 on success. */
    int (*read)(storage_t *storage, uint64_t offset, void *buffer, uint64_t size);
    void (*destroy)(storage_t *storage);
    uint64_t size;
};

int storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size);
void storage_free(storage_t *storage);

storage_t *storage_file_new(FILE *file, uint64_t offset, uint64_t size);
storage_t *storage_sub_new(storage_t *base, uint64_t offset, uint64_t size);
storage_t *storage_fill_new(uint64_t size, unsigned char value);
/* [0, split) from low, [split, split + high->size) from high. */
storage_t *storage_switch_new(storage_t *low, uint64_t split, storage_t *high);
/* The counter of the block at offset is upper_iv (big-endian) followed by (counter_offset + offset) / 16. */
storage_t *storage_aes_ctr_new(storage_t *base, aes_ctx_t *aes, const unsigned char upper_iv[8], uint64_t counter_offset);
/* Sector n of the storage is XTS sector first_sector + n. */
storage_t *storage_aes_xts_new(storage_t *base, aes_ctx_t *aes, uint64_t sector_size, uint64_t first_sector);
/* The storages below take ownership of their tree. */
storage_t *storage_aes_ctr_ex_new(storage_t *base, aes_ctx_t *aes, const unsigned char upper_iv[8], uint64_t counter_offset, bucket_tree_t *tree);
storage_t *storage_indirect_new(storage_t *original, storage_t *patch, bucket_tree_t *tree);
storage_t *storage_compressed_new(storage_t *data, bucket_tree_t *tree);

/* Checks the 16-byte table header of an NCA FS header (patch, sparse or compression info). */
int bucket_tree_header_verify(const bucket_tree_header_t *header);
uint64_t bucket_tree_query_node_storage_size(uint64_t entry_size, uint32_t entry_count);
uint64_t bucket_tree_query_entry_storage_size(uint64_t entry_size, uint32_t entry_count);
/* The tree whose node storage is [offset, offset + node size) of table and whose entry sets follow it. NULL, after
   saying why on stderr, when the table is inconsistent. */
bucket_tree_t *bucket_tree_load(storage_t *table, uint64_t offset, uint64_t size, uint64_t entry_size, uint32_t entry_count);
/* An empty tree covering [0, end_offset), as LibHac makes for a table of no entries. */
bucket_tree_t *bucket_tree_empty(uint64_t end_offset);
void bucket_tree_free(bucket_tree_t *tree);

/* Whether any byte of [offset, offset + size) of an indirect storage comes from its patch storage. */
int storage_indirect_has_patch_data(storage_t *indirect, uint64_t offset, uint64_t size);

#endif
