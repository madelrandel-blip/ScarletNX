/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The platform layer on Linux, Android, FreeBSD and Apple systems: threads, time, the guest's address space with
   written-page tracking, files, libraries, the text entry dialog and the crash handler */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* REG_RIP, pipe2 */
#endif
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#if defined(__linux__)
#include <linux/falloc.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0 /* FreeBSD never reserves swap for a mapping */
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

extern char** environ;

/* the crash report needs about 12 KiB of stack; a thread whose own stack overflowed has none left */
#define ALT_STACK_SIZE 32768
/* below this a thread is a small helper that gets no alternate signal stack */
#define ALT_STACK_MIN_THREAD ((size_t)256 << 10)
/* the least stack a thread gets: FreeBSD's PTHREAD_STACK_MIN (2 KiB on amd64) is less than its dynamic linker takes
   to bind the thread's first call, so never under glibc's 16 KiB */
#define THREAD_STACK_FLOOR   ((size_t)PTHREAD_STACK_MIN > 16384 ? (size_t)PTHREAD_STACK_MIN : (size_t)16384)

/* ================================================================ condition variables */
void pc_init(PCond* c) {
#if defined(__APPLE__)
    pthread_cond_init(&c->c, NULL); /* Darwin has no clock attribute: timed waits are relative there */
#else
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC); /* deadlines don't move when the wall clock is set */
    pthread_cond_init(&c->c, &a);
    pthread_condattr_destroy(&a);
#endif
}
void pc_signal(PCond* c) { pthread_cond_signal(&c->c); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    struct timespec ts;
    int r;
    if (timeout_ns < 0) {
        pthread_cond_wait(&c->c, &m->l);
        return 0;
    }
#if defined(__APPLE__)
    ts.tv_sec = (time_t)(timeout_ns / 1000000000);
    ts.tv_nsec = (long)(timeout_ns % 1000000000);
    r = pthread_cond_timedwait_relative_np(&c->c, &m->l, &ts);
#else
    {
        u64 sec;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        sec = (u64)timeout_ns / 1000000000u;
        ts.tv_nsec += (long)((u64)timeout_ns % 1000000000u);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            sec++;
        }
        ts.tv_sec = (time_t)((u64)ts.tv_sec + sec);
    }
    r = pthread_cond_timedwait(&c->c, &m->l, &ts);
#endif
    return r == ETIMEDOUT;
}

/* ================================================================ threads */
typedef struct {
    PThreadFn fn;
    void* arg;
    int alt_stack;
} ThreadStart;

static void alt_stack_off(void) {
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
}

/* the alternate stack lives in this frame for as long as fn runs, and is switched off before the frame goes. Not
   inlined: in thread_start's frame the array would take its room on every thread, the small ones too */
__attribute__((noinline)) static void run_with_alt_stack(PThreadFn fn, void* arg) {
    char stack[ALT_STACK_SIZE];
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = stack;
    ss.ss_size = sizeof stack;
    sigaltstack(&ss, NULL);
    fn(arg);
    alt_stack_off();
}

static void* thread_start(void* p) {
    ThreadStart s = *(ThreadStart*)p;
    free(p);
    if (s.alt_stack)
        run_with_alt_stack(s.fn, s.arg);
    else
        s.fn(s.arg);
    return NULL;
}

#if defined(__linux__) && !defined(__ANDROID__)
/* every CPU the process may use (plat_cpus), which plat_thread gives each thread it starts: one a game thread started
   would otherwise run only on the share of that thread's core (plat_thread_cpus) */
static cpu_set_t g_all_cpus;
#endif
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    ThreadStart* s = malloc(sizeof *s);
    pthread_attr_t a;
    pthread_t t;
    int r;
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    s->alt_stack = stack == 0 || stack >= ALT_STACK_MIN_THREAD;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
#if defined(__linux__) && !defined(__ANDROID__)
    if (CPU_COUNT(&g_all_cpus)) pthread_attr_setaffinity_np(&a, sizeof g_all_cpus, &g_all_cpus);
#endif
    if (stack) {
        /* smaller than the minimum, setstacksize fails and the default would be used silently */
        if (stack < THREAD_STACK_FLOOR) stack = THREAD_STACK_FLOOR;
#if defined(__APPLE__)
        {
            size_t page = (size_t)sysconf(_SC_PAGESIZE); /* 16 KiB on Apple silicon; Darwin wants whole pages */
            stack = (stack + page - 1) / page * page;
        }
#endif
        pthread_attr_setstacksize(&a, stack);
    }
    r = pthread_create(&t, &a, thread_start, s);
    pthread_attr_destroy(&a);
    if (r) {
        free(s);
        return -1;
    }
    return 0;
}
void plat_thread_exit(void) {
    /* pthread_exit leaves run_with_alt_stack's frame without returning through it */
    alt_stack_off();
    pthread_exit(NULL);
}
/* Linux only: Android's CPUs differ in speed, so a fixed share of them would make a core slow */
#if defined(__linux__) && !defined(__ANDROID__)
int plat_cpus(int* ids, int max) {
    int c, n = 0;
    if (sched_getaffinity(0, sizeof g_all_cpus, &g_all_cpus)) {
        CPU_ZERO(&g_all_cpus);
        return 0;
    }
    for (c = 0; c < CPU_SETSIZE && n < max; c++)
        if (CPU_ISSET(c, &g_all_cpus)) ids[n++] = c;
    return n;
}
void plat_thread_cpus(const int* ids, int n) {
    cpu_set_t s;
    int i;
    CPU_ZERO(&s);
    for (i = 0; i < n; i++) CPU_SET(ids[i], &s);
    if (sched_setaffinity(0, sizeof s, &s))
        WARN_LIMIT(1, "Could not move a game thread to its CPUs: %s\n", strerror(errno));
}
#else
int plat_cpus(int* ids, int max) {
    (void)ids;
    (void)max;
    return 0;
}
void plat_thread_cpus(const int* ids, int n) {
    (void)ids;
    (void)n;
}
#endif
void plat_yield(void) { sched_yield(); }
void plat_sleep_ns(i64 ns) {
    struct timespec ts;
    if (ns <= 0) {
        sched_yield();
        return;
    }
    ts.tv_sec = (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    while (nanosleep(&ts, &ts) && errno == EINTR) {
    }
}
void plat_exit_now(int code) {
    fflush(NULL);
    _exit(code);
}

/* ================================================================ time */
u64 plat_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
}
i64 plat_unix_time(void) { return (i64)time(NULL); }
i64 plat_tz_offset(void) {
    time_t now = time(NULL);
    struct tm t;
    if (!localtime_r(&now, &t)) return 0;
    return (i64)t.tm_gmtoff;
}

/* ================================================================ the guest's address space */
/* Written-page tracking (Linux 6.7 and later): the reservation is registered with a userfaultfd in asynchronous
   write-protect mode, so the kernel unprotects a page on its first write (the guest's, or a read() into guest memory)
   with no handler thread, and PAGEMAP_SCAN lists the written pages and protects them again in one call. The
   userfaultfd is kept open for the whole run; only the ranges a cache scans are ever protected again. Registration
   belongs to a mapping: a range mapped again with MAP_FIXED (the alias pool's, below) is registered again, or a scan
   over it fails. g_pagemap goes back to -1 when that fails, so every scan says -1 and the caches compare contents */
static int g_pagemap = -1;
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY)
static int g_uffd = -1;
#endif

static int ww_register(u8* p, u64 size) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY)
    struct uffdio_register reg;
    if (g_uffd < 0) return 0;
    memset(&reg, 0, sizeof reg);
    reg.range.start = (u64)(uintptr_t)p;
    reg.range.len = size;
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    return ioctl(g_uffd, UFFDIO_REGISTER, &reg) ? -1 : 0;
#else
    (void)p;
    (void)size;
    return 0;
#endif
}

static void ww_init(u8* p, u64 size) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY)
    struct uffdio_api api;
    long fd;
    int pm;
    if (getenv("SNX_NO_WW")) return;
    fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (fd < 0) return;
    memset(&api, 0, sizeof api);
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
    g_uffd = (int)fd;
    if (ioctl((int)fd, UFFDIO_API, &api) || ww_register(p, size)) {
        close((int)fd);
        g_uffd = -1;
        return;
    }
    pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm < 0) {
        close((int)fd);
        g_uffd = -1;
        return;
    }
    g_pagemap = pm; /* before any other thread exists: mem_init runs on the main thread at startup */
#else
    (void)p;
    (void)size;
#endif
}
/* a range mapped again: registered again, or tracking off for the run (the pagemap stays open, as a scan on another
   thread may be using it) */
static void ww_remapped(u8* p, u64 size) {
    if (__atomic_load_n(&g_pagemap, __ATOMIC_ACQUIRE) < 0 || !ww_register(p, size)) return;
    __atomic_store_n(&g_pagemap, -1, __ATOMIC_RELEASE);
    WARN("memory: could not watch remapped guest memory for writes (error %d): caches compare contents now\n", errno);
}

int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
#if defined(__linux__) && defined(PAGEMAP_SCAN)
    struct page_region regions[64];
    struct pm_scan_arg arg;
    u64 mask = (u64)sysconf(_SC_PAGESIZE) - 1; /* the kernel's pages: PAGEMAP_SCAN wants its bounds on them */
    u64 start = (u64)(uintptr_t)p & ~mask, end = ((u64)(uintptr_t)p + size + mask) & ~mask;
    int pm = __atomic_load_n(&g_pagemap, __ATOMIC_ACQUIRE);
    if (pm < 0) return -1;
    while (start < end) {
        int i, n;
        memset(&arg, 0, sizeof arg);
        arg.size = sizeof arg;
        arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        arg.start = start;
        arg.end = end;
        arg.vec = (u64)(uintptr_t)regions;
        arg.vec_len = sizeof regions / sizeof *regions;
        arg.category_mask = PAGE_IS_WRITTEN;
        arg.return_mask = PAGE_IS_WRITTEN;
        n = ioctl(pm, PAGEMAP_SCAN, &arg);
        if (n < 0) return -1;
        for (i = 0; i < n; i++) cb((u8*)(uintptr_t)regions[i].start, regions[i].end - regions[i].start);
        /* a full vector may have stopped the walk early: go on from where it stopped */
        if ((u64)n < arg.vec_len || !arg.walk_end || arg.walk_end >= end) break;
        start = arg.walk_end;
    }
    return 0;
#else
    (void)p;
    (void)size;
    (void)cb;
    return -1;
#endif
}

u64 plat_vm_page(void) {
    long host = sysconf(_SC_PAGESIZE);
    u64 page = host > 0 ? (u64)host : 4096;
    const char* forced = getenv("SNX_HOST_PAGE");
    if (forced) { /* a larger power of two only: the host's pages can't be split */
        u64 v = strtoull(forced, NULL, 0);
        if (v > page && v <= (1u << 20) && !(v & (v - 1))) page = v;
    }
    return page;
}
/* mmap aligns to the host's own page: a larger plat_vm_page (SNX_HOST_PAGE) takes a page more and trims the rest, so
   guest addresses sit at the same place in a page as their host addresses */
u8* plat_vm_reserve(u64 size) {
    u64 page = plat_vm_page(), extra = page - (u64)sysconf(_SC_PAGESIZE), head;
    u8* p = (u8*)mmap(NULL, (size_t)(size + extra), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == (u8*)MAP_FAILED) return NULL;
    head = (page - (u64)(uintptr_t)p % page) % page;
    if (head) munmap(p, (size_t)head);
    if (extra > head) munmap(p + head + size, (size_t)(extra - head));
    p += head;
    ww_init(p, size);
    return p;
}
int plat_vm_commit(u8* p, u64 size) { return mprotect(p, (size_t)size, PROT_READ | PROT_WRITE) ? -1 : 0; }
void plat_vm_decommit(u8* p, u64 size) {
#if defined(__linux__)
    madvise(p, (size_t)size, MADV_DONTNEED);
    mprotect(p, (size_t)size, PROT_NONE);
#else
    /* only Linux's MADV_DONTNEED drops the contents (Darwin's may keep them, FreeBSD's does): fresh pages over the
       range are certain to read zero */
    mmap(p, (size_t)size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
}

/* The alias pool (Linux and Android): one memfd, as large as asked and sparse, whose pages mem.c maps at guest
   addresses with MAP_SHARED, so two addresses of the reservation (and a service's mapping outside it) are the same
   pages, and a freed range is punched out so it reads zero when handed out again. The other systems here keep mem.c's
   copies: the pool is untested there (FreeBSD has memfd_create and, from 14, fspacectl to free a range; Apple has
   shm_open, whose descriptors can't be read or written, and no way to free part of one) */
static int g_alias = -1;

int plat_alias_init(u64 size) {
#if defined(__linux__) && defined(SYS_memfd_create) && defined(FALLOC_FL_PUNCH_HOLE)
    long fd = syscall(SYS_memfd_create, "snx-alias", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return -1;
    if (ftruncate((int)fd, (off_t)size)) {
        close((int)fd);
        return -1;
    }
    g_alias = (int)fd; /* mem_init, before any other thread */
    return 0;
#else
    (void)size;
    return -1;
#endif
}
int plat_alias_map(u8* p, u64 off, u64 size) {
    if (g_alias < 0 ||
        mmap(p, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_alias, (off_t)off) == MAP_FAILED)
        return -1;
    ww_remapped(p, size);
    return 0;
}
int plat_alias_unmap(u8* p, u64 size) {
    if (mmap(p, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED)
        return -1;
    ww_remapped(p, size); /* the anonymous mapping is a new one too */
    return 0;
}
int plat_alias_write(u64 off, const void* src, u64 size) {
    const u8* s = (const u8*)src;
    while (size) {
        ssize_t n = pwrite(g_alias, s, (size_t)size, (off_t)off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        s += n, off += (u64)n, size -= (u64)n;
    }
    return 0;
}
int plat_alias_read(u64 off, void* dst, u64 size) {
    u8* d = (u8*)dst;
    while (size) {
        ssize_t n = pread(g_alias, d, (size_t)size, (off_t)off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        d += n, off += (u64)n, size -= (u64)n;
    }
    return 0;
}
void plat_alias_free(u64 off, u64 size) {
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
    if (g_alias >= 0 && fallocate(g_alias, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)size))
        fatal("Could not free %llu KiB of the alias pool (error %d)", (unsigned long long)(size >> 10), errno);
#else
    (void)off;
    (void)size;
#endif
}
u8* plat_alias_host(u64 off, u64 size) {
    void* p;
    if (g_alias < 0) return NULL;
    p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, g_alias, (off_t)off);
    return p == MAP_FAILED ? NULL : (u8*)p;
}
void plat_alias_host_free(u8* p, u64 size) { munmap(p, (size_t)size); }

/* ================================================================ libraries */
#if defined(__ANDROID__)
/* Shared storage (/sdcard) is mounted noexec: a module there loads from a copy in the app's cache folder */
static void* dl_open_from_cache(const char* path) {
    const char* cache = SDL_GetAndroidCachePath();
    const char* base = strrchr(path, '/');
    char copy[1400], buf[65536];
    FILE *in, *out;
    size_t n;
    int ok = 1;
    if (!cache) return NULL;
    base = base ? base + 1 : path;
    if ((size_t)snprintf(copy, sizeof copy, "%s/%s", cache, base) >= sizeof copy) return NULL;
    in = fopen(path, "rb");
    if (!in) return NULL;
    out = fopen(copy, "wb");
    if (!out) {
        fclose(in);
        return NULL;
    }
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in);
    fclose(in);
    if (fclose(out) || !ok) return NULL;
    return dlopen(copy, RTLD_NOW | RTLD_LOCAL);
}
#endif
void* plat_dl_open(const char* path) {
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#if defined(__ANDROID__)
    if (!h) h = dl_open_from_cache(path);
#endif
    return h;
}
void* plat_dl_sym(void* h, const char* name) { return dlsym(h, name); }
const char* plat_dl_error(void) {
    const char* e = dlerror();
    return e ? e : "no error reported";
}
int plat_setenv(const char* name, const char* value) { return setenv(name, value, 1); }

/* ================================================================ files and folders */
int plat_is_dir(const char* path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
u64 plat_file_size(const char* path) {
    struct stat st;
    return stat(path, &st) || st.st_size < 0 ? 0 : (u64)st.st_size;
}

typedef struct {
    DIR* dir;
    char path[1024]; /* the folder, to stat its entries */
} DirState;
int plat_dir_open(PDir* d, const char* path) {
    size_t len = strlen(path);
    DirState* s;
    memset(d, 0, sizeof *d);
    /* a longer path would be cut, and the entries stat'ed in some other folder */
    if (len >= sizeof s->path) return -1;
    s = malloc(sizeof *s);
    if (!s) return -1;
    memcpy(s->path, path, len + 1);
    s->dir = opendir(path);
    if (!s->dir) {
        free(s);
        return -1;
    }
    d->h = s;
    d->first = 1;
    return 0;
}
int plat_dir_next(PDir* d) {
    DirState* s = (DirState*)d->h;
    struct dirent* e;
    if (!s) return 0;
    d->first = 0;
    while ((e = readdir(s->dir)) != NULL) {
        char full[1600];
        struct stat st;
        size_t n = strlen(e->d_name);
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || n >= sizeof d->name) continue;
        memcpy(d->name, e->d_name, n + 1);
        snprintf(full, sizeof full, "%s/%s", s->path, e->d_name);
        d->is_dir = 0;
        d->size = 0;
        if (!stat(full, &st)) {
            d->is_dir = S_ISDIR(st.st_mode) != 0;
            d->size = st.st_size > 0 ? (u64)st.st_size : 0;
        }
        return 1;
    }
    return 0;
}
void plat_dir_close(PDir* d) {
    DirState* s = (DirState*)d->h;
    if (s) {
        closedir(s->dir);
        free(s);
    }
    d->h = NULL;
}

i64 plat_pread(FILE* f, void* buf, u64 size, u64 off) {
    int fd = fileno(f);
    u64 got = 0;
    if (fd < 0 || size > (u64)INT64_MAX || off > (u64)INT64_MAX - size) return -1;
    /* pread may return less than asked (past 2 GiB on Linux, or interrupted): go on until done or at the end */
    while (got < size) {
        ssize_t r = pread(fd, (u8*)buf + got, (size_t)(size - got), (off_t)(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (u64)r;
    }
    return (i64)got;
}

static void file_map_clear(PFileMap* m) {
    m->data = NULL;
    m->size = 0;
    m->borrowed = 0;
}
/* stat_it: the size comes from fstat, and the file must be a regular one; else known is trusted */
static int file_map(PFileMap* m, const char* path, int stat_it, u64 known) {
    int fd;
    u64 size = known;
    void* p;
    file_map_clear(m);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (stat_it) {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
            close(fd);
            return -1;
        }
        size = (u64)st.st_size;
    }
    if (size > (u64)SIZE_MAX || size > (u64)INT64_MAX) {
        close(fd);
        return -1;
    }
    if (!size) {
        close(fd);
        return 0;
    }
    p = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* the mapping keeps the file */
    if (p == MAP_FAILED) return -1;
    m->data = (const u8*)p;
    m->size = size;
    return 0;
}
int plat_file_map(PFileMap* m, const char* path) { return file_map(m, path, 1, 0); }
int plat_file_map_sized(PFileMap* m, const char* path, u64 size) { return file_map(m, path, 0, size); }
void plat_file_unmap(PFileMap* m) {
    if (m->data && !m->borrowed) munmap((void*)m->data, (size_t)m->size);
    file_map_clear(m);
}
int plat_replace_file(const char* tmp, const char* dst) {
    if (!rename(tmp, dst)) return 0;
    remove(tmp);
    return -1;
}

/* ================================================================ text entry */
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
int plat_text_input(char* out, size_t cap, const char* initial) {
    (void)out;
    (void)cap;
    (void)initial;
    return -1;
}
#else
enum { DIALOG_OK = 0, DIALOG_CANCELLED = 1, DIALOG_FAILED = -1, DIALOG_MISSING = -2 };

/* Runs a dialog program (no shell: the initial text is the game's data) and takes the first line it prints */
static int run_dialog(char* const argv[], char* out, size_t cap) {
    posix_spawn_file_actions_t fa;
    pid_t pid;
    int pipefd[2], r, status, len_ok = 1;
    size_t len = 0;
    int line_done = 0;
    char buf[512];
    if (pipe(pipefd)) return DIALOG_FAILED;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC); /* dup2 onto stdout clears it for the child's copy */
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    r = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (r) {
        close(pipefd[0]);
        return r == ENOENT ? DIALOG_MISSING : DIALOG_FAILED;
    }
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof buf);
        ssize_t i;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) len_ok = 0;
        if (n <= 0) break;
        for (i = 0; i < n && !line_done; i++) {
            if (buf[i] == '\n')
                line_done = 1;
            else if (len + 1 < cap)
                out[len++] = buf[i];
            else
                len_ok = 0; /* longer than the game's buffer */
        }
    }
    close(pipefd[0]);
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return DIALOG_FAILED;
    if (!WIFEXITED(status)) return DIALOG_FAILED;
    switch (WEXITSTATUS(status)) {
        case 0:
            if (!len_ok || !cap) return DIALOG_FAILED;
            out[len] = 0;
            return DIALOG_OK;
        case 1: return DIALOG_CANCELLED;
        case 127: return DIALOG_MISSING; /* posix_spawnp may report a missing program this way */
        default: return DIALOG_FAILED;
    }
}

int plat_text_input(char* out, size_t cap, const char* initial) {
    int r;
#if defined(__APPLE__)
    /* the text goes in as an argument of the script, never spliced into its source */
    char* argv[] = {"osascript",
                    "-e",
                    "on run argv",
                    "-e",
                    "return text returned of (display dialog \"Enter text\" default answer (item 1 of argv) with title "
                    "\"ScarletNX\")",
                    "-e",
                    "end run",
                    "--",
                    NULL,
                    NULL};
    argv[8] = (char*)initial;
    r = run_dialog(argv, out, cap);
#else
    char entry[1100];
    const char* desk = getenv("XDG_CURRENT_DESKTOP");
    int kde_first = desk && (strstr(desk, "KDE") || strstr(desk, "Plasma"));
    char* zenity[] = {"zenity", "--entry", "--title=ScarletNX", "--text=Enter text", NULL, NULL};
    char* kdialog[] = {"kdialog", "--title", "ScarletNX", "--inputbox", "Enter text", NULL, NULL};
    if ((size_t)snprintf(entry, sizeof entry, "--entry-text=%s", initial) >= sizeof entry) return -1;
    zenity[4] = entry;
    kdialog[5] = (char*)initial;
    r = run_dialog(kde_first ? kdialog : zenity, out, cap);
    if (r == DIALOG_MISSING) r = run_dialog(kde_first ? zenity : kdialog, out, cap);
#endif
    return r < 0 ? -1 : r;
}
#endif

/* ================================================================ the crash handler */
static int g_crash_reporting; /* a thread is reporting a crash */
static __thread int t_crashed; /* this thread faulted already */
static char g_main_alt_stack[ALT_STACK_SIZE];

static void* context_pc(void* ctx) {
    ucontext_t* uc = (ucontext_t*)ctx;
#if defined(__linux__) && defined(__x86_64__)
    return (void*)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
    return (void*)(uintptr_t)uc->uc_mcontext.pc;
#elif defined(__linux__) && defined(__powerpc64__)
    return (void*)(uintptr_t)uc->uc_mcontext.gp_regs[32]; /* PT_NIP (asm/ptrace.h), glibc's and musl's alike */
#elif defined(__FreeBSD__) && defined(__x86_64__)
    return (void*)(uintptr_t)uc->uc_mcontext.mc_rip;
#elif defined(__FreeBSD__) && defined(__aarch64__)
    return (void*)(uintptr_t)uc->uc_mcontext.mc_gpregs.gp_elr;
#elif defined(__APPLE__) && defined(__aarch64__)
    return (void*)(uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
    return (void*)(uintptr_t)uc->uc_mcontext->__ss.__rip;
#else
    (void)uc;
    return NULL;
#endif
}

static void crash_signal(int sig, siginfo_t* si, void* ctx) {
    int idle = 0;
    void* fault;
    /* a fault inside the report itself */
    if (t_crashed) _exit(1);
    t_crashed = 1;
    /* one report at a time: it ends the process, so a second crashing thread just waits for that */
    if (!__atomic_compare_exchange_n(&g_crash_reporting, &idle, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        for (;;) pause();
    log_crash_mode();
    /* for SIGILL and SIGFPE si_addr is the instruction, not an address it touched */
    fault = sig == SIGSEGV || sig == SIGBUS ? si->si_addr : NULL;
    crash_report(context_pc(ctx), fault, sig);
    _exit(1);
}

/* abort() (a driver's assert, a failed allocation in a library): say so, then let it go on to end the process */
static void abort_signal(int sig) {
    log_crash_mode();
    logf_(0, "*** Shutdown: abort() (signal %d), probably the video driver or out of memory ***\n", sig);
}

void plat_install_crash_handler(void) {
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    struct sigaction sa;
    stack_t ss;
    size_t i;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = g_main_alt_stack;
    ss.ss_size = sizeof g_main_alt_stack;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = crash_signal;
    /* NODEFER: a fault during the report re-enters the handler, which ends the process, instead of hanging */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    for (i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = abort_signal;
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    sigaction(SIGABRT, &sa, NULL);
}

void plat_prof_start(void) {}
