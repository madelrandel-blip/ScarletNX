/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Written from the Arm Architecture Reference Manual for A-profile (DDI 0487) pseudocode, with the bitfield,
   division and exclusive-monitor rules cross-checked against QEMU (target/arm/tcg/translate-a64.c,
   helper-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The guest CPU of a game module (a Cortex-A57 at EL0) and the small helpers generated code and the interpreter
   inline. Every module and its runtime library include it, and the emitters reach the fields by offsetof, so the
   layout below is the binary contract that SNX_ABI versions. */
#ifndef SNX_CPU_H
#define SNX_CPU_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;

#define RESTRICT    __restrict__
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define SNX_INLINE static inline __attribute__((always_inline))
#else /* C90 has no inline: unused copies must not warn in every file that includes this one */
#define SNX_INLINE static __attribute__((always_inline, unused))
#endif
/* a 64-bit constant from two 32-bit halves, since C90 has no long long literals */
#define SNX_U64C(hi, lo) (((u64)(hi) << 32) | (u64)(lo))

/* The version of the contract between a game module and the runtime: this header (the CPU layout, the SNX_FP_*
   numbers), the helpers in cpu_helpers.c, RtAPI and the module descriptor. The recompiler writes it into each
   module and the runtime refuses any other. Bump it for any change of layout or of a helper's results.
   7: CPU.poll and RtAPI.poll, read on the backward branches of generated code.
   8: the FP/SIMD emitters and discovery changes.
   9: RtAPI.svcs, which the NX modules call each SVC through by its number.
  10: RtAPI.miss replaces the module's own snx_interp_entry as snx_lookup's fallback, and CPU.disp's slots are read
      with relaxed atomic loads, since the runtime JIT fills them while guest threads run. */
#define SNX_ABI 10

#if defined(_WIN32)
#define SNX_EXPORT __declspec(dllexport)
#else
#define SNX_EXPORT __attribute__((visibility("default")))
#endif

/* one 128-bit vector register, lane 0 in the lowest bytes (the host is little-endian, as the guest) */
typedef union V128 {
    u8 b[16];
    i8 sb[16];
    u16 h[8];
    i16 sh[8];
    u32 s[4];
    i32 ss[4];
    u64 d[2];
    i64 sd[2];
    f32 f[4];
    f64 df[2];
} V128;

#include "common/runtime/interface.h"
typedef void (*GuestFn)(struct CPU* RESTRICT c, u64 entry);

typedef struct CPU {
    u64 x[32];        /* X0-X30, and x[31] is SP: the zero register never reaches the state */
    u64 pc;
    u32 n, z, cf, vf; /* PSTATE.NZCV, one 0/1 field each */
    V128 v[32];
    u32 fpcr, fpsr;
    u64 tpidr, tpidrro; /* TPIDR_EL0, TPIDRRO_EL0 (the thread's TLS) */
    /* the exclusive monitor: armed by a load-exclusive with its address, size (pairs or 0x10 into it) and the
       value(s) it read */
    u64 excl_addr, excl_val, excl_val2;
    u32 excl_size;
    u32 excl_on;
    u8* mem;       /* the host address of guest address 0 */
    GuestFn* disp; /* the function of each code word: disp[(pc - code_lo) >> 2], 0 for none */
    u64 code_lo, code_hi;
    const RtAPI* rt;
    void* thread; /* the runtime thread or process this CPU belongs to */
    /* written by another host thread when the runtime wants this thread's core back (a thread of higher priority
       waits for it): generated code reads it on each backward branch, the interpreter on each taken branch, and then
       calls rt->poll */
    u32 poll;
} CPU;

/* The emitters bake these offsets into generated code, so modules built before a layout change would read the
   wrong fields: a failed check here means SNX_ABI must change too. The fields after mem move with the pointer size,
   and are checked on 64-bit hosts, the only ones games run on. */
typedef char snx_check_v128[sizeof(V128) == 16 ? 1 : -1];
typedef char snx_check_cpu_x[offsetof(CPU, x) == 0 ? 1 : -1];
typedef char snx_check_cpu_pc[offsetof(CPU, pc) == 256 ? 1 : -1];
typedef char snx_check_cpu_nzcv[offsetof(CPU, n) == 264 && offsetof(CPU, z) == 268 && offsetof(CPU, cf) == 272 &&
                                        offsetof(CPU, vf) == 276
                                    ? 1
                                    : -1];
typedef char snx_check_cpu_v[offsetof(CPU, v) == 280 ? 1 : -1];
typedef char snx_check_cpu_fp[offsetof(CPU, fpcr) == 792 && offsetof(CPU, fpsr) == 796 ? 1 : -1];
typedef char snx_check_cpu_tls[offsetof(CPU, tpidr) == 800 && offsetof(CPU, tpidrro) == 808 ? 1 : -1];
typedef char snx_check_cpu_excl[offsetof(CPU, excl_addr) == 816 && offsetof(CPU, excl_val) == 824 &&
                                        offsetof(CPU, excl_val2) == 832 && offsetof(CPU, excl_size) == 840 &&
                                        offsetof(CPU, excl_on) == 844
                                    ? 1
                                    : -1];
typedef char snx_check_cpu_mem[offsetof(CPU, mem) == 848 ? 1 : -1];
typedef char snx_check_cpu_tail[sizeof(void*) != 8 || (offsetof(CPU, disp) == 856 && offsetof(CPU, code_lo) == 864 &&
                                                       offsetof(CPU, code_hi) == 872 && offsetof(CPU, rt) == 880 &&
                                                       offsetof(CPU, thread) == 888 && offsetof(CPU, poll) == 896 &&
                                                       sizeof(CPU) == 904)
                                    ? 1
                                    : -1];

/* ---- guest memory: a flat mapping at M, no translation ---- */
#define MP(a)       (M + (u64)(a))
#define RD8(a)      (*(u8*)MP(a))
#define RD16(a)     (*(u16*)MP(a))
#define RD32(a)     (*(u32*)MP(a))
#define RD64(a)     (*(u64*)MP(a))
#define RDS8(a)     (*(i8*)MP(a))
#define RDS16(a)    (*(i16*)MP(a))
#define RDS32(a)    (*(i32*)MP(a))
#define WR8(a, v)   (*(u8*)MP(a) = (u8)(v))
#define WR16(a, v)  (*(u16*)MP(a) = (u16)(v))
#define WR32(a, v)  (*(u32*)MP(a) = (u32)(v))
#define WR64(a, v)  (*(u64*)MP(a) = (u64)(v))
#define RDQ(dst, a) memcpy(&(dst), MP(a), 16)
#define WRQ(a, src) memcpy(MP(a), &(src), 16)

/* ---- indirect calls: the function compiled for a pc, or the runtime JIT for one discovery missed ---- */
/* A slot goes from NULL to a function while other threads read it (the runtime JIT publishes with release ordering,
   gpu/nvn_trace.c installs wrappers): a relaxed load, which costs a plain one, makes that no data race. The code and
   data behind a published address were written before the release store, and reaching them depends on the loaded
   address, which orders them on every host the backends target */
SNX_INLINE GuestFn snx_lookup(CPU* c, u64 pc);
SNX_INLINE GuestFn snx_lookup(CPU* c, u64 pc) {
    u64 offset = pc - c->code_lo;
    GuestFn fn;
    if (UNLIKELY(offset >= c->code_hi - c->code_lo || (pc & 3))) return c->rt->miss;
    fn = __atomic_load_n(&c->disp[offset >> 2], __ATOMIC_RELAXED);
    return LIKELY(fn != 0) ? fn : c->rt->miss;
}
#define CALLDYN(t)                                                                                                     \
    do {                                                                                                               \
        u64 t__ = (t);                                                                                                 \
        snx_lookup(c, t__)(c, t__);                                                                                    \
    } while (0)

/* ---- condition codes (ConditionHolds) ---- */
SNX_INLINE int cond_ok(const CPU* c, u32 cond) {
    int holds;
    switch ((cond >> 1) & 7) {
        case 0: holds = c->z != 0; break;                       /* EQ/NE */
        case 1: holds = c->cf != 0; break;                      /* CS/CC */
        case 2: holds = c->n != 0; break;                       /* MI/PL */
        case 3: holds = c->vf != 0; break;                      /* VS/VC */
        case 4: holds = c->cf != 0 && c->z == 0; break;         /* HI/LS */
        case 5: holds = c->n == c->vf; break;                   /* GE/LT */
        case 6: holds = c->z == 0 && c->n == c->vf; break;      /* GT/LE */
        default: return 1;                                      /* AL, and NV which also always holds */
    }
    return (cond & 1) ? !holds : holds;
}
/* NZCV from a nibble (N in bit 3), and to the register's bits 31:28 */
SNX_INLINE void set_nzcv(CPU* c, u32 nzcv) {
    c->n = (nzcv >> 3) & 1;
    c->z = (nzcv >> 2) & 1;
    c->cf = (nzcv >> 1) & 1;
    c->vf = nzcv & 1;
}
SNX_INLINE u32 get_nzcv(const CPU* c) { return (c->n << 31) | (c->z << 30) | (c->cf << 29) | (c->vf << 28); }

/* ---- AddWithCarry and the flag-setting forms ---- */
SNX_INLINE u64 adds64(CPU* c, u64 a, u64 b, u32 cin) {
    u64 sum = a + b + cin;
    c->n = (u32)(sum >> 63);
    c->z = sum == 0;
    c->cf = cin ? sum <= a : sum < a; /* the unsigned sum overflowed */
    c->vf = (u32)((~(a ^ b) & (a ^ sum)) >> 63); /* same-signed operands, other-signed result */
    return sum;
}
SNX_INLINE u32 adds32(CPU* c, u32 a, u32 b, u32 cin) {
    u32 sum = a + b + cin;
    c->n = sum >> 31;
    c->z = sum == 0;
    c->cf = cin ? sum <= a : sum < a;
    c->vf = (~(a ^ b) & (a ^ sum)) >> 31;
    return sum;
}
/* a - b is AddWithCarry(a, ~b, 1): carry means no borrow */
SNX_INLINE u64 subs64(CPU* c, u64 a, u64 b) {
    u64 diff = a - b;
    c->n = (u32)(diff >> 63);
    c->z = diff == 0;
    c->cf = a >= b;
    c->vf = (u32)(((a ^ b) & (a ^ diff)) >> 63);
    return diff;
}
SNX_INLINE u32 subs32(CPU* c, u32 a, u32 b) {
    u32 diff = a - b;
    c->n = diff >> 31;
    c->z = diff == 0;
    c->cf = a >= b;
    c->vf = ((a ^ b) & (a ^ diff)) >> 31;
    return diff;
}
SNX_INLINE u64 sbcs64(CPU* c, u64 a, u64 b, u32 cin) { return adds64(c, a, ~b, cin); }
SNX_INLINE u32 sbcs32(CPU* c, u32 a, u32 b, u32 cin) { return adds32(c, a, ~b, cin); }
/* ANDS/BICS: N and Z from the result, C and V cleared */
SNX_INLINE void logic64(CPU* c, u64 r) {
    c->n = (u32)(r >> 63);
    c->z = r == 0;
    c->cf = 0;
    c->vf = 0;
}
SNX_INLINE void logic32(CPU* c, u32 r) {
    c->n = r >> 31;
    c->z = r == 0;
    c->cf = 0;
    c->vf = 0;
}

/* ---- shifts: s below the width, except the rotates, which take it modulo the width ---- */
SNX_INLINE u64 ror64(u64 x, u32 s) {
    s &= 63;
    return s ? (x >> s) | (x << (64 - s)) : x;
}
SNX_INLINE u32 ror32(u32 x, u32 s) {
    s &= 31;
    return s ? (x >> s) | (x << (32 - s)) : x;
}
/* the shifted register operand: t is LSL, LSR, ASR, ROR */
SNX_INLINE u64 shf64(u64 x, u32 t, u32 s) {
    if (t == 0) return x << s;
    if (t == 1) return x >> s;
    if (t == 2) return (u64)((i64)x >> s);
    return ror64(x, s);
}
SNX_INLINE u32 shf32(u32 x, u32 t, u32 s) {
    if (t == 0) return x << s;
    if (t == 1) return x >> s;
    if (t == 2) return (u32)((i32)x >> s);
    return ror32(x, s);
}
/* the extended register operand: opt is UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW, SXTX, then LSL #sh */
SNX_INLINE u64 ext64(u64 x, u32 opt, u32 sh) {
    switch (opt & 7) {
        case 0: x &= 0xFF; break;
        case 1: x &= 0xFFFF; break;
        case 2: x &= 0xFFFFFFFFu; break;
        case 4: x = (u64)(i64)(i8)(u8)x; break;
        case 5: x = (u64)(i64)(i16)(u16)x; break;
        case 6: x = (u64)(i64)(i32)(u32)x; break;
        default: break; /* UXTX, SXTX: the whole register */
    }
    return x << sh;
}

/* ---- bit counts and reversals ---- */
SNX_INLINE u32 clz64(u64 x) { return x ? (u32)__builtin_clzll(x) : 64; }
SNX_INLINE u32 clz32(u32 x) { return x ? (u32)__builtin_clz(x) : 32; }
SNX_INLINE u32 popc8(u8 x) { return (u32)__builtin_popcount(x); }
SNX_INLINE u32 bswap32(u32 x) { return __builtin_bswap32(x); }
SNX_INLINE u64 bswap64(u64 x) { return __builtin_bswap64(x); }
/* CLS: the leading bits equal to the sign bit, the sign bit itself not counted */
SNX_INLINE u32 cls64(u64 x) { return clz64(x ^ (u64)((i64)x >> 1)) - 1; }
SNX_INLINE u32 cls32(u32 x) { return clz32(x ^ (u32)((i32)x >> 1)) - 1; }
SNX_INLINE u64 rbit64(u64 x) {
    const u64 m1 = SNX_U64C(0x55555555u, 0x55555555u), m2 = SNX_U64C(0x33333333u, 0x33333333u),
              m4 = SNX_U64C(0x0F0F0F0Fu, 0x0F0F0F0Fu);
    x = ((x & m1) << 1) | ((x >> 1) & m1); /* swap neighbouring bits, then pairs, then nibbles: bytes reversed */
    x = ((x & m2) << 2) | ((x >> 2) & m2);
    x = ((x & m4) << 4) | ((x >> 4) & m4);
    return bswap64(x);
}
SNX_INLINE u32 rbit32(u32 x) { return (u32)(rbit64(x) >> 32); }
/* REV16: the bytes of each halfword swapped */
SNX_INLINE u64 rev16_64(u64 x) {
    const u64 m = SNX_U64C(0x00FF00FFu, 0x00FF00FFu);
    return ((x & m) << 8) | ((x >> 8) & m);
}
/* REV32: the bytes of each word reversed */
SNX_INLINE u64 rev32_64(u64 x) {
    u64 r = bswap64(x);
    return (r << 32) | (r >> 32);
}

/* ---- division: x/0 is 0 and INT_MIN/-1 is INT_MIN, with no trap ---- */
SNX_INLINE u64 udiv64(u64 a, u64 b) { return b ? a / b : 0; }
SNX_INLINE u32 udiv32(u32 a, u32 b) { return b ? a / b : 0; }
SNX_INLINE i64 sdiv64(i64 a, i64 b) {
    if (b == 0) return 0;
    if (b == -1) return (i64)((u64)0 - (u64)a); /* negation wraps INT_MIN to itself, where C's division traps */
    return a / b;
}
SNX_INLINE i32 sdiv32(i32 a, i32 b) {
    if (b == 0) return 0;
    if (b == -1) return (i32)((u32)0 - (u32)a);
    return a / b;
}

/* ---- the high half of a 64x64 product, from 32-bit partial products ---- */
SNX_INLINE u64 umulh64(u64 a, u64 b) {
    u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (u32)lh + (u32)hl; /* below 3 * 2^32: no carry lost */
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}
/* signed: the unsigned product less b for a negative a and less a for a negative b (mod 2^64) */
SNX_INLINE i64 smulh64(i64 a, i64 b) {
    u64 high = umulh64((u64)a, (u64)b);
    if (a < 0) high -= (u64)b;
    if (b < 0) high -= (u64)a;
    return (i64)high;
}

/* ---- UBFM, SBFM, BFM for the interpreter (DecodeBitMasks with immediate false) ----
   wmask is the rotated field of imms+1 ones, tmask the d+1 low ones, d = (imms - immr) mod datasize */
SNX_INLINE u64 bmask(u32 n) { return n >= 64 ? ~(u64)0 : ((u64)1 << n) - 1; }
SNX_INLINE u64 snx_bf_ror(u64 x, u32 s, u32 ds) { return ds == 32 ? ror32((u32)x, s) : ror64(x, s); }
SNX_INLINE u64 ubfm_(u64 src, u32 immr, u32 imms, u32 ds) {
    u64 wmask = snx_bf_ror(bmask(imms + 1), immr, ds), tmask = bmask(((imms - immr) & (ds - 1)) + 1);
    return snx_bf_ror(src, immr, ds) & wmask & tmask;
}
SNX_INLINE u64 sbfm_(u64 src, u32 immr, u32 imms, u32 ds) {
    u64 wmask = snx_bf_ror(bmask(imms + 1), immr, ds), tmask = bmask(((imms - immr) & (ds - 1)) + 1);
    u64 bot = snx_bf_ror(src, immr, ds) & wmask, top = ((src >> imms) & 1) ? bmask(ds) : 0;
    return (top & ~tmask & bmask(ds)) | (bot & tmask);
}
SNX_INLINE u64 bfm_(u64 dst, u64 src, u32 immr, u32 imms, u32 ds) {
    u64 wmask = snx_bf_ror(bmask(imms + 1), immr, ds), tmask = bmask(((imms - immr) & (ds - 1)) + 1);
    u64 bot = (dst & ~wmask) | (snx_bf_ror(src, immr, ds) & wmask);
    return ((dst & ~tmask) | (bot & tmask)) & bmask(ds);
}
/* the logical immediate (DecodeBitMasks with immediate true), replicated to ds bits; 0 for a reserved encoding */
u64 snx_decode_bitmask(u32 N, u32 imms, u32 immr, u32 ds);

/* ---- host instruction set extensions ----
   The helpers are plain C with no host-specific paths. The runtime library (rtlib.c) is compiled without -march,
   since the recompiler that embeds it runs on other machines than the one that built it; the LLVM backend compiles
   it for the CPU each module is built on (snx_ir_rtlib_object), and the generated code uses the host's extensions
   itself (ir.c's snx_ir_host). */

/* ---- CRC32 and CRC32C (castagnoli) of the low bytes (1, 2, 4 or 8) of val ---- */
u32 snx_crc32(u32 acc, u64 val, int bytes, int castagnoli);

/* ---- floating point (helpers/fp.c) ----
   Raw IEEE encodings in and out, with integer arithmetic only: no host floating point, libm or host rounding
   state, so results don't depend on the host. Only c->fpcr is read and only c->fpsr written (its cumulative flags,
   never cleared), so exceptions never reach process-global state. Widths: 32 and 64 for arithmetic, also 16 for
   format conversion. Rounding modes: 0 ties to even, 1 toward +inf, 2 toward -inf, 3 toward zero, 4 ties away,
   5 to odd (FCVTXN). Integer widths are 32 or 64, with 0 <= fbits <= the integer width.
   Generated code passes the operation numbers below as literals: they are ABI. */
enum { SNX_FP_ADD, SNX_FP_SUB, SNX_FP_MUL, SNX_FP_DIV, SNX_FP_MULX, SNX_FP_ABD, SNX_FP_RECPS, SNX_FP_RSQRTS };
enum { SNX_FP_SQRT, SNX_FP_RECPE, SNX_FP_RSQRTE, SNX_FP_RECPX };
typedef char snx_check_fp_ops[SNX_FP_MUL == 2 && SNX_FP_RSQRTS == 7 && SNX_FP_SQRT == 0 && SNX_FP_RECPX == 3 ? 1 : -1];
u64 snx_fp_binary(CPU* c, u64 a, u64 b, int width, int op);
u64 snx_fp_fma(CPU* c, u64 add, u64 a, u64 b, int width); /* add + a * b, rounded once */
u64 snx_fp_unary(CPU* c, u64 bits, int width, int op);
u64 snx_fp_convert(CPU* c, u64 bits, int src_width, int dst_width, int rm);
u64 snx_fp_to_int(CPU* c, u64 bits, int fp_width, int int_width, int uns, int fbits, int rm);
u64 snx_fp_from_int(CPU* c, u64 bits, int int_width, int fp_width, int uns, int fbits);
u32 snx_fp_compare(CPU* c, u64 a, u64 b, int width, int signal); /* the NZCV nibble of FCMP (signal: FCMPE) */
u64 snx_fp_minmax(CPU* c, u64 a, u64 b, int width, int minimum, int numeric);
u64 snx_fp_round(CPU* c, u64 bits, int width, int rm, int exact); /* FRINT*; exact: FRINTX's inexact flag */
u32 urecpe32(u32 x);
u32 ursqrte32(u32 x);

/* ---- saturation of a value to a signed or unsigned bits-wide integer ---- */
SNX_INLINE i64 sat_s(i64 v, int bits) {
    i64 hi = bits >= 64 ? (i64)SNX_U64C(0x7FFFFFFFu, 0xFFFFFFFFu) : (i64)(((u64)1 << (bits - 1)) - 1);
    i64 lo = -hi - 1;
    return v > hi ? hi : v < lo ? lo : v;
}
SNX_INLINE u64 sat_u(i64 v, int bits) {
    u64 hi = bmask((u32)bits);
    if (v < 0) return 0;
    return (u64)v > hi ? hi : (u64)v;
}

/* ---- AdvSIMD helpers out of line (helpers/simd.c, helpers/crypto.c) ---- */
void v_tbl(CPU* c, u32 d, u32 n, u32 len, u32 m, int q, int tbx);
void v_aes(V128* d, const V128* k, int dec);         /* AESE (dec 0), AESD (1): d = round(d ^ k) */
void v_aesmc(V128* d, const V128* s, int inv);       /* AESMC (inv 0), AESIMC (1) */
void v_sha1(CPU* c, u32 op, u32 d, u32 n, u32 m);    /* op 0-3: SHA1C/P/M/SU0; 0x10, 0x11: SHA1H, SHA1SU1 */
void v_sha256(CPU* c, u32 op, u32 d, u32 n, u32 m);  /* op 4-6: SHA256H/H2/SU1; 0x12: SHA256SU0 */
void v_pmull(V128* d, const V128* a, const V128* b, int hi); /* PMULL 1Q (hi: PMULL2) */
u64 v_expand_imm(u32 op, u32 cmode, u32 imm8);       /* AdvSIMDExpandImm */

/* DC ZVA: DCZID_EL0 gives 64-byte blocks */
SNX_INLINE void dc_zva(u8* M, u64 a) { memset(MP(a & ~(u64)63), 0, 64); }

/* ---- exclusives and ordering ----
   Load-exclusive, load-acquire and store-release are sequentially consistent host atomics of the access size
   (LDACQ, STREL), so no later access moves above an acquire load and no earlier one below a release store, which
   plain or volatile accesses don't guarantee once the compiler reorders them. Every DMB, DSB and ISB is a seq_cst
   fence (SNX_FENCE in a64_templates.inc's BARRIER). Nothing else orders guest memory, so plain guest loads and
   stores stay plain host ones, on x86-64, AArch64 and PowerPC64 hosts alike.
   The monitor is value based: a store-exclusive succeeds when the monitor is armed for the same address and size
   and a compare-and-swap from the value the load read succeeds. It is cleared after each store-exclusive. Status
   0 is success. Unlike hardware it sees no ABA and ordinary stores don't clear it (docs/DIVERGENCES.md). */
SNX_INLINE u32 snx_cas64(u64* p, u64 expected, u64 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SNX_INLINE u32 snx_cas32(u32* p, u32 expected, u32 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SNX_INLINE u32 snx_cas16(u16* p, u16 expected, u16 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SNX_INLINE u32 snx_cas8(u8* p, u8 expected, u8 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
/* one atomic 16-byte compare-and-swap of both halves (p[0] the low one): cmpxchg16b on x86-64, lqarx/stqcx. on
   POWER8 */
u32 snx_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1);
#define SNX_FENCE()    __atomic_thread_fence(__ATOMIC_SEQ_CST)
/* ABI 3 made these and snx_cas128 atomic, so modules from before are refused */
#define LDACQ(T, a)    __atomic_load_n((T*)MP(a), __ATOMIC_SEQ_CST)
#define STREL(T, a, v) __atomic_store_n((T*)MP(a), (T)(v), __ATOMIC_SEQ_CST)
/* LDXR/LDAXR of 1 << sz bytes */
SNX_INLINE u64 ldx(CPU* c, u8* M, u64 a, u32 sz) {
    u64 v;
    if (sz == 0)
        v = LDACQ(u8, a);
    else if (sz == 1)
        v = LDACQ(u16, a);
    else if (sz == 2)
        v = LDACQ(u32, a);
    else
        v = LDACQ(u64, a);
    c->excl_addr = a;
    c->excl_val = v;
    c->excl_size = sz;
    c->excl_on = 1;
    return v;
}
/* STXR/STLXR: the status, 0 when stored */
SNX_INLINE u32 stx(CPU* c, u8* M, u64 a, u32 sz, u64 v) {
    u32 stored = 0;
    if (c->excl_on && c->excl_addr == a && c->excl_size == sz) {
        if (sz == 0)
            stored = snx_cas8((u8*)MP(a), (u8)c->excl_val, (u8)v);
        else if (sz == 1)
            stored = snx_cas16((u16*)MP(a), (u16)c->excl_val, (u16)v);
        else if (sz == 2)
            stored = snx_cas32((u32*)MP(a), (u32)c->excl_val, (u32)v);
        else
            stored = snx_cas64((u64*)MP(a), c->excl_val, v);
    }
    c->excl_on = 0;
    return !stored;
}
/* LDXP/LDAXP: a pair of words is one 64-bit access (first register in the low word), a pair of doublewords two */
SNX_INLINE void ldxp(CPU* c, u8* M, u64 a, u32 sz, u64* v1, u64* v2) {
    if (sz == 2) {
        u64 both = LDACQ(u64, a);
        *v1 = (u32)both;
        *v2 = both >> 32;
        c->excl_val = both;
        c->excl_val2 = 0;
    } else {
        *v1 = LDACQ(u64, a);
        *v2 = LDACQ(u64, a + 8);
        c->excl_val = *v1;
        c->excl_val2 = *v2;
    }
    c->excl_addr = a;
    c->excl_size = sz | 0x10;
    c->excl_on = 1;
}
/* STXP/STLXP: one 64-bit or one 16-byte compare-and-swap */
SNX_INLINE u32 stxp(CPU* c, u8* M, u64 a, u32 sz, u64 v1, u64 v2) {
    u32 stored = 0;
    if (c->excl_on && c->excl_addr == a && c->excl_size == (sz | 0x10)) {
        if (sz == 2)
            stored = snx_cas64((u64*)MP(a), c->excl_val, (u64)(u32)v1 | ((u64)v2 << 32));
        else
            stored = snx_cas128((u64*)MP(a), c->excl_val, c->excl_val2, v1, v2);
    }
    c->excl_on = 0;
    return !stored;
}

/* ---- system registers EL0 reaches, by o0:op1:CRn:CRm:op2 (the MRS/MSR word's bits 19:5) ---- */
u64 snx_mrs(CPU* c, u32 sysreg);
void snx_msr(CPU* c, u32 sysreg, u64 v);

/* ---- what the templates' C uses ---- */
#define RX(n) ((n) == 31 ? (u64)0 : c->x[n])
#define SETX(n, e)                                                                                                     \
    do {                                                                                                               \
        u64 t__ = (e);                                                                                                 \
        if ((n) != 31) c->x[n] = t__;                                                                                  \
    } while (0)
/* a scalar FP/SIMD result: the low 32, 64 or 16 bits of Vd, the rest of the register zeroed */
#define SETSB(D_, E_)                                                                                                  \
    do {                                                                                                               \
        u32 t__ = (u32)(E_);                                                                                           \
        V128* p__ = &c->v[D_];                                                                                         \
        p__->d[1] = 0;                                                                                                 \
        p__->d[0] = t__;                                                                                               \
    } while (0)
#define SETDB(D_, E_)                                                                                                  \
    do {                                                                                                               \
        u64 t__ = (u64)(E_);                                                                                           \
        V128* p__ = &c->v[D_];                                                                                         \
        p__->d[1] = 0;                                                                                                 \
        p__->d[0] = t__;                                                                                               \
    } while (0)
#define SETH(D_, E_)                                                                                                   \
    do {                                                                                                               \
        u16 t__ = (u16)(E_);                                                                                           \
        V128* p__ = &c->v[D_];                                                                                         \
        p__->d[1] = 0;                                                                                                 \
        p__->d[0] = t__;                                                                                               \
    } while (0)
#define SETHB(d, e) SETH(d, e)
#define FPRM        ((int)((c->fpcr >> 22) & 3)) /* FPCR.RMode */
#define CBAR()      __asm__ __volatile__("" ::: "memory")
/* YIELD marks spin-wait loops. The compiler barrier makes such a loop reload guest memory (a loop with no call
   could otherwise keep it in registers), and x86's pause tells the CPU the loop spins, which frees the sibling
   hyperthread and avoids a pipeline flush when the loop ends. POWER's yield hint (or 27,27,27) lowers the hardware
   thread's priority the same way */
#if defined(__x86_64__)
#define YIELD()                                                                                                        \
    do {                                                                                                               \
        __builtin_ia32_pause();                                                                                        \
        CBAR();                                                                                                        \
    } while (0)
#elif defined(__powerpc64__)
#define YIELD() __asm__ __volatile__("or 27,27,27" ::: "memory")
#else
#define YIELD() CBAR()
#endif
/* FMOV (immediate): VFPExpandImm of imm8 = a:b:cdefgh to a single or double */
SNX_INLINE u32 vfp_imm32(u32 i) {
    u32 b = (i >> 6) & 1;
    u32 exponent = ((b ^ 1) << 7) | (b ? 0x7Cu : 0) | ((i >> 4) & 3); /* NOT(b):Replicate(b,5):cd */
    return ((i >> 7) & 1) << 31 | exponent << 23 | (i & 15) << 19;
}
SNX_INLINE u64 vfp_imm64(u32 i) {
    u64 b = (i >> 6) & 1;
    u64 exponent = ((b ^ 1) << 10) | (b ? 0x3FCu : 0) | ((i >> 4) & 3); /* NOT(b):Replicate(b,8):cd */
    return (u64)((i >> 7) & 1) << 63 | exponent << 52 | (u64)(i & 15) << 48;
}
/* every AdvSIMD, crypto and SIMD load/store word: 0 for an unallocated one (UNDEFINED) */
int snx_simd(CPU* c, u8* M, u32 w);
#endif
