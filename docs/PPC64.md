<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# PowerPC64

ScarletNX builds for 64-bit PowerPC Linux from POWER8 on. The little-endian port (ppc64le) works: the test suite
passes under qemu-user, and Super Haxagon, recompiled under qemu, played a level to its game over screen there with
lavapipe as the Vulkan driver. No real POWER machine has run it yet, and CI doesn't build it yet (the `linux-ppc64le`
job in `.github/workflows/build.yml` is commented out).

The big-endian hosts are not ported yet. Their builds are set up (the `linux-ppc64-cross` preset for Debian's glibc
port, ELFv1, and `linux-ppc64-musl-cross` for musl, ELFv2), and the recompiler's linker takes both ABIs, but guest
memory is still read and written in the host's byte order. A Switch game's memory is little-endian, so every load and
store, the exclusive monitor, the vector registers, IPC and the services' structures, and the GPU's data have to
convert on a big-endian host. That is the next part of the port.

## Building on an x86-64 Debian machine

The tools, once:

```bash
sudo apt install clang lld llvm ninja-build cmake ccache mmdebstrap qemu-user qemu-user-binfmt binfmt-support \
    glslang-tools spirv-tools debian-ports-archive-keyring
```

A sysroot with the target's libraries and LLVM, made without root by `scripts/ppc64-sysroot.sh` (about 1 GB). It is
staged in `/var/tmp`, as `/tmp` is often too small. `ppc64` makes the big-endian one from Debian's ports archive:

```bash
scripts/ppc64-sysroot.sh ppc64el .cache/sysroot-ppc64el
export SNX_SYSROOT=$PWD/.cache/sysroot-ppc64el
```

Then the build and the tests, which run under qemu-ppc64le:

```bash
cmake --preset linux-ppc64le-cross -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build --preset linux-ppc64le-cross
ctest --preset linux-ppc64le-cross -j"$(nproc)"
```

The target's `llvm-config` is a PowerPC program, so `scripts/ppc64-llvm-config.sh` runs it under qemu. The presets
point pkg-config at the sysroot and set `QEMU_LD_PREFIX` for the tests. Three tests are skipped under the emulator:
`3ds_static_pipeline` and `3ds_extract` link with the compiler path built into the driver, which is the build
machine's, and `gui_process_tree` reads a resident size that qemu reports as zero.

## Running a game under qemu

The recompiler runs on the machine the module is for, so under qemu it takes a while (Super Haxagon: about 20
minutes, against 4 natively on x86-64):

```bash
export QEMU_LD_PREFIX=$SNX_SYSROOT VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
qemu-ppc64le build/linux-ppc64le-cross/scarletnx -r app.nro -o App --outdir games/ppc64le
cd games/ppc64le/App && qemu-ppc64le ./App -H --seconds=120 --shot=/path/to/shot
```

## Host pages larger than 4 KiB

Most ppc64le distributions, Debian's among them, run kernels with 64 KiB pages, and Apple silicon and some AArch64
kernels use 16 KiB ones. The Switch maps memory in 4 KiB pages, which a host can only commit and protect in its own,
so `src/nx/memory/mem.c` keeps the guest's pages in its block list and commits whole host pages under them. Freeing a
guest page decommits its host page once nothing else in it is mapped, and zeroes the page otherwise, so it reads zero
when mapped again, as on the console. The differences from a 4 KiB host:

- A guest access to a free page that shares a host page with a mapped one doesn't fault.
- Views of memory (MapMemory, shared memory, ldr's code) are copies, as on Windows, instead of the same pages. A view
  can only share host pages with its source when both sit at the same place in a host page, and games pick their
  addresses in 4 KiB steps. The services reach shared memory through the guest's mapping while it lasts.
- The Linux runtime (`src/gnuabi/`) protects each host page as the most permissive of its guest pages, so a write to
  compiled code beside writable data in one host page isn't seen.

`SNX_HOST_PAGE=16384` or `65536` in the environment makes any machine work in those pages, which is how the tests
and CI check these paths on 4 KiB hosts (`ldr_service_64k_pages`, `gnuabi_memory_64k_pages`,
`nx_runtime_homebrew_64k_pages`); `memory_accounting` runs as on 16 and 64 KiB hosts by itself.

## What differs from the other hosts

- **The baseline is POWER8** (`cmake/cross.cmake`). Before it there is no `lqarx`/`stqcx.`, and the 128-bit
  compare-and-swap that STXP becomes would be a libatomic call, which a module doesn't link.
- **Guest tail calls go through stubs.** Without POWER10's PC-relative code, LLVM refuses a guaranteed tail call to a
  function it can't see shares the caller's TOC: another unit's function, or a pointer. A module has one TOC, so the
  jump is safe, and `ppc_tail_calls` in `src/common/cpu/recompiler/llvm/ir.c` sends those calls to a naked stub of the
  same module that branches on.
- **The linker** (`src/common/cpu/recompiler/llvm/link_elf.c`, with the relocations in `elf_object.c`) handles the
  TOC relocations, ELFv2's local entry points, ELFv1's `.opd` descriptors, and 16-byte stubs for a call past `bl`'s
  32 MiB.
- **No runtime JIT.** The JIT's loader (`code_loader.c`) doesn't load PowerPC64 objects: code loaded beside the
  runtime would call its functions, which have a TOC of their own, and each such call would need a stub that saves
  and restores r2. The JIT replaced the interpreter, so there only the code the recompiler found can run.
- **Rounding stays on the slow path.** LLVM 19 lowers `llvm.roundeven` to a libm call on POWER8, and fails on its
  vector form, so FRINTN and the rounding conversions use the software path. Fused multiply-add is a fast path, as
  POWER always has it.
- **SDL3 is built without AltiVec** (`cmake/dependencies.cmake`): its blitters prefetch with `dstt`, which the ISA
  phased out and qemu traps.
