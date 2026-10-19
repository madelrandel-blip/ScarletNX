/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tex_upload (vk/texture.c) from guest memory into the staging buffer, read back region by region against a walk of
   the block-linear layout byte by byte (bl_offset below): a level whose rows are whole 16-byte pieces, one whose rows
   end inside a piece, a 3D level with blocks of slices, mip levels, a level read through gmem_read, a pitch-linear
   level, and a converted format, which still goes through its packed rows. A format the GPU takes as it
   is goes into the staging buffer without a buffer of the level's size of its own. Levels past the allocation cap go
   in bands of rows of a slice, each region checked where it lands, and an ASTC level's block rows reach
   vk_astc_upload strip by strip as the layout has them */
#include <stdlib.h>
#include <string.h>
static size_t g_watch, g_big; /* allocations of g_watch bytes or more, while watched */
static void* counted_malloc(size_t n) {
    if (g_watch && n >= g_watch) g_big++;
    return malloc(n);
}
#define malloc counted_malloc
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* texture.c builds without -Wextra */
#include "nx/gpu/vk/texture.c"
#undef malloc

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ guest memory and the staging ring */
#define GVA   0x100000ull
#define GUEST (4u << 20)
static u8 guest[GUEST];
static u64 guest_end = GVA + GUEST; /* gmem_ptr's view ends here: a level past it reads through gmem_read */
u8* gmem_ptr(u64 gva, u64 size) {
    if (gva < GVA || gva + size > guest_end) return NULL;
    return guest + (gva - GVA);
}
int gmem_read(u64 gva, void* dst, u64 size) {
    for (u64 i = 0; i < size; i++) ((u8*)dst)[i] = gva + i >= GVA && gva + i < guest_end ? guest[gva + i - GVA] : 0;
    return 0;
}
#define STAGE (8u << 20)
static u8 stage[STAGE];
static u64 stage_used;
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) {
    u64 at = (stage_used + align - 1) & ~(align - 1);
    if (at + size > STAGE) return NULL;
    memset(stage + at, 0xA5, size); /* bytes the upload leaves alone show */
    stage_used = at + size;
    *buf = (VkBuffer)(uintptr_t)0x5157;
    *off = at;
    return stage + at;
}
static VkBufferImageCopy regs[1024];
static u32 nregs;
static void VKAPI_CALL copy_to_image(VkCommandBuffer cb, VkBuffer b, VkImage img, VkImageLayout l, uint32_t n,
                                     const VkBufferImageCopy* r) {
    (void)cb, (void)b, (void)img, (void)l;
    for (u32 i = 0; i < n && nregs < 1024; i++) regs[nregs++] = r[i];
}
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = copy_to_image;
PFN_vkCmdBlitImage vkCmdBlitImage;
PFN_vkCmdClearColorImage vkCmdClearColorImage;
PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage;
PFN_vkCmdCopyImage vkCmdCopyImage;
PFN_vkCreateImageView vkCreateImageView;
PFN_vkCreateSampler vkCreateSampler;
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
/* record.c's wrappers, which the frame's command buffer goes through: made at once here */
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, u32 n,
                              const VkBufferImageCopy* r) {
    vkCmdCopyBufferToImage(cb, src, dst, dl, n, r);
}
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageCopy* r) {
    vkCmdCopyImage(cb, src, sl, dst, dl, n, r);
}
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageBlit* r, VkFilter filter) {
    vkCmdBlitImage(cb, src, sl, dst, dl, n, r, filter);
}
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c, u32 n,
                            const VkImageSubresourceRange* r) {
    vkCmdClearColorImage(cb, img, l, c, n, r);
}
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue* c,
                                   u32 n, const VkImageSubresourceRange* r) {
    vkCmdClearDepthStencilImage(cb, img, l, c, n, r);
}

/* ------------------------------------------------ what texture.c reaches otherwise, unused here */
VkState g_vk;
_Atomic int g_cs_any;
u32 g_vk_present;
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    (void)what, (void)file, (void)line;
    return r == VK_SUCCESS;
}
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)0xCB; }
void vk_render_end(void) {}
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage_,
                 VkAccessFlags2 access) {
    (void)cb, (void)s, (void)layout, (void)stage_, (void)access;
}
VkImageAspectFlags vk_aspect(const VkFmt* f) {
    return f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}
/* the ASTC upload asks for its block rows in strips of 3: they land in astc_out, slice after slice */
static u8 astc_out[64 << 10];
int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 w, u32 h, u32 depth, u32 level, u32 layer, VkAstcRows rows,
                   void* ctx) {
    (void)cb, (void)level, (void)layer;
    u32 nx = (w + t->f.bw - 1) / t->f.bw, ny = (h + t->f.bh - 1) / t->f.bh;
    for (u32 z = 0; z < depth; z++)
        for (u32 y = 0; y < ny; y += 3) rows(ctx, z, y, ny - y < 3 ? ny - y : 3, astc_out + ((u64)z * ny + y) * nx * 16);
    return 1;
}
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to) {
    (void)cb, (void)img, (void)aspect, (void)src_stage, (void)src_access, (void)from, (void)dst_stage, (void)dst_access,
        (void)to;
}
void vk_compute_mem_need(u64 a, u64 n) { (void)a, (void)n; }
void vk_depth_unpack(const VkFmt* f, const u8* src, u64 pixels, u8* depth, u8* stencil) {
    (void)f, (void)src, (void)pixels, (void)depth, (void)stencil;
}
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2) {
    (void)img, (void)a, (void)v1, (void)v2;
}
u64 vk_hash(const void* p, u64 size) {
    (void)p;
    return size;
}
int vk_rt_drawn_fresh(Surf* s) {
    (void)s;
    return 0;
}
int vk_rt_drawn_in(u64 lo, u64 hi, Surf** out, int max) {
    (void)lo, (void)hi, (void)out, (void)max;
    return 0;
}
Surf* vk_rt_find(u64 gva) {
    (void)gva;
    return NULL;
}
void vk_rt_note_read(Surf* s) { (void)s; }
u64 vk_sample_hash(u64 gva, u64 len) {
    (void)gva;
    return len;
}
u32 vk_sc(u32 v) { return v ? v : 1; }
VkFormat vk_srgb_as(VkFormat f, int srgb) {
    (void)srgb;
    return f;
}
void vk_surf_add(Surf* s) { (void)s; }
void vk_surf_evict(u32 before) { (void)before; }
Surf* vk_surf_first(u64 gva) {
    (void)gva;
    return NULL;
}
VkFmt vk_tic_format(u32 w0, int srgb) {
    (void)w0, (void)srgb;
    return (VkFmt){0};
}
VkImage vk_unscaled_image(const Surf* s, u32 layers, VmaAllocation* mem) {
    (void)s, (void)layers, (void)mem;
    return VK_NULL_HANDLE;
}
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf) { (void)ii, (void)vf; }
int vk_written_since(const u8* host, u64 len, u32 since) {
    (void)host, (void)len, (void)since;
    return -1;
}
u32 vk_ww_now(void) { return 0; }
u32 vk_ww_epoch(void) { return 0; }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    (void)since, (void)a, (void)sz;
    return 0;
}
u64 gpu_mem_generation(void) { return 0; }
void etc2_decode_image(const u8* src, u32 w, u32 h, int kind, u8* dst) {
    (void)src, (void)w, (void)h, (void)kind, (void)dst;
}
u32 etc2_pixel_bytes(int kind) {
    (void)kind;
    return 4;
}
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo* ii, const VmaAllocationCreateInfo* ai, VkImage* img,
                        VmaAllocation* mem, VmaAllocationInfo* info) {
    (void)a, (void)ii, (void)ai, (void)img, (void)mem, (void)info;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem) {
    (void)ii, (void)img, (void)mem;
    return 0;
}
/* the block linear layout: GOBs of 64 bytes by 8 rows in blocks of 1 << bh by 1 << bd GOBs, the GOB's 16 byte groups
   in their order */
u64 bl_offset(u32 x, u32 y, u32 z, u32 wb, u32 h, u32 bh_log2, u32 bd_log2) {
    u32 gob_w = (wb + 63) / 64, bh = 1u << bh_log2, bd = 1u << bd_log2;
    u32 blocks_h = ((h + 7) / 8 + bh - 1) / bh, gy = y / 8, xb = x % 64, yb = y % 8;
    u64 block = 512ull * bh * bd;
    u64 o = ((u64)(z / bd) * blocks_h * gob_w + (u64)(gy / bh) * gob_w + x / 64) * block +
            ((u64)(z % bd) * bh + gy % bh) * 512;
    return o + (xb / 32) * 256 + (yb / 2) * 64 + ((xb % 32) / 16) * 32 + (yb % 2) * 16 + xb % 16;
}

/* ------------------------------------------------ the cases */
static void fill(void) {
    for (u32 i = 0; i < GUEST; i++) guest[i] = (u8)(i * 7 + (i >> 9) * 13 + 1);
}
static Surf tex(u32 w, u32 h, u32 d3, u32 levels, u32 bpp, u32 bh, u32 bd, int bl) {
    Surf t;
    memset(&t, 0, sizeof t);
    t.gva = GVA;
    t.w = w;
    t.h = h;
    t.layers = 1;
    t.levels = levels;
    t.depth3d = d3;
    t.type = d3 > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    t.f = (VkFmt){.vk = VK_FORMAT_R8G8B8A8_UNORM, .bpp = (u8)bpp, .bw = 1, .bh = 1, .conv = VKC_NONE};
    t.bl = (u32)bl;
    t.bh = bh;
    t.bd = bd;
    t.pitch = bl ? 0 : (w * bpp + 31) & ~31u;
    t.img = (VkImage)(uintptr_t)0x1A6;
    return t;
}
/* the upload, then every region against the layout walked byte by byte where it lands: level l's bytes start after
   the levels before it, as tex_upload lays them out, and its regions cover each of its rows once. bands: the regions
   the base level takes, more than one when it goes in bands */
static void upload_and_check(Surf* t, u64 limit, u32 bands) {
    guest_end = GVA + limit;
    stage_used = 0;
    nregs = 0;
    g_watch = 0;
    CHECK(tex_upload(t));
    guest_end = GVA + GUEST;
    u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1;
    u64 at = 0;
    u32 first = 0;
    for (u32 l = 0; l < t->levels; l++) {
        u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, ld = d3 >> l ? d3 >> l : 1;
        u32 rowb = lw * t->f.bpp, lbh = 0, lbd = 0, covered = 0, n = 0;
        u64 lsz = t->bl ? bl_level_size(t->w, t->h, d3, &t->f, t->bh, t->bd, l, &lbh, &lbd) : (u64)t->pitch * lh * ld;
        for (; first + n < nregs && regs[first + n].imageSubresource.mipLevel == l; n++) {
            const VkBufferImageCopy* r = &regs[first + n];
            u32 y0 = (u32)r->imageOffset.y, z0 = (u32)r->imageOffset.z, rows = r->imageExtent.height;
            CHECK(r->imageOffset.x == 0 && r->imageExtent.width == lw && y0 + rows <= lh &&
                  z0 + r->imageExtent.depth <= ld);
            covered += rows * r->imageExtent.depth;
            const u8* s = stage + r->bufferOffset;
            for (u32 z = z0; z < z0 + r->imageExtent.depth; z++)
                for (u32 y = y0; y < y0 + rows; y++)
                    for (u32 x = 0; x < rowb; x++) {
                        u64 o = t->bl ? bl_offset(x, y, z, rowb, lh, lbh, lbd) : (u64)(z * lh + y) * t->pitch + x;
                        u8 want = at + o < limit ? guest[at + o] : 0;
                        CHECK(s[((u64)(z - z0) * rows + (y - y0)) * rowb + x] == want);
                    }
        }
        CHECK(covered == lh * ld && (l || n == bands));
        first += n;
        at += lsz;
    }
    CHECK(first == nregs);
}

int main(void) {
    fill();
    g_vk.alloc_cap = 256ull << 20;
    /* the video's picture: 1920 wide in 4 byte pixels, rows of whole pieces, 16 GOBs a block */
    Surf a = tex(1920, 136, 1, 1, 4, 4, 0, 1);
    upload_and_check(&a, GUEST, 1);
    /* rows that end inside a piece (27 pixels of 4 bytes, 108 bytes), and its mip levels with their shrunk blocks */
    Surf b = tex(27, 50, 1, 3, 4, 3, 0, 1);
    upload_and_check(&b, GUEST, 1);
    /* a 3D level with blocks of two slices, 2 byte texels: rows of 70 bytes */
    Surf c = tex(35, 20, 4, 1, 2, 1, 1, 1);
    upload_and_check(&c, GUEST, 1);
    /* a level gmem_ptr can't hand out in one piece: read through gmem_read, which gives zeros past the mapping */
    Surf d = tex(64, 64, 1, 1, 4, 2, 0, 1);
    upload_and_check(&d, 3 * 4096 + 100, 1);
    /* pitch linear */
    Surf e = tex(33, 9, 1, 1, 4, 0, 0, 0);
    upload_and_check(&e, GUEST, 1);
    /* no level-sized buffer of its own for a format the GPU takes as it is */
    stage_used = nregs = 0;
    g_big = 0;
    g_watch = 1920 * 136 * 4;
    tex_upload(&a);
    CHECK(g_big == 0);
    /* a converted format still packs its rows first: RGBA4 from 2 byte texels, through level_convert */
    Surf f = tex(16, 8, 1, 1, 2, 0, 0, 1);
    f.f.conv = VKC_RGBA4;
    stage_used = nregs = 0;
    tex_upload(&f);
    CHECK(nregs == 1);
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 16; x++) {
            u32 v = guest[bl_offset(x * 2, y, 0, 32, 8, 0, 0)] | guest[bl_offset(x * 2 + 1, y, 0, 32, 8, 0, 0)] << 8;
            u32 want = (v & 15) * 17 | ((v >> 4) & 15) * 17 << 8 | ((v >> 8) & 15) * 17 << 16 | (v >> 12) * 17 << 24;
            u32 got;
            memcpy(&got, stage + regs[0].bufferOffset + (y * 16 + x) * 4, 4);
            CHECK(got == want);
        }
    /* past a 16 KiB allocation cap, bands of rows of one slice: the picture (7680 byte rows) in 68 bands of 2 rows,
       the pitch-linear level in bands of 13 rows (its second level fits whole), and under a 4 KiB cap the 3D level
       (5600 bytes) slice by slice */
    g_vk.alloc_cap = 16 << 10;
    upload_and_check(&a, GUEST, 68);
    Surf g = tex(300, 40, 1, 2, 4, 0, 0, 0);
    upload_and_check(&g, GUEST, 4);
    g_vk.alloc_cap = 4 << 10;
    upload_and_check(&c, GUEST, 4);
    g_vk.alloc_cap = 16 << 10;
    /* a converted format is converted band by band: 64 rows of 256 converted bytes a band */
    stage_used = nregs = 0;
    Surf h = tex(64, 300, 1, 1, 2, 2, 0, 1);
    h.f.conv = VKC_RGBA4;
    CHECK(tex_upload(&h) && nregs == 5);
    for (u32 i = 0; i < nregs; i++)
        for (u32 y = 0; y < regs[i].imageExtent.height; y++)
            for (u32 x = 0; x < 64; x++) {
                u32 gy = (u32)regs[i].imageOffset.y + y;
                u32 v = guest[bl_offset(x * 2, gy, 0, 128, 300, 2, 0)] | guest[bl_offset(x * 2 + 1, gy, 0, 128, 300, 2, 0)]
                                                                            << 8;
                u32 want = (v & 15) * 17 | ((v >> 4) & 15) * 17 << 8 | ((v >> 8) & 15) * 17 << 16 | (v >> 12) * 17 << 24;
                u32 got;
                memcpy(&got, stage + regs[i].bufferOffset + (y * 64 + x) * 4, 4);
                CHECK(got == want);
            }
    g_vk.alloc_cap = 256ull << 20;
    /* an ASTC level (4x4 blocks of 16 bytes) of 3 slices: its block rows as vk_astc_upload receives them */
    Surf k = tex(60, 37, 3, 1, 16, 1, 1, 1);
    k.f.bw = k.f.bh = 4;
    k.f.conv = VKC_ASTC;
    stage_used = nregs = 0;
    CHECK(tex_upload(&k) && nregs == 0);
    for (u32 z = 0; z < 3; z++)
        for (u32 y = 0; y < 10; y++)
            for (u32 x = 0; x < 15 * 16; x++)
                CHECK(astc_out[((z * 10 + y) * 15) * 16 + x] == guest[bl_offset(x, y, z, 15 * 16, 10, 1, 1)]);
    printf("tex_upload_rows: %u checks passed\n", checks);
    return 0;
}
