/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's files as the runtime itself reads them (the loader, shared fonts, the control data), beside the fsp-srv
   service in fs.c */
#ifndef SNX_FS_H
#define SNX_FS_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/plat.h" /* FILE, PFileMap */
#include "nx/hle/filesystem/fs_tree.h" /* FsTree, FsNode */

/* Indexes the game's data folder and maps RomFS and ExeFS for the run. Called once, before any guest file is loaded */
void fs_init(void);
/* Lookups by a path in the index. *size is 0 when the file isn't there */
FILE* fs_open_read(const char* path, u64* size);
int fs_map_file(const char* path, PFileMap* map); /* 0, or -1 */
u8* fs_read_file(const char* path, u64* size);    /* malloc'd, or NULL */
/* a file that exists only in memory, which the lookups find like any other. 0, or -1 */
int fs_add_virtual_file(const char* path, const void* data, u64 size);
/* the RomFS the game opens: romfs.bin as it is, or an image built from the romfs folder */
void romfs_init(void);
/* ---- HLE state in g_hle (hle.h) */
/* RomFS. With no romfs.bin, the image is made up from the romfs/ folder: its header and tables in memory (rmeta), the
   files' data read from the files themselves when the game reads that part of the image */
typedef struct {
    FsNode* node; /* retained for the run: the image keeps its files */
    u64 off;      /* where its data starts in the image */
    int logged;   /* its first read was logged, and its first failure warned (atomics: game threads read at once) */
    int warned;
} RFile;
/* unmapped RomFS files open on the host, least recently used closed first */
#define NOPEN 48
typedef struct {
    FsNode* node;
    FILE* f;
    u64 used; /* the clock when it was last read */
} ROpen;
/* The service objects' data, each in a pool (ipc.c's lock guards them, svcobj_new_data). obj is the object's pool
   index + 1, which ipc.c writes */
/* an IFileSystem's root folder, retained */
typedef struct {
    u32 obj;
    FsNode* root;
} FsData;
/* an IFile, or a system archive's IStorage (the RomFS's has no data). f is NULL for a node held in memory */
typedef struct {
    u32 obj;
    FILE* f;
    FsNode* node;
    u32 mode; /* OpenMode: 1 read, 2 write, 4 allow append */
} FileData;
/* an IDirectory: the entries filter picks (1 folders, 2 files), after the name last listed, until done */
typedef struct {
    u32 obj;
    FsNode* node;
    u32 filter;
    char last[FS_TREE_PATH_MAX]; /* a name, not a node: the entry may be deleted or renamed meanwhile */
    int done;
} DirData;
#define FS_FS_MAX   64 /* IFileSystems: one per mount (save data, the SD card, caches, BCAT), a few dozen at most */
#define FS_FILE_MAX 1024 /* IFiles and IStorages: each is an object of the fsp-srv domain, which holds DOMAIN_OBJECTS */
#define FS_DIR_MAX  128 /* IDirectories, 4 KB each: a game lists a few folders at a time, and closes each after */
typedef struct {
    FsTree tree;
    RFile* rfiles; /* in offset order: romfs_build lays the files out in this order */
    int nrfiles;
    u8* rmeta;
    u64 rmeta_size; /* header + tables sit at the start of the image */
    u64 rdata_off, rtotal;
    FILE* rraw;     /* plain romfs.bin, when it couldn't be mapped */
    FsNode* rimage; /* plain romfs.bin, mapped */
    ROpen openf[NOPEN]; /* rf_open's files, under tree.lock */
    u64 openclk;
    u8 emptyrom[0x200]; /* the empty RomFS of the system archives (emptyrom_init) */
    /* the service objects' data */
    FsData fss[FS_FS_MAX];
    FileData files[FS_FILE_MAX];
    DirData dirs[FS_DIR_MAX];
    u32 fs_next, file_next, dir_next;
} HleFs;
/* all zero: fs_init sets the tree up */
#endif
