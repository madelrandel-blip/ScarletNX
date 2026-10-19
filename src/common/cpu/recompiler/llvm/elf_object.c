/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF64 x86-64, AArch64 and PowerPC64 relocatable objects: parsing, symbol resolution, relocation and the AArch64
   and PowerPC64 veneers for link_elf.c and code_loader.c */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* snprintf */
#include "common/cpu/recompiler/llvm/elf_object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* error, size_t error_size, const char* format, const char* a, const char* b) {
    snprintf(error, error_size, format, a, b);
    return -1;
}

const char* snx_elf_symbol_name(const SnxElfObject* o, const Elf64_Sym* s) {
    if (ELF64_ST_TYPE(s->st_info) == STT_SECTION) return "<section>";
    return o->strtab + s->st_name;
}

const char* snx_elf_section_name(const SnxElfObject* o, size_t index) {
    return (const char*)o->input->data + o->sh[o->eh->e_shstrndx].sh_offset + o->sh[index].sh_name;
}

unsigned snx_elf_ppc_abi(unsigned e_flags) { return (e_flags & 3) == 2 ? SNX_ELF_PPC_ELFV2 : SNX_ELF_PPC_ELFV1; }

unsigned char snx_elf_host_data(void) {
    const uint16_t one = 1;
    unsigned char first;
    memcpy(&first, &one, 1);
    return first ? ELFDATA2LSB : ELFDATA2MSB;
}

int snx_elf_parse(SnxElfObject* o, const SnxLinkInput* input, unsigned* machine, unsigned* abi, char* error,
                  size_t error_size) {
    size_t i;
    unsigned object_abi;
    memset(o, 0, sizeof(*o));
    o->input = input;
    o->eh = (const Elf64_Ehdr*)input->data;
    if (input->size < sizeof(Elf64_Ehdr) || memcmp(o->eh->e_ident, ELFMAG, SELFMAG) ||
        o->eh->e_ident[EI_CLASS] != ELFCLASS64 || o->eh->e_ident[EI_DATA] != snx_elf_host_data() ||
        o->eh->e_type != ET_REL ||
        (o->eh->e_machine != EM_X86_64 && o->eh->e_machine != EM_AARCH64 && o->eh->e_machine != EM_PPC64) ||
        o->eh->e_shentsize != sizeof(Elf64_Shdr) ||
        o->eh->e_shoff + (uint64_t)o->eh->e_shnum * sizeof(Elf64_Shdr) > input->size)
        return fail(error, error_size,
                    "%s: not an x86-64, AArch64 or PowerPC64 ELF relocatable object in this host's byte order%s",
                    input->name, "");
    object_abi = o->eh->e_machine == EM_PPC64 ? snx_elf_ppc_abi(o->eh->e_flags) : 0;
    if (!*machine) {
        *machine = o->eh->e_machine;
        *abi = object_abi;
    }
    if (o->eh->e_machine != *machine) return fail(error, error_size, "%s: another machine's object%s", input->name, "");
    if (object_abi != *abi)
        return fail(error, error_size, "%s: another PowerPC64 ABI's object (ELFv1 and ELFv2)%s", input->name, "");
    o->sh = (const Elf64_Shdr*)(input->data + o->eh->e_shoff);
    /* one block, the 8-byte arrays first */
    o->address = (uint64_t*)calloc(o->eh->e_shnum, 2 * sizeof(uint64_t) + sizeof(int));
    if (!o->address) return fail(error, error_size, "out of memory%s%s", "", "");
    o->position = o->address + o->eh->e_shnum;
    o->kind = (int*)(o->position + o->eh->e_shnum);
    for (i = 0; i < o->eh->e_shnum; ++i) {
        const Elf64_Shdr* s = &o->sh[i];
        o->kind[i] = -1;
        if (s->sh_type != SHT_NOBITS && s->sh_offset + s->sh_size > input->size)
            return fail(error, error_size, "%s: truncated section%s", input->name, "");
        if (s->sh_type == SHT_SYMTAB) {
            o->syms = (const Elf64_Sym*)(input->data + s->sh_offset);
            o->nsyms = s->sh_size / sizeof(Elf64_Sym);
            if (s->sh_link >= o->eh->e_shnum) return fail(error, error_size, "%s: bad symbol table%s", input->name, "");
            o->strtab = (const char*)input->data + o->sh[s->sh_link].sh_offset;
        }
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_flags & SHF_TLS)
            return fail(error, error_size, "%s: thread-local data is not supported%s", input->name, "");
        if (s->sh_type == SHT_INIT_ARRAY || s->sh_type == SHT_FINI_ARRAY || s->sh_type == SHT_PREINIT_ARRAY)
            return fail(error, error_size, "%s: constructors are not supported%s", input->name, "");
        if (s->sh_type == SHT_NOBITS)
            o->kind[i] = SNX_ELF_BSS;
        else if (s->sh_type == SHT_PROGBITS || s->sh_type == SHT_X86_64_UNWIND)
            o->kind[i] = s->sh_flags & SHF_EXECINSTR ? SNX_ELF_TEXT
                         : s->sh_flags & SHF_WRITE   ? SNX_ELF_DATA
                                                     : SNX_ELF_RODATA;
        else if (s->sh_type == SHT_NOTE || s->sh_type == SHT_GROUP)
            continue;
        else
            return fail(error, error_size, "%s: unsupported allocated section type%s", input->name, "");
    }
    if (!o->syms) return fail(error, error_size, "%s: no symbol table%s", input->name, "");
    return 0;
}

void snx_elf_free(SnxElfObject* o) {
    free(o->address);
    o->address = o->position = NULL;
    o->kind = NULL;
}

int snx_elf_collect_globals(const SnxElfObject* objs, size_t count, const SnxLinkNames* loaded, SnxLinkNames* globals,
                            char* error, size_t error_size) {
    size_t i, j, found;
    for (i = 0; i < count; ++i) {
        const SnxElfObject* o = &objs[i];
        for (j = 1; j < o->nsyms; ++j) {
            const Elf64_Sym* s = &o->syms[j];
            int bind = ELF64_ST_BIND(s->st_info);
            const char* name;
            if ((bind != STB_GLOBAL && bind != STB_WEAK) || s->st_shndx == SHN_UNDEF) continue;
            name = snx_elf_symbol_name(o, s);
            if (s->st_shndx == SHN_COMMON) return fail(error, error_size, "%s: common symbol %s", o->input->name, name);
            if (loaded && snx_link_find(loaded, name, &found)) {
                if (bind == STB_WEAK) continue;
                return fail(error, error_size, "duplicate symbol %s (%s defines one already loaded)", name,
                            o->input->name);
            }
            if (snx_link_find(globals, name, &found)) {
                const SnxElfObject* other = &objs[found >> 32];
                if (bind == STB_WEAK) continue;
                if (ELF64_ST_BIND(other->syms[found & 0xFFFFFFFFu].st_info) != STB_WEAK)
                    return fail(error, error_size, "duplicate symbol %s (%s)", name, o->input->name);
            }
            snx_link_insert(globals, name, (i << 32) | j);
        }
    }
    return 0;
}

int snx_elf_definition(const SnxElfObject* objs, const SnxLinkNames* globals, size_t object, size_t index,
                       size_t* def_object, size_t* def_index, char* error, size_t error_size) {
    const SnxElfObject* o = &objs[object];
    const Elf64_Sym* s;
    size_t found;
    if (index >= o->nsyms) return fail(error, error_size, "%s: bad symbol index%s", o->input->name, "");
    s = &o->syms[index];
    if (s->st_shndx != SHN_UNDEF) {
        *def_object = object;
        *def_index = index;
        return SNX_ELF_HERE;
    }
    if (snx_link_find(globals, snx_elf_symbol_name(o, s), &found)) {
        *def_object = found >> 32;
        *def_index = found & 0xFFFFFFFFu;
        return SNX_ELF_HERE;
    }
    return SNX_ELF_ELSEWHERE;
}

uint64_t snx_elf_symbol_address(const SnxElfObject* objs, size_t object, size_t index, int* absolute) {
    const SnxElfObject* o = &objs[object];
    const Elf64_Sym* s = &o->syms[index];
    *absolute = s->st_shndx == SHN_ABS;
    if (*absolute) return s->st_value;
    return o->address[s->st_shndx] + s->st_value;
}

int snx_elf_got_relocation(unsigned machine, unsigned type) {
    if (machine == EM_PPC64) return 0; /* its objects keep their addresses in their own .toc sections */
    if (machine == EM_AARCH64) return type == SNX_A64_ADR_GOT_PAGE || type == SNX_A64_LD64_GOT_LO12_NC;
    return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX;
}

int snx_elf_call_relocation(unsigned machine, unsigned type) {
    if (machine == EM_PPC64) return type == SNX_PPC_REL24;
    if (machine == EM_AARCH64) return type == SNX_A64_CALL26 || type == SNX_A64_JUMP26;
    return type == R_X86_64_PLT32;
}

int snx_elf_absolute_relocation(unsigned machine, unsigned type) {
    if (machine == EM_PPC64) return type == SNX_PPC_ADDR64 || type == SNX_PPC_TOC;
    return type == (machine == EM_AARCH64 ? (unsigned)SNX_A64_ABS64 : (unsigned)R_X86_64_64);
}

unsigned snx_elf_relative_type(unsigned machine) {
    return machine == EM_AARCH64 ? (unsigned)SNX_A64_RELATIVE
           : machine == EM_PPC64 ? (unsigned)SNX_PPC_RELATIVE
                                 : (unsigned)R_X86_64_RELATIVE;
}

void snx_elf_layout(SnxElfObject* objs, size_t count, uint64_t* kind_size, uint64_t* kind_align) {
    size_t i, j, k;
    for (k = 0; k < SNX_ELF_KINDS; ++k) {
        kind_size[k] = 0;
        kind_align[k] = 1;
    }
    for (k = 0; k < SNX_ELF_KINDS; ++k)
        for (i = 0; i < count; ++i) {
            SnxElfObject* o = &objs[i];
            for (j = 0; j < o->eh->e_shnum; ++j) {
                uint64_t align = o->sh[j].sh_addralign ? o->sh[j].sh_addralign : 1;
                if (o->kind[j] != (int)k) continue;
                kind_size[k] = (kind_size[k] + align - 1) / align * align;
                o->position[j] = kind_size[k];
                kind_size[k] += o->sh[j].sh_size;
                if (align > kind_align[k]) kind_align[k] = align;
            }
            if (k == SNX_ELF_TEXT && o->veneers) { /* right after the object's code, in reach of all of it */
                o->island = (kind_size[k] + 3) & ~(uint64_t)3;
                kind_size[k] = o->island + (o->eh->e_machine == EM_PPC64 ? SNX_ELF_PPC_VENEER_SIZE : SNX_ELF_VENEER_SIZE) *
                                               (uint64_t)o->veneers;
            }
        }
}

/* ---- AArch64 instruction fields. Everything in uint64_t: a negative displacement is its two's complement, which
   the masks cut to the field as the instruction wants it, with no signed shift or overflow on the way ---- */

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* the instruction at p with value in its bits from shift, width wide */
static void field(unsigned char* p, uint64_t value, unsigned shift, unsigned width) {
    uint32_t mask = (uint32_t)((((uint64_t)1 << width) - 1) << shift);
    snx_link_put32(p, (get32(p) & ~mask) | ((uint32_t)(value << shift) & mask));
}
/* ADR and ADRP's 21-bit immediate: its low two bits at 29, the rest at 5 */
static void adr_field(unsigned char* p, uint64_t value) {
    field(p, value & 3, 29, 2);
    field(p, value >> 2, 5, 19);
}
static uint64_t page_of(uint64_t address) { return address & ~(uint64_t)0xFFF; }
/* whether value, read as signed, fits a signed field of bits: [-2^(bits-1), 2^(bits-1)) moved up by 2^(bits-1) is
   [0, 2^bits) */
static int fits(uint64_t value, unsigned bits) { return (value + ((uint64_t)1 << (bits - 1))) >> bits == 0; }
/* ADRP's displacement from the page of P to the page of target, in pages: its 21 bits reach 4 GiB either way */
static int adrp(unsigned char* p, uint64_t target, uint64_t P) {
    uint64_t pages = page_of(target) - page_of(P);
    if (!fits(pages, 33)) return SNX_LINK_RANGE;
    adr_field(p, pages >> 12);
    return 0;
}
/* a PC-relative branch's displacement, in instructions, into its field */
static int branch(unsigned char* p, uint64_t displacement, unsigned bits, unsigned shift) {
    if (!fits(displacement, bits + 2)) return SNX_LINK_RANGE;
    if (displacement & 3) return SNX_LINK_MISALIGNED;
    field(p, displacement >> 2, shift, bits);
    return 0;
}

/* the bytes a relocation writes (4 for the types snx_elf_relocate refuses) */
static size_t field_size(unsigned machine, unsigned type) {
    if (type == 0) return 0; /* R_X86_64_NONE, R_AARCH64_NONE, R_PPC64_NONE */
    if (machine == EM_PPC64) switch (type) {
            case SNX_PPC_ENTRY:
            case SNX_PPC_PLTSEQ:
            case SNX_PPC_PLTCALL: return 0;
            case SNX_PPC_ADDR64:
            case SNX_PPC_REL64:
            case SNX_PPC_TOC: return 8;
            case SNX_PPC_REL16:
            case SNX_PPC_REL16_LO:
            case SNX_PPC_REL16_HI:
            case SNX_PPC_REL16_HA:
            case SNX_PPC_ADDR16_LO:
            case SNX_PPC_ADDR16_HI:
            case SNX_PPC_ADDR16_HA:
            case SNX_PPC_ADDR16_LO_DS:
            case SNX_PPC_TOC16:
            case SNX_PPC_TOC16_LO:
            case SNX_PPC_TOC16_HI:
            case SNX_PPC_TOC16_HA:
            case SNX_PPC_TOC16_DS:
            case SNX_PPC_TOC16_LO_DS: return 2;
            default: return 4;
        }
    if (machine == EM_AARCH64) return type == SNX_A64_ABS64 || type == SNX_A64_PREL64 ? 8 : 4;
    switch (type) {
        case R_X86_64_64:
        case R_X86_64_PC64:
        case R_X86_64_GOTOFF64:
        case R_X86_64_GOTPC64: return 8;
        default: return 4;
    }
}

static int relocate_x86(unsigned char* p, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t got,
                        uint64_t slot) {
    switch (type) {
        case R_X86_64_NONE: return 0;
        case R_X86_64_64: snx_link_put64(p, S + (uint64_t)A); return 0;
        case R_X86_64_PC32:
        case R_X86_64_PLT32: return snx_link_rel32(p, S + (uint64_t)A, P);
        case R_X86_64_PC64: snx_link_put64(p, S + (uint64_t)A - P); return 0;
        case R_X86_64_GOTPCREL:
        case R_X86_64_GOTPCRELX:
        case R_X86_64_REX_GOTPCRELX: return snx_link_rel32(p, slot + (uint64_t)A, P);
        case R_X86_64_GOTOFF64: snx_link_put64(p, S + (uint64_t)A - got); return 0;
        case R_X86_64_GOTPC32: return snx_link_rel32(p, got + (uint64_t)A, P);
        case R_X86_64_GOTPC64: snx_link_put64(p, got + (uint64_t)A - P); return 0;
        default: return SNX_LINK_UNSUPPORTED;
    }
}

/* AArch64's, as ELF for the Arm 64-bit Architecture (5.7) computes them: S + A, its page, or its distance from P */
static int relocate_a64(unsigned char* p, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t slot) {
    uint64_t target = S + (uint64_t)A;
    unsigned scale;
    switch (type) {
        case SNX_A64_NONE: return 0;
        case SNX_A64_ABS64: snx_link_put64(p, target); return 0;
        case SNX_A64_PREL64: snx_link_put64(p, target - P); return 0;
        case SNX_A64_PREL32: return snx_link_rel32(p, target, P);
        case SNX_A64_ADR_PREL_LO21:
            if (!fits(target - P, 21)) return SNX_LINK_RANGE;
            adr_field(p, target - P);
            return 0;
        case SNX_A64_ADR_PREL_PG_HI21: return adrp(p, target, P);
        case SNX_A64_ADR_PREL_PG_HI21_NC: adr_field(p, (page_of(target) - page_of(P)) >> 12); return 0;
        case SNX_A64_ADD_ABS_LO12_NC: field(p, target & 0xFFF, 10, 12); return 0;
        case SNX_A64_LDST8_ABS_LO12_NC:
        case SNX_A64_LDST16_ABS_LO12_NC:
        case SNX_A64_LDST32_ABS_LO12_NC:
        case SNX_A64_LDST64_ABS_LO12_NC:
        case SNX_A64_LDST128_ABS_LO12_NC:
            /* the offset is scaled by the access's size, which the address must be a multiple of */
            scale = type == SNX_A64_LDST8_ABS_LO12_NC    ? 0
                    : type == SNX_A64_LDST16_ABS_LO12_NC ? 1
                    : type == SNX_A64_LDST32_ABS_LO12_NC ? 2
                    : type == SNX_A64_LDST64_ABS_LO12_NC ? 3
                                                         : 4;
            if (target & (((uint64_t)1 << scale) - 1)) return SNX_LINK_MISALIGNED;
            field(p, (target & 0xFFF) >> scale, 10, 12);
            return 0;
        case SNX_A64_ADR_GOT_PAGE: return adrp(p, slot + (uint64_t)A, P);
        case SNX_A64_LD64_GOT_LO12_NC: /* LDR Xt: the slot's offset in its page, in 8-byte units */
            if (slot & 7) return SNX_LINK_MISALIGNED;
            field(p, (slot & 0xFFF) >> 3, 10, 12);
            return 0;
        case SNX_A64_CALL26:
        case SNX_A64_JUMP26: return branch(p, target - P, 26, 0);
        case SNX_A64_CONDBR19: return branch(p, target - P, 19, 5);
        case SNX_A64_TSTBR14: return branch(p, target - P, 14, 5);
        default: return SNX_LINK_UNSUPPORTED;
    }
}

/* ---- PowerPC64, in the host's byte order as its objects are: a half16 relocation's offset is its halfword's own,
   in either byte order ---- */

static void host16(unsigned char* p, uint16_t v) { memcpy(p, &v, 2); }
static void host32(unsigned char* p, uint32_t v) { memcpy(p, &v, 4); }
static void host64(unsigned char* p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t host_get16(const unsigned char* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static uint32_t host_get32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint16_t ppc_lo(uint64_t v) { return (uint16_t)v; }
static uint16_t ppc_hi(uint64_t v) { return (uint16_t)(v >> 16); }
static uint16_t ppc_ha(uint64_t v) { return (uint16_t)((v + 0x8000) >> 16); }
/* whether value, read as signed, fits 32 bits: the reach of an @ha, @l pair */
static int fits32(uint64_t value) { return fits(value, 32); }
/* a DS-form field: the low two bits are the instruction's own */
static int ds_field(unsigned char* p, uint64_t value) {
    if (value & 3) return SNX_LINK_MISALIGNED;
    host16(p, (uint16_t)((host_get16(p) & 3) | (ppc_lo(value) & 0xFFFC)));
    return 0;
}
/* a branch's displacement into the instruction's mask */
static int ppc_branch(unsigned char* p, uint64_t displacement, unsigned bits, uint32_t mask) {
    if (!fits(displacement, bits)) return SNX_LINK_RANGE;
    host32(p, (host_get32(p) & ~mask) | ((uint32_t)displacement & mask));
    return 0;
}

/* PowerPC64's, as the 64-bit ELF V2 ABI (and the V1 supplement) compute them: S + A, its distance from P or from the
   TOC, and their halves */
static int relocate_ppc(unsigned char* p, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t toc) {
    uint64_t value = S + (uint64_t)A, relative = value - P, toc_relative = value - toc;
    switch (type) {
        case SNX_PPC_NONE:
        case SNX_PPC_ENTRY: /* the linker may rewrite a global entry's TOC setup; it needn't */
        case SNX_PPC_PLTSEQ:
        case SNX_PPC_PLTCALL: /* markers of an inline PLT call sequence, which code without a PLT leaves as it is */
            return 0;
        case SNX_PPC_TOC: host64(p, toc); return 0; /* .TOC., in an ELFv1 descriptor */
        case SNX_PPC_ADDR64: host64(p, value); return 0;
        case SNX_PPC_REL64: host64(p, relative); return 0;
        case SNX_PPC_REL32:
            if (!fits32(relative)) return SNX_LINK_RANGE;
            host32(p, (uint32_t)relative);
            return 0;
        case SNX_PPC_REL24: return ppc_branch(p, relative, 26, 0x03FFFFFCu);
        case SNX_PPC_REL14: return ppc_branch(p, relative, 16, 0xFFFCu);
        case SNX_PPC_REL16:
            if (!fits(relative, 16)) return SNX_LINK_RANGE;
            host16(p, ppc_lo(relative));
            return 0;
        case SNX_PPC_REL16_LO: host16(p, ppc_lo(relative)); return 0;
        case SNX_PPC_REL16_HI: host16(p, ppc_hi(relative)); return 0;
        case SNX_PPC_REL16_HA:
            if (!fits32(relative)) return SNX_LINK_RANGE;
            host16(p, ppc_ha(relative));
            return 0;
        case SNX_PPC_ADDR16_LO: host16(p, ppc_lo(value)); return 0;
        case SNX_PPC_ADDR16_HI: host16(p, ppc_hi(value)); return 0;
        case SNX_PPC_ADDR16_HA: host16(p, ppc_ha(value)); return 0;
        case SNX_PPC_ADDR16_LO_DS: return ds_field(p, value);
        case SNX_PPC_TOC16:
            if (!fits(toc_relative, 16)) return SNX_LINK_RANGE;
            host16(p, ppc_lo(toc_relative));
            return 0;
        case SNX_PPC_TOC16_LO: host16(p, ppc_lo(toc_relative)); return 0;
        case SNX_PPC_TOC16_HI: host16(p, ppc_hi(toc_relative)); return 0;
        case SNX_PPC_TOC16_HA:
            if (!fits32(toc_relative)) return SNX_LINK_RANGE;
            host16(p, ppc_ha(toc_relative));
            return 0;
        case SNX_PPC_TOC16_DS:
            if (!fits(toc_relative, 16)) return SNX_LINK_RANGE;
            return ds_field(p, toc_relative);
        case SNX_PPC_TOC16_LO_DS: return ds_field(p, toc_relative);
        default: return SNX_LINK_UNSUPPORTED;
    }
}

int snx_elf_relocate(unsigned machine, unsigned char* p, size_t room, unsigned type, uint64_t S, int64_t A, uint64_t P,
                     uint64_t got, uint64_t slot) {
    if (field_size(machine, type) > room) return SNX_LINK_OUTSIDE;
    if (machine == EM_AARCH64) return relocate_a64(p, type, S, A, P, slot);
    if (machine == EM_PPC64) return relocate_ppc(p, type, S, A, P, got);
    return relocate_x86(p, type, S, A, P, got, slot);
}

int snx_elf_veneer(unsigned char* v, uint64_t at, uint64_t target, int indirect) {
    int rc;
    if (indirect && (target & 7)) return SNX_LINK_MISALIGNED;
    snx_link_put32(v, 0x90000010u); /* ADRP x16, target */
    rc = adrp(v, target, at);
    if (rc) return rc;
    if (indirect) {
        snx_link_put32(v + 4, 0xF9400210u); /* LDR x16, [x16, :lo12:target], scaled by 8 */
        field(v + 4, (target & 0xFFF) >> 3, 10, 12);
    } else {
        snx_link_put32(v + 4, 0x91000210u); /* ADD x16, x16, :lo12:target */
        field(v + 4, target & 0xFFF, 10, 12);
    }
    snx_link_put32(v + 8, 0xD61F0200u); /* BR x16 */
    return 0;
}

int snx_elf_branch_veneer(unsigned char* p, uint64_t P, unsigned char* v, uint64_t at, uint64_t target, int indirect) {
    int rc = snx_elf_veneer(v, at, target, indirect);
    if (rc) return rc;
    return snx_elf_relocate(EM_AARCH64, p, 4, SNX_A64_CALL26, at, 0, P, 0, 0);
}

/* ELFv2's local entry offset: (1 << n) >> 2 instructions for st_other's n, none for 0 and 1 */
static uint64_t ppc_local_entry(unsigned char other) { return (uint64_t)((1u << ((other >> 5) & 7)) >> 2) << 2; }

int snx_elf_ppc_entry(const SnxElfObject* objs, const SnxLinkNames* globals, unsigned abi, size_t def_object,
                      size_t def_index, uint64_t S, int64_t A, uint64_t* global, uint64_t* local, char* error,
                      size_t error_size) {
    const SnxElfObject* o = &objs[def_object];
    const Elf64_Sym* sym = &o->syms[def_index];
    size_t j, k, code_object, code_index;
    int absolute;
    *global = *local = S + (uint64_t)A;
    if (abi == SNX_ELF_PPC_ELFV2) {
        if (((sym->st_other >> 5) & 7) == 1)
            return fail(error, error_size, "%s: a call to %s, which doesn't keep r2 (PC-relative code)", o->input->name,
                        snx_elf_symbol_name(o, sym));
        *local += ppc_local_entry(sym->st_other);
        return 0;
    }
    if (sym->st_shndx >= o->eh->e_shnum || strcmp(snx_elf_section_name(o, sym->st_shndx), ".opd")) return 0;
    for (j = 0; j < o->eh->e_shnum; ++j) {
        const Elf64_Rela* r = (const Elf64_Rela*)(o->input->data + o->sh[j].sh_offset);
        size_t n = o->sh[j].sh_size / sizeof(Elf64_Rela);
        if (o->sh[j].sh_type != SHT_RELA || o->sh[j].sh_info != sym->st_shndx) continue;
        for (k = 0; k < n; ++k)
            if (r[k].r_offset == sym->st_value + (uint64_t)A && ELF64_R_TYPE(r[k].r_info) == SNX_PPC_ADDR64) {
                int where = snx_elf_definition(objs, globals, def_object, ELF64_R_SYM(r[k].r_info), &code_object,
                                               &code_index, error, error_size);
                if (where < 0) return -1;
                if (where != SNX_ELF_HERE) break;
                *global = *local = snx_elf_symbol_address(objs, code_object, code_index, &absolute) +
                                   (uint64_t)r[k].r_addend;
                return 0;
            }
    }
    return fail(error, error_size, "%s: .opd descriptor of %s without its code", o->input->name,
                snx_elf_symbol_name(o, sym));
}

int snx_elf_ppc_branch_veneer(unsigned char* p, uint64_t P, unsigned char* v, uint64_t at, uint64_t target,
                              uint64_t toc) {
    uint64_t offset = target - toc;
    if (!fits32(offset)) return SNX_LINK_RANGE;
    host32(v, 0x3D820000u | ppc_ha(offset));     /* addis r12, r2, offset@ha */
    host32(v + 4, 0x398C0000u | ppc_lo(offset)); /* addi r12, r12, offset@l */
    host32(v + 8, 0x7D8903A6u);                  /* mtctr r12 */
    host32(v + 12, 0x4E800420u);                 /* bctr */
    return snx_elf_relocate(EM_PPC64, p, 4, SNX_PPC_REL24, at, 0, P, toc, 0);
}
#else
typedef int snx_elf_object_unused; /* ISO C wants a declaration in every file */
#endif
