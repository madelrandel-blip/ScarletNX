/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hactool as scarletnx --extract calls it, in its own process (snx_hac.c): the containers opened once, each NCA read
   where it lies in them, and the ExeFS and RomFS written straight into the extraction. Plain C90, for the driver;
   hactool's own headers stay inside hactool_lib */
#ifndef SNX_HAC_H
#define SNX_HAC_H

#include <stddef.h>
#include <stdint.h>

typedef struct SnxHac SnxHac;

/* a file of a container: an NSP's, or one in an XCI's secure partition */
typedef struct {
    const char* name;
    uint64_t offset, size; /* in the container's file */
} SnxHacEntry;

enum { SNX_HAC_OTHER, SNX_HAC_PROGRAM, SNX_HAC_CONTROL };

/* what an NCA's header says */
typedef struct {
    int type;  /* SNX_HAC_PROGRAM, SNX_HAC_CONTROL or SNX_HAC_OTHER */
    int patch; /* it has a Patch RomFS (BKTR) section: an update's */
    uint64_t title_id;
} SnxHacNca;

SnxHac* snx_hac_new(void);
void snx_hac_free(SnxHac* hac);

/* a prod.keys line: 1 when name is a key's, else 0. Its value must have the key's length in hex digits */
int snx_hac_set_key(SnxHac* hac, const char* name, const char* value);
/* after the last key: what derives from them */
void snx_hac_derive_keys(SnxHac* hac);
void snx_hac_add_title_key(SnxHac* hac, const unsigned char rights_id[16], const unsigned char key[16]);

/* a container to read NCAs in place from, XCI or NSP: its number for the calls below, or -1. Two at most */
int snx_hac_open(SnxHac* hac, const char* path, int xci);
size_t snx_hac_entries(SnxHac* hac, int container, const SnxHacEntry** entries);
/* the first size bytes of an entry: 0, or -1 */
int snx_hac_read(SnxHac* hac, int container, size_t entry, void* buffer, size_t size);
/* 0, or -1 when its header cannot be decrypted (the keys are wrong or missing) */
int snx_hac_nca(SnxHac* hac, int container, size_t entry, SnxHacNca* nca);
/* the NCA's ExeFS into exefs_dir and its RomFS into romfs_dir (either NULL to leave it), an update's over the base NCA
   (base_container -1 for none): 0, or -1 */
int snx_hac_extract(SnxHac* hac, int container, size_t entry, int base_container, size_t base_entry,
                    const char* exefs_dir, const char* romfs_dir);

/* tick(written, data) as hactool writes the extraction, written the bytes so far */
void snx_hac_progress(void (*tick)(uint64_t written, void* data), void* data);
/* cleanup(data) before an exit hactool takes on a dump it cannot read, which ends the process */
void snx_hac_on_exit(void (*cleanup)(void* data), void* data);
/* what hactool said last on its error stream, runs of 16 or more hex digits (possible keys) left out, or "" */
const char* snx_hac_message(void);

#endif
