/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "gnuabi/process/process.h"
#include "gnuabi/network/network.h"
#include "gnuabi/abi/errno/errno.h"
#include "common/utilities/common.h"
#include <openssl/rand.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* the clocks' FILETIME */
#else
#include <unistd.h>
#endif

/* 0, or -GNU_EIO: RAND_bytes says 1 when it filled the buffer, and takes an int's worth at a time */
int gnu_random_bytes(void* destination, size_t size) {
    unsigned char* out = destination;
    while (size) {
        int n = size > INT_MAX ? INT_MAX : (int)size;
        if (RAND_bytes(out, n) != 1) return -GNU_EIO;
        out += n;
        size -= (size_t)n;
    }
    return 0;
}
int gnu_clock_gettime(int id, int64_t* seconds, int64_t* nanos) {
    if (id != 0 && id != 1 && id != 4 && id != 5 && id != 6 && id != 7 && id != 2 && id != 3) return -GNU_EINVAL;
#ifdef _WIN32
    if (id == 0 || id == 5) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        ticks -= UINT64_C(116444736000000000);
        *seconds = (int64_t)(ticks / 10000000);
        *nanos = (int64_t)(ticks % 10000000) * 100;
    } else if (id == 2 || id == 3) {
        FILETIME created, exit_time, kernel, user;
        BOOL ok = id == 2 ? GetProcessTimes(GetCurrentProcess(), &created, &exit_time, &kernel, &user)
                          : GetThreadTimes(GetCurrentThread(), &created, &exit_time, &kernel, &user);
        if (!ok) return -GNU_EIO;
        uint64_t ticks = (((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                         (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime);
        *seconds = (int64_t)(ticks / 10000000);
        *nanos = (int64_t)(ticks % 10000000) * 100;
    } else {
        LARGE_INTEGER counter, frequency;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        *seconds = counter.QuadPart / frequency.QuadPart;
        *nanos = (int64_t)((counter.QuadPart % frequency.QuadPart) * 1000000000 / frequency.QuadPart);
    }
#else
    clockid_t host = id == 0 || id == 5 ? CLOCK_REALTIME
                     : id == 2          ? CLOCK_PROCESS_CPUTIME_ID
                     : id == 3          ? CLOCK_THREAD_CPUTIME_ID
                                        : CLOCK_MONOTONIC;
    struct timespec time;
    if (clock_gettime(host, &time)) return -GNU_EIO;
    *seconds = time.tv_sec;
    *nanos = time.tv_nsec;
#endif
    return 0;
}
void gnu_sleep(int64_t seconds, int64_t nanoseconds) {
#ifdef _WIN32
    Sleep((DWORD)(seconds * 1000 + nanoseconds / 1000000));
#else
    struct timespec t;
    t.tv_sec = (time_t)seconds;
    t.tv_nsec = (long)nanoseconds;
    while (nanosleep(&t, &t));
#endif
}
static int push(GnuProcess* p, uint64_t* sp, const void* data, size_t n) {
    if (n > *sp || *sp - n < UINT64_C(0xef800000)) return -GNU_E2BIG;
    *sp -= n;
    return gnu_memory_write(&p->memory, *sp, data, n);
}
int gnu_process_stack(GnuProcess* p, int argc, const char* const* argv, const char* const* env) {
    uint64_t sp = UINT64_C(0xf0000000), stack_low = UINT64_C(0xef800000);
    if (argc < 1 || argc > 65536) return -GNU_EINVAL;
    size_t envc = 0;
    if (env)
        while (env[envc]) {
            if (++envc > 65536) return -GNU_E2BIG;
        }
    if (gnu_memory_map(&p->memory, stack_low, sp - stack_low, 3, 0)) return -GNU_ENOMEM;
    uint64_t* arguments = calloc((size_t)argc + envc, sizeof *arguments);
    if (!arguments) return -GNU_ENOMEM;
    uint64_t* environment = arguments + argc;
    int result = 0;
    for (size_t i = envc; i > 0; i--) {
        if ((result = push(p, &sp, env[i - 1], strlen(env[i - 1]) + 1))) goto done;
        environment[i - 1] = sp;
    }
    for (int i = argc; i > 0; i--) {
        if (!argv[i - 1] || (result = push(p, &sp, argv[i - 1], strlen(argv[i - 1]) + 1))) {
            result = -GNU_E2BIG;
            goto done;
        }
        arguments[i - 1] = sp;
    }
    const char platform[] = "aarch64";
    if ((result = push(p, &sp, platform, sizeof platform))) goto done;
    uint64_t platform_address = sp;
    unsigned char random[16];
    if ((result = gnu_random_bytes(random, sizeof random)) || (result = push(p, &sp, random, sizeof random))) goto done;
    uint64_t random_address = sp;
    /* Features advertised here must be implemented by the shared recompiler. */
    const uint64_t auxiliary[] = {
        3,  p->elf.phdr,
        4,  p->elf.phentsize,
        5,  p->elf.phnum,
        6,  GNU_PAGE_SIZE,
        7,  p->interp.image ? p->interp.bias : 0,
        8,  0,
        9,  p->elf.entry,
        11, 1000,
        12, 1000,
        13, 1000,
        14, 1000,
        15, platform_address,
        16, 3,
        17, 100,
        23, 0,
        25, random_address,
        26, 0,
        31, arguments[0],
        0,  0
    };
    size_t words = 1 + (size_t)argc + 1 + envc + 1 + sizeof auxiliary / sizeof auxiliary[0];
    uint64_t bytes = (uint64_t)words * 8;
    uint64_t initial = (sp - bytes) & ~UINT64_C(15);
    if (initial < stack_low) {
        result = -GNU_E2BIG;
        goto done;
    }
    uint64_t* values = calloc(words, 8);
    if (!values) {
        result = -GNU_ENOMEM;
        goto done;
    }
    size_t index = 0;
    values[index++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) values[index++] = arguments[i];
    values[index++] = 0;
    for (size_t i = 0; i < envc; i++) values[index++] = environment[i];
    values[index++] = 0;
    memcpy(values + index, auxiliary, sizeof auxiliary);
    result = gnu_memory_write(&p->memory, initial, values, (size_t)bytes);
    free(values);
    if (!result) {
        p->cpu.x[31] = initial;
        /* a dynamic program starts in its dynamic linker, which finds the program through AT_PHDR and AT_ENTRY */
        p->cpu.pc = p->interp.image ? p->interp.entry : p->elf.entry;
        /* Linux starts all general registers at zero; argc/argv live on the stack. */
        result = gnu_memory_protect(&p->memory, stack_low, UINT64_C(0x800000), p->elf.stack_permissions);
    }
done:
    free(arguments); /* and the environment after them */
    return result;
}

int gnu_process_stack32(GnuProcess* p, int argc, const char* const* argv, const char* const* env) {
    uint32_t sp = 0xf0000000u, stack_low = 0xef800000u;
    if (argc < 1 || argc > 65536) return -GNU_EINVAL;
    size_t envc = 0;
    for (; env && env[envc]; )
        if (++envc > 65536) return -GNU_E2BIG;
    if (gnu_memory_map(&p->memory, stack_low, (uint64_t)sp - stack_low, 3, 0)) return -GNU_ENOMEM;
    uint32_t* arguments = calloc((size_t)argc, sizeof *arguments);
    uint32_t* environment = calloc(envc ? envc : 1, sizeof *environment);
    if (!arguments || !environment) {
        free(arguments);
        free(environment);
        return -GNU_ENOMEM;
    }
    int result = 0;
    uint64_t s64 = sp;
    for (size_t i = envc; i > 0; i--) {
        if ((result = push(p, &s64, env[i - 1], strlen(env[i - 1]) + 1))) goto done;
        environment[i - 1] = (uint32_t)s64;
    }
    for (int i = argc; i > 0; i--) {
        if (!argv[i - 1] || (result = push(p, &s64, argv[i - 1], strlen(argv[i - 1]) + 1))) {
            result = -GNU_E2BIG;
            goto done;
        }
        arguments[i - 1] = (uint32_t)s64;
    }
    const char platform[] = "arm";
    if ((result = push(p, &s64, platform, sizeof platform))) goto done;
    uint32_t platform_address = (uint32_t)s64;
    unsigned char random[16];
    if ((result = gnu_random_bytes(random, sizeof random)) || (result = push(p, &s64, random, sizeof random)))
        goto done;
    uint32_t random_address = (uint32_t)s64;
    const uint32_t auxiliary[] = {3,  (uint32_t)p->elf.phdr,
                                  4,  p->elf.phentsize,
                                  5,  p->elf.phnum,
                                  6,  GNU_PAGE_SIZE,
                                  7,  0,
                                  8,  0,
                                  9,  (uint32_t)p->elf.entry | (p->elf.thumb ? 1u : 0u),
                                  11, 1000,
                                  12, 1000,
                                  13, 1000,
                                  14, 1000,
                                  15, platform_address,
                                  16, 3,
                                  17, 100,
                                  23, 0,
                                  25, random_address,
                                  26, 0,
                                  31, arguments[0],
                                  0,  0};
    size_t words = 1 + (size_t)argc + 1 + envc + 1 + sizeof auxiliary / sizeof auxiliary[0];
    uint64_t bytes = (uint64_t)words * 4;
    uint64_t initial = (s64 - bytes) & ~UINT64_C(7);
    if (initial < stack_low) {
        result = -GNU_E2BIG;
        goto done;
    }
    uint32_t* values = calloc(words, 4);
    if (!values) {
        result = -GNU_ENOMEM;
        goto done;
    }
    size_t index = 0;
    values[index++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) values[index++] = arguments[i];
    values[index++] = 0;
    for (size_t i = 0; i < envc; i++) values[index++] = environment[i];
    values[index++] = 0;
    memcpy(values + index, auxiliary, sizeof auxiliary);
    result = gnu_memory_write(&p->memory, initial, values, (size_t)bytes);
    free(values);
    if (!result) {
        p->arm.r[13] = (uint32_t)initial;
        p->arm.r[15] = (uint32_t)p->elf.entry;
        p->arm.cpsr = 0x10u | (p->elf.thumb ? 0x20u : 0u); /* USR, Thumb state as loaded */
        result = gnu_memory_protect(&p->memory, stack_low, 0x800000, p->elf.stack_permissions);
    }
done:
    free(arguments);
    free(environment);
    return result;
}
static void code_changed(void* context, uint64_t address, uint64_t length) {
    GnuProcess* p = context;
    gnu_jit_invalidate(&p->jit, address, length);
}

int gnu_process_load(GnuProcess* p, const char* path, const unsigned char* image, size_t size, int argc,
                     const char* const* argv, const char* const* env, char* error, size_t cap) {
    int r = gnu_memory_init(&p->memory);
    if (cap) error[0] = 0;
    if (r) {
        snprintf(error, cap, "Cannot reserve the guest address space");
        return r;
    }
    p->memory.code_changed = code_changed;
    p->memory.code_context = p;
    r = gnu_elf_load_image(&p->elf, &p->memory, image, size, error, cap);
    if (!r && p->elf.interp[0]) {
        /* the dynamic linker, from the guest's namespace (the sysroot's /lib/ld-linux-aarch64.so.1) */
        unsigned char* linker = NULL;
        size_t linker_size = 0;
        char guest[4096];
        if (p->elf.bits == 32) {
            snprintf(error, cap, "Dynamically linked AArch32 programs are unsupported");
            return -GNU_ENOEXEC;
        }
        r = gnu_vfs_load(&p->vfs, -100, p->elf.interp, &linker, &linker_size, guest, sizeof guest);
        if (r) {
            snprintf(error, cap, "Cannot load the dynamic linker %s (%d)", p->elf.interp, r);
            return r;
        }
        r = gnu_elf_load_at(&p->interp, &p->memory, linker, linker_size, GNU_ELF_INTERP_BIAS, 0, error, cap);
        free(linker);
    }
    if (r) return r;
    if (p->elf.bits == 32) {
        memset(&p->arm, 0, sizeof p->arm);
        p->arm.mem = p->memory.base;
        p->arm.mem_size = p->memory.size;
        p->arm.cpsr = 0x10u | (p->elf.thumb ? 0x20u : 0u);
        r = gnu_process_stack32(p, argc, argv, env);
    } else {
        /* Linux starts all general registers at zero */
        memset(&p->cpu, 0, sizeof p->cpu);
        p->cpu.mem = p->memory.base;
        p->cpu.rt = &gnu_rtapi;
        p->cpu.thread = p;
        gnu_jit_bind(&p->jit, &p->cpu);
        r = gnu_process_stack(p, argc, argv, env);
    }
    if (r) {
        snprintf(error, cap, "Cannot build the initial stack (%d)", r);
        return r;
    }
    r = gnu_vfs_set_process(&p->vfs, p->pid, path, argc, argv);
    /* the isolated namespace has no host files: the program is imported as /program */
    if (!r && !strcmp(path, "/program")) r = gnu_vfs_set_executable(&p->vfs, image, size);
    if (!r && p->elf.bits == 64 && p->jit.mode == GNU_AOT) {
        SnxExecRange* ranges = calloc(p->elf.code_count ? p->elf.code_count : 1, sizeof *ranges);
        if (!ranges) return -GNU_ENOMEM;
        for (size_t i = 0; i < p->elf.code_count; i++) {
            ranges[i].address = p->elf.code[i].address;
            ranges[i].data = p->elf.code[i].data;
            ranges[i].size = p->elf.code[i].size;
        }
        r = gnu_jit_prepare(&p->jit, ranges, p->elf.code_count, p->elf.entry, error, cap);
        free(ranges);
    }
    return r;
}

int gnu_process_init(GnuProcess* p, const char* path, int argc, const char* const* argv, const char* const* env,
                     const char* sysroot, char* error, size_t cap) {
    GnuJit jit = p->jit; /* initialized by the caller */
    char guest[4096];
    unsigned char* image = NULL;
    size_t size = 0;
    if (cap) error[0] = 0;
    memset(p, 0, sizeof *p);
    p->jit = jit;
    p->vfork_fd = -1;
#ifdef _WIN32
    p->pid = p->tid = (uint32_t)GetCurrentProcessId();
#else
    p->pid = p->tid = (uint32_t)getpid();
#endif
    int r = gnu_vfs_init(&p->vfs);
    if (!r && sysroot) r = gnu_vfs_set_root(&p->vfs, sysroot);
    if (r) {
        snprintf(error, cap, "Cannot create the guest filesystem (%d)", r);
        gnu_vfs_destroy(&p->vfs);
        return r;
    }
    image = snx_read_file(path, &size);
    if (!image) {
        snprintf(error, cap, "Cannot read '%s'", path);
        gnu_vfs_destroy(&p->vfs);
        return -GNU_ENOENT;
    }
    if (sysroot) {
        /* the program's guest path: inside the sysroot without its prefix, the host's path otherwise, as QEMU has it */
        char* absolute = snx_absolute(path);
        r = gnu_hostfs_program_path(&p->vfs, absolute ? absolute : path, guest, sizeof guest);
        free(absolute);
    } else
        strcpy(guest, "/program");
    if (!r) r = gnu_process_load(p, guest, image, size, argc, argv, env, error, cap);
    free(image);
    if (!r) r = gnu_net_process_init(p);
    if (r) {
        if (cap && !error[0]) snprintf(error, cap, "Guest process initialization failed (%d)", r);
        gnu_process_destroy(p);
        p->jit = jit;
    }
    return r;
}
void gnu_process_destroy(GnuProcess* p) {
    gnu_vfs_destroy(&p->vfs);
    gnu_elf_destroy(&p->elf);
    gnu_elf_destroy(&p->interp);
    gnu_memory_destroy(&p->memory);
}
void gnu_process_exit(GnuProcess* p, int status) {
#ifdef _WIN32
    /* a vfork child that never exec'd: its parent goes on, and wait4 finds the status */
    if (p->vfork_parent) gnu_spawn_win32_child_exit(p, status);
#endif
    if (p->clear_tid) {
        uint32_t zero = 0;
        (void)gnu_memory_write(&p->memory, p->clear_tid, &zero, 4);
    }
    p->exited = 1;
    p->exit_code = status & 255;
    if (p->running) longjmp(p->exit_target, 1);
}
void gnu_process_fault(GnuProcess* p, int signal, const char* reason) {
    p->faulted = signal;
    snprintf(p->diagnostic, sizeof p->diagnostic, "%s", reason);
    gnu_process_exit(p, 128 + signal);
}
