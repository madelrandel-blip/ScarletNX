/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's startup and the state it sets up for the whole run (main.c), the crash report (crash.c), and showing
   vi's buffers at the game's pace (present.c) */
#ifndef SNX_MAIN_H
#define SNX_MAIN_H
#include "common/cpu/aarch64/cpu.h"

int snx_main(int argc, char** argv); /* runs the game the arguments or the program's name say; its exit status */
/* the launcher's menu (src/gui), for an executable built with it: a bare start of the runtime opens it, with the
   program's own path */
extern int (*snx_menu)(const char* self);
/* Saves what outlives the run (the hints, the renderer's pipeline cache) and reports SNX_NVN_TRACE before a clean exit:
   the main loop's, the unattended limit's, svcExitProcess. The first caller saves; one that comes while it does waits,
   then does nothing */
void snx_save_before_exit(void);
/* The game's folders, each ending in a separator (g_exedir may be empty), and its name: <g_exedir><g_exename>_code.so
   is its code, <g_exedir><g_exename>_data/ (g_datadir) its files */
extern char g_datadir[1024], g_exename[256], g_exedir[1024];
/* where its settings (config), its log and threads.txt (state) and its caches go: g_exedir, or for a game in an
   installed build's data folder the XDG folders of each kind */
extern char g_confdir[1024], g_statedir[1024], g_cachedir[1024];
extern volatile int g_quit; /* set to leave the main loop, which saves and exits */
extern int g_frames;        /* the frames shown so far, atomic: the presenting thread counts them */
/* the crash handler's report (plat_posix.c, plat_win32.c): the faulting host pc, the address it touched (NULL when not
   a memory fault) and the signal, or the Windows exception code. Ends the process */
void crash_report(void* host_pc, void* fault, int sig);
/* one of vi's queued buffers on the display, once the GPU has drawn it (the GPU thread): nvmap_id's memory from offset,
   its layout, format and size, the crop rectangle (left, top, right, bottom; all 0 for the whole buffer) and the
   transform's flips (bit 0 horizontal, bit 1 vertical) */
void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform);
/* the wait before vi hands the game a buffer for its next frame, as fps_limit says: with fps_limit=game until the
   frame before has been on the display for interval refreshes (its swap interval) */
void present_pace(u32 interval);
/* a buffer in guest memory at a as RGBA8 into out (w * h words), for one no render target holds */
void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out);

#endif
