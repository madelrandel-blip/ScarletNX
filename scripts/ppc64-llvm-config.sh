#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The target LLVM's llvm-config for a cross build (SNX_LLVM_CONFIG of the linux-ppc64* presets, docs/PPC64.md). The
# sysroot's llvm-config is a PowerPC program: it runs under qemu-user, and the paths it prints that are the target's
# own (/usr/...) get the sysroot in front, as the build machine reads them there.
set -eu
sysroot=${SNX_SYSROOT:?SNX_SYSROOT names the sysroot (scripts/ppc64-sysroot.sh)}
config=
for c in "$sysroot"/usr/lib/llvm-*/bin/llvm-config "$sysroot"/usr/bin/llvm-config; do
    [ -x "$c" ] && config=$c
done
[ -n "$config" ] || { echo "no llvm-config in $sysroot: install LLVM's development files there" >&2; exit 1; }
# the program's byte order, from its ELF header (EI_DATA: 1 little-endian, 2 big-endian)
case $(od -An -tu1 -j5 -N1 "$config" | tr -d ' ') in
    1) qemu=qemu-ppc64le ;;
    *) qemu=qemu-ppc64 ;;
esac
"$qemu" -L "$sysroot" "$config" "$@" | sed -E "s#(^|[ =]|-I|-L)/(usr|lib)/#\\1$sysroot/\\2/#g"
