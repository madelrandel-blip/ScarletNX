/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The VIC shader's parameters (vic_params.glsl, which GLSL and C share) and vic.c's packing of a job into them */
#ifndef SNX_VIC_H
#define SNX_VIC_H
#include "nx/gpu/gpu.h"

typedef struct VicGpuPlane VicGpuPlane;
typedef struct VicGpuSlot VicGpuSlot;
typedef struct VicGpuJob VicGpuJob;
#define uint u32
#include "nx/gpu/video/vic_params.glsl"
#undef uint
/* the uniform buffer, as vic.comp declares it */
typedef struct {
    VicGpuJob job;
    VicGpuSlot slots[8];
} VicGpuParams;
_Static_assert(sizeof(VicGpuPlane) == 32 && sizeof(VicGpuSlot) == 176 && sizeof(VicGpuJob) == 128,
               "vic_params.glsl's structs are not std140's");

/* the shader's parameters for job j: in_base[2s] and [2s + 1] are slot s's luma and chroma planes' byte offsets in the input
   buffer, oc_base the output chroma's in the output buffer (the luma, or RGBA, plane starts it). 0 when the shader
   can't compose the job (a matrix slot whose soft clamp has lo above hi, or a slot outside the target rectangle,
   which slot_resolve never leaves), which the CPU then does */
int vic_gpu_params(const VicJob* j, VicGpuParams* p, const u32 in_base[16], u32 oc_base);
/* 1 when every plane of j keeps word aligned columns word aligned, for the shader's build that needs it (vic.comp);
   0 for the -DVIC_UNALIGNED one */
int vic_gpu_aligned(const VicJob* j);

#endif
