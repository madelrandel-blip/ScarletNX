/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU front end: the channels and their engines' registers, guest memory as the GPU sees it, the GPFIFO queue
   and its GPU thread, and the structures the 2D engine and VIC hand the renderer */
#ifndef SNX_GPU_H
#define SNX_GPU_H
#include "common/cpu/aarch64/cpu.h"

/* A method batch of the pushbuffer in progress. A header's count can run past the end of its GPFIFO entry into the
   next one, so the batch belongs to the channel, not to the entry */
typedef struct {
    u32 mode; /* the header's mode while words are owed: 1 incrementing, 3 non-incrementing, 5 incrementing once */
    u32 sub;  /* its subchannel */
    u32 m;    /* the method the next word goes to */
    u32 cnt;  /* the words still owed */
} PbBatch;

/* The macro engine (mme.c) of a channel, kept between steps: a method a macro sends can make the stream wait
   (gpu_park), and the macro goes on from here (mme_resume) once the wait ends */
typedef struct {
    u32 regs[8];     /* r0 reads as 0 */
    u32 carry;       /* of the last add or subtract */
    u32 method;      /* where the next send goes */
    u32 incr;        /* how far the method moves after a send */
    u32 pc;          /* the next instruction, a word index into the channel's macro memory */
    u32 target;      /* where a taken branch goes once its delay slot has run */
    u32 np, next;    /* the parameters of the call (in the channel's mme_params) and the next one to fetch */
    u32 steps;       /* instructions run in this call, against a macro that never ends */
    int running;     /* a call started and not ended: a wait stopped it */
    int branch;      /* a branch was taken: after the next instruction, its delay slot, pc becomes target */
    int slot;        /* the next instruction is a delay slot, whose exit bit doesn't count */
    int exiting;     /* the call ends after the next instruction, the exit's delay slot */
} GpuMme;

/* Inline-to-memory (I2M, the methods at 0x60-0x6D of the 3D, compute and inline-to-memory classes): a launch's
   destination, and the bytes the data words have brought so far */
typedef struct {
    u8* buf;
    u32 cap;      /* bytes buf has room for */
    u32 len, pos; /* the bytes the launch wants, and those that came */
    u32 line, lines, pitch;
    u32 bh, bd;   /* block height and depth (log2 GOBs) of a block linear destination */
    u32 w, h, z;  /* its width in bytes, height and slice */
    u32 x, y;     /* the origin, x in bytes */
    int linear;
    u64 addr;
} GpuI2m;

/* A GPU channel: an nvhost-gpu fd's state, named by its syncpoint id. It is made at the channel's first submission
   and dropped when nvdrv closes it (gpu_channel_close) */
typedef struct GpuChannel {
    u32 id;
    u32 sub_class[8]; /* the class bound to each subchannel (puller method 0) */
    PbBatch pb;
    u32 puller[0x40];
    /* the engines' registers, by method */
    u32 r3d[0x1000]; /* the 3D engine's. 0xE00 up are the macro calls' */
    u32 r3d_shadow[0xE00]; /* the MME shadow RAM (SET_MME_SHADOW_RAM_CONTROL, 3D 0x49) */
    u32 shadow_mode;       /* 0 or 1 track, 2 passthrough, 3 replay */
    u32 rcomp[0x1000];
    u32 r2d[0x400];
    u32 rdma[0x200];
    u32 ri2m[0x80];
    GpuI2m i2m;
    /* macros: the code and start addresses uploaded (3D 0x45-0x48), and the call the stream is collecting the
       parameters of (0xE00 up): a call runs at its batch's last word */
    u32 mme_code[0x2000];
    u32 mme_start[0x80];
    /* a macro's native implementation is chosen by the hash of its code from its start to the end of the upload batch
       that wrote it (mme.c): the batch each code word came in (0 never uploaded; batches count 0x45 writes), and per
       slot 0 when not chosen since the code or the starts changed, 1 for the interpreter, else a native one's index + 2 */
    u32 mme_batch;
    u32 mme_code_batch[0x2000];
    u8 mme_choice[0x80];
    u64 mme_hash[0x80]; /* the hash mme_choose found for each slot, for the interpreted calls' statistics */
    u32 mme_upload, mme_start_ptr;
    u32 mme_params[0x10000];
    u32 mme_nparams;
    int mme_collecting;
    u32 mme_index;
    GpuMme mme; /* the macro running, or one a wait stopped after the method that asked for it */
    /* constant buffer data (3D 0x8E4-0x8F3) a batch is still sending: written to memory in one go */
    u64 cb_addr;
    u32 cb_n;
    u32 cb_words[256];
    /* the constant buffers bound to each stage (3D 0x904 + 8 * stage), address and size, 0 when unbound. The renderer
       reads the drawing channel's: another channel binds its own */
    u64 cbind[5][18];
    u32 cbind_size[5][18];
    int render_off; /* SetRenderEnable (3D 0x554-0x556) or its override (0x651) turned drawing off */
    /* counter reports (3D 0x6C3 operation 2): g_draws at each selector's last reset, and the selectors reset */
    u64 counter_base[32];
    u32 counter_reset;
    u32 instance_count; /* the instance the next draw is: BEGIN's next-instance bit and the SUBSEQUENT methods count */
    u32* inline_indices; /* the 3D inline index methods' indices, widened to u32, until END draws them */
    u32 inline_n, inline_cap;
    /* the copy engine's launches of the job running: a submission-time copy of a linear source belongs to one */
    u64 dma_job_id;
    u32 dma_launch_index;
    /* what the stream waits for before its next method (gpu_park). The GPU thread runs other channels meanwhile, as
       the GPU's host interface (PFIFO) switches channels on an acquire that fails */
    u32 park;      /* GPU_PARK_*, or 0 */
    u32 park_val;  /* the value compared with, or the syncpoint's threshold */
    int park_geq;  /* reach park_val (a wrapping compare) instead of equalling it */
    u64 park_addr; /* the semaphore's GPU address, or the syncpoint id */
    u64 park_end;  /* the plat_ns() past which the stream goes on anyway */
} GpuChannel;
enum {
    GPU_PARK_SEMAPHORE = 1, /* the puller's SemaphoreOperation acquire */
    GPU_PARK_REPORT,        /* the 3D or compute engine's report semaphore acquire */
    GPU_PARK_SYNCPOINT,     /* the puller's syncpoint wait */
    GPU_PARK_FENCE          /* a submission's fence, waited for before its first entry */
};
typedef struct {
    GpuChannel* chan[128]; /* by syncpoint id: nvdrv hands out ids below 128 */
    int nchan;
} GpuState;
extern GpuState g_gpu;

/* a 2D engine surface, the source or destination of a blit (2D 0x80-0x89 and 0x8C-0x95) */
typedef struct {
    u32 format, linear, bh, depth, layer, pitch, width, height;
    u64 addr;
} Surf2D;

/* the VIC composition (gpu/video/vic.c): a surface plane as VIC addresses it (pitch linear rows or block linear
   GOBs), a slot's resolved rectangles and color matrix, and a job's planes and slots. The renderer's vic_compose
   runs the composition as a compute shader (gpu/vk/vic.c); when it returns 0 the caller composes in guest memory */
typedef struct VicPlane {
    u8* p;   /* the plane's bytes in guest memory */
    u64 lim; /* bytes p may touch */
    u32 pitch, h, blk, bh;
} VicPlane;
typedef struct VicSlot {
    int on;                               /* the slot composes */
    u32 sx0, sy0, dx0, dy0, w, h;         /* the resolved rectangles: source start, destination start and size */
    int mat[3][4], mat_shift, mat_enable; /* the color matrix, out = (R*c0 + G*c1 + B*c2) >> shift, + c3, >> 8 */
    u32 alpha, clamp_lo, clamp_hi;        /* the planar alpha and the soft clamp range (10 bit) */
    u32 vu; /* input format 0x43 (Y8___U8V8_N420): V in each chroma pair's even byte, U in the odd one */
    VicPlane iy, ic;
} VicSlot;
typedef struct VicJob {
    u32 fmt, sw, sh;
    u32 tx0, ty0, tw, th; /* the target rectangle the background colour fills, in the surface; tw 0: none. The
                             slots lie inside it (vic.c's slot_resolve clips them) */
    u32 bg[4];            /* the background colour: R, G, B, A, 10 bit */
    VicPlane oy, oc;      /* oc.p is NULL when the format has no chroma */
    VicSlot slots[8];
} VicJob;

/* ---- gpu.c: guest memory through the GPU's address space. A read lands the renderer's pending results over the
   range first, a write lets it land what would overwrite the write later, and records the write (gpu_mem_written).
   Unmapped pages read as zero and take no writes; the int calls return -1 when any page was unmapped */
int gmem_read(u64 gva, void* dst, u64 size);
int gmem_write(u64 gva, const void* src, u64 size);
/* gmem_read and gmem_write in halves, for a copy made of many small runs (a block linear row's 16 byte runs): the
   hooks once over the span the runs fall in (gmem_need before reading it, gmem_before_write before writing it and
   gpu_mem_written after), then the runs without them */
void gmem_need(u64 gva, u64 size);
void gmem_read_raw(u64 gva, void* dst, u64 size);
void gmem_before_write(u64 gva, u64 size);
void gmem_write_raw(u64 gva, const void* src, u64 size);
/* a shader program: up to size bytes, stopping at the first page with no guest memory behind it, zeros after */
void gmem_read_code(u64 gva, void* dst, u64 size);
/* the host address of [gva, gva + size) when it is contiguous in guest memory, after landing pending results over
   it; NULL otherwise */
u8* gmem_ptr(u64 gva, u64 size);
/* the same without landing anything: only for comparing a pending result's original guest bytes */
u8* gmem_ptr_untracked(u64 gva, u64 size);
u32 gmem_r32(u64 gva);
void gmem_w32(u64 gva, u32 v);
/* the byte offset of (x_bytes, y, z) in a block linear surface width_bytes wide and height rows high, with blocks of
   2^bh_log2 GOBs high and 2^bd_log2 deep (the caller shrinks them for small surfaces, as the hardware does) */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2);
/* a block linear surface w_bytes x h (one slice) to pitch linear rows of dst_pitch bytes, and back */
void deswizzle(u8* dst, u32 dst_pitch, const u8* src, u32 w_bytes, u32 h, u32 bh_log2);
void swizzle(u8* dst, const u8* src, u32 src_pitch, u32 w_bytes, u32 h, u32 bh_log2);
/* an inline-to-memory launch from an engine's registers (regs[base] is LINE_LENGTH_IN, regs[base + 0xC] the launch),
   and its data words */
void i2m_launch(GpuChannel* ch, u32* regs, int base);
void i2m_data(GpuChannel* ch, u32 v);
u32 gpu_color_bpp(u32 format); /* bytes per pixel of a color format (render targets and 2D surfaces share them) */
void gpu_signal_point(void);   /* the GPU is about to do something the game can observe */
/* a semaphore or report payload the game can poll: it happens once everything before it in the stream is done */
void gpu_release_write(u64 gva, const void* data, u32 len);

/* the engines' methods (engines.c, compute.c, fermi2d.c, dma.c): m is the method, last marks a batch's last word */
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last);
void compute_method(GpuChannel* ch, u32 m, u32 v, int last);
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v);
void fermi2d_method(GpuChannel* ch, u32 m, u32 v);
void dma_method(GpuChannel* ch, u32 m, u32 v);
/* dma.c: the linear source of a LAUNCH_DMA (v) with registers r, read now for the channel's copy to use when it runs:
   called in the channel's submit order, for every launch the stream will run (gpu_submit) */
void dma_snap_launch(u32 channel, u64 job_id, u32 launch_index, const u32* r, u32 v);
void dma_snap_finish(u32 channel, u64 job_id); /* the job ended: what it left goes */
void dma_snap_reset(u32 channel);              /* the channel closed: what it queued goes */

/* maxwell3d.c: the record of GPU-side writes. Each write stamps a new generation on the pages it covers, by GPU
   address and by the guest pages behind them */
void gpu_mem_written(u64 gva, u64 size);
void gpu_mem_written_shader(u64 gva, u64 size); /* a shader's result landing in guest memory */
u64 gpu_mem_generation(void);                   /* the last generation fully published */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz); /* a GPU-side write after since touched [a, a + sz) */

/* ---- gpu.c: GPFIFO submission and the GPU thread's queue */
void gpu_init(void);
/* queues GPFIFO entries for the GPU thread, which waits for the fence (wait_id ~0: none) as the stream's first
   method, runs them on channel (the channel's syncpoint id), then increments the syncpoint incs times, after which it
   should be at expect (the fence nvdrv promised). The entries are u64s at any alignment. A channel's jobs run in
   order, and another channel's may run before them while they wait (gpu_park) */
void gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect);
/* calls fn(arg) on the GPU thread in order with the other calls, once the jobs queued before it on channels
   chans[0..nchans) (n at most 4) are done. Other channels' jobs, queued before or after it, don't hold it back */
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans);
void gpu_channel_close(u32 channel);  /* nvdrv closed the channel: a new one with the same id starts from reset */
void gpu_release_retire(u32 channel); /* complete pending syncpoint releases before nvdrv reuses its id */
/* the channel's stream waits for the u32 at GPU address addr (a syncpoint's id for the syncpoint kinds) to equal val,
   or reach it (geq), before its next method. Met already, nothing waits. Otherwise the method that asked returns and
   the stream stops after it (a macro after the method too, mme_resume), and the GPU thread goes on with other
   channels until it's met */
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq);
/* something a parked channel may wait for changed (a syncpoint increment, on the GPU thread or off it): look again.
   Any thread */
void gpu_kick(void);
extern volatile u64 g_gpu_submits;
/* guest memory [cpu_addr, cpu_addr + len) changed without a write the host's write tracking sees: decommitted pages,
   which now read as zero (memory/mem.c). Caches over that memory check it again. Any thread */
void gpu_mem_changed(u64 cpu_addr, u64 len);
extern void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len); /* the renderer's side of it, set when it starts */
/* the renderer lands pending shader results before the stream goes on past them: before a write of memory those
   results overlap (size > 0), and before anything the game can observe (gpu_signal_point, size 0) */
extern void (*g_gpu_before_write)(u64 gva, u64 size);
extern int g_gpu_releases; /* the renderer holds releases waiting to happen (renderer.h release_need) */
/* the renderer's memory lock held across a change of backing. full is for CPU guest pages, whose GPU aliases are
   unknown; GPU address space changes give the range. Begin is taken before the memory manager's own lock */
extern void (*g_gpu_mapping_begin)(u64 gva, u64 size, int full);
extern void (*g_gpu_mapping_end)(void);

/* ---- engines.c: the 3D engine's methods and the report semaphores */
u64 gpu_ticks(void); /* the GPU's timestamp: 614.4 MHz ticks of the guest clock */
/* draws that differ only in their instance are merged into one instanced draw while they come in. The merged draw
   is sent before anything that could see it is missing (another method, another channel, the stream's end) */
void r3d_pending_flush(void);
int r3d_pending_on(void);
/* mme.c: a macro a wait stopped (ch->mme.running) goes on, until it ends or waits again */
void mme_resume(GpuChannel* ch);

/* ---- maxwell3d.c: the 3D engine's side of the renderer */
void r3d_clear(GpuChannel* ch, u32 v);
/* a clear is cut to scissor 0 when CLEAR_FLAGS (3D 0x43E) has SCISSOR, bit 8, and that scissor is enabled. Bit 4,
   CLEAR_RECT, is another bound (nouveau names the bits; Eden reads the scissor from bit 8 too) */
static inline int r3d_clear_scissored(const u32* r) { return (r[0x43E] & 0x100) && (r[0x380] & 1); }
void r3d_draw_texture(GpuChannel* ch); /* DRAW_TEXTURE (3D 0x420-0x42B) */
/* a draw: indexed 0 none, 1 the index buffer, 3 the channel's inline indices */
void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances);
void r3d_cb_bind(GpuChannel* ch, int stage, u32 v); /* records the binding in ch->cbind */
void r3d_const_upload(GpuChannel* ch, u32 v, int last);
void r3d_const_flush(GpuChannel* ch); /* writes the constant buffer data r3d_const_upload is holding */
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy);
#endif
