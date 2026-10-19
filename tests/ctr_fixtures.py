# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Synthetic CTR format fixtures. No console assets, keys or signed content."""

import hashlib
import struct

TITLE = 0x0004000001234500
NONE = 0xFFFFFFFF
sha = lambda b: hashlib.sha256(b).digest()
align = lambda n, a: (n + a - 1) & -a


def put(b, offset, fmt, *values):
    struct.pack_into(fmt, b, offset, *values)


def repeat_blz(size):
    # Reverse decode three 'A' literals then distance-3 backreferences.
    ops = [(False, b'A')] * 3
    left = size - 3
    while left:
        count = min(18, left)
        if count < 3:
            ops.extend([(False, b'A')] * count)
        else:
            ops.append((True, struct.pack('<H', (count - 3) << 12)))
        left -= count
    groups = []
    for at in range(0, len(ops), 8):
        group = ops[at : at + 8]
        flags = sum(int(ref) << (7 - i) for i, (ref, _) in enumerate(group))
        groups.append(b''.join(data for _, data in reversed(group)) + bytes([flags]))
    payload = b''.join(reversed(groups))
    total = len(payload) + 8
    assert total <= size
    return payload + struct.pack('<II', (8 << 24) | total, size - total)


def romfs(file_name='hello.txt', directory_name='dir', patch=None, duplicate=False):
    root_name = file_name.encode('utf-16le', errors='surrogatepass')
    child_name = 'é😀.bin'.encode('utf-16le')
    directory = directory_name.encode('utf-16le', errors='surrogatepass')
    dirs = bytearray(24 + align(24 + len(directory), 4))
    put(dirs, 0, '<6I', 0, NONE, 24, 0, NONE, 0)
    put(dirs, 24, '<6I', 0, NONE, NONE, NONE if duplicate else align(32 + len(root_name), 4), NONE, len(directory))
    dirs[48 : 48 + len(directory)] = directory
    first_size = align(32 + len(root_name), 4)
    if duplicate:
        child_name = root_name
    files = bytearray(first_size + align(32 + len(child_name), 4))
    put(files, 0, '<IIQQII', 0, first_size if duplicate else NONE, 0, 5, NONE, len(root_name))
    files[32 : 32 + len(root_name)] = root_name
    put(files, first_size, '<IIQQII', 0 if duplicate else 24, NONE, 5, 4, NONE, len(child_name))
    files[first_size + 32 : first_size + 32 + len(child_name)] = child_name
    header = bytearray(0x28)
    dh = 0x28
    dt = dh + 4
    fh = dt + len(dirs)
    ft = fh + 4
    data = align(ft + len(files), 16)
    put(header, 0, '<10I', 0x28, dh, 4, dt, len(dirs), fh, 4, ft, len(files), data)
    if patch:
        patch(header, dirs, files)
    level3 = bytearray(data + 9)
    level3[:0x28] = header
    put(level3, dh, '<I', 0)
    put(level3, fh, '<I', 0)
    level3[dt : dt + len(dirs)] = dirs
    level3[ft : ft + len(files)] = files
    level3[data:] = b'helloDATA'
    padded3 = bytes(level3).ljust(0x1000, b'\0')
    level2 = sha(padded3).ljust(0x1000, b'\0')
    level1 = sha(level2).ljust(0x1000, b'\0')
    archive = bytearray(0x4000)
    archive[:4] = b'IVFC'
    put(archive, 4, '<II', 0x10000, 32)
    for at, logical, length in [(0xC, 0, 32), (0x24, 0x1000, 32), (0x3C, 0x2000, len(level3))]:
        put(archive, at, '<QQII', logical, length, 12, 0)
    put(archive, 0x54, '<I', 0x5C)
    archive[0x60:0x80] = sha(level1)
    archive[0x1000:0x2000] = padded3
    archive[0x2000:0x3000] = level1
    archive[0x3000:0x4000] = level2
    return bytes(archive)


def ncch(
    compressed=False,
    with_romfs=True,
    romfs_options=None,
    patch_exheader=None,
    patch_exefs=None,
    encrypted=False,
    patch_header=None,
    code_blob=None,
):
    exheader = bytearray(0x800)
    exheader[:8] = b'TESTCTR\0'
    exheader[0xD] = int(compressed)
    put(exheader, 0x10, '<3I', 0x100000, 1, 8)
    put(exheader, 0x1C, '<I', 0x4000)
    put(exheader, 0x20, '<3I', 0x200000, 1, 16)
    put(exheader, 0x30, '<4I', 0x300000, 1, 4, 0x1000)
    put(exheader, 0x200, '<Q', TITLE)
    exheader[0x20F] = 0x30  # the main thread's priority (ARM11 local capabilities), an application's usual one
    if patch_exheader:
        patch_exheader(exheader)
    code = bytearray(0x3000)
    put(code, 0, '<II', 0xE3A0002A, 0xEF000003)  # ARM MOV r0,#42; SVC #3 (not executed by extraction)
    code[0x1000:0x1010] = b'READONLY-DATA!!!'
    code[0x2000:0x2004] = b'DATA'
    stored = repeat_blz(len(code)) if compressed else bytes(code)
    if code_blob is not None:
        stored = code_blob
    exefs = bytearray(align(0x200 + align(len(stored), 512) + 4, 512))
    exefs[:8] = b'.code\0\0\0'
    put(exefs, 8, '<II', 0, len(stored))
    exefs[16:24] = b'icon\0\0\0\0'
    put(exefs, 24, '<II', align(len(stored), 512), 4)
    exefs[0x200 : 0x200 + len(stored)] = stored
    exefs[0x200 + align(len(stored), 512) : 0x204 + align(len(stored), 512)] = b'ICON'
    exefs[0x1E0:0x200] = sha(stored)
    exefs[0x1C0:0x1E0] = sha(b'ICON')
    if patch_exefs:
        patch_exefs(exefs)
    rom = romfs(**(romfs_options or {})) if with_romfs else b''
    header = bytearray(0x200)
    header[0x100:0x104] = b'NCCH'
    total = 0xA00 + len(exefs) + len(rom)
    put(header, 0x104, '<I', total // 512)
    put(header, 0x112, '<H', 2)
    put(header, 0x118, '<Q', TITLE)
    header[0x150:0x160] = b'CTR-P-SYNTHETIC\0'
    header[0x160:0x180] = sha(exheader[:0x400])
    put(header, 0x180, '<I', 0x400)
    header[0x18C:0x190] = bytes([1, 2, 0, (0 if encrypted else 4) | (0 if rom else 2)])
    put(header, 0x1A0, '<3I', 0xA00 // 512, len(exefs) // 512, 1)
    header[0x1C0:0x1E0] = sha(exefs[:0x200])
    if rom:
        put(header, 0x1B0, '<3I', (0xA00 + len(exefs)) // 512, len(rom) // 512, 1)
        header[0x1E0:0x200] = sha(rom[:0x200])
    if patch_header:
        patch_header(header)
    return bytes(header + exheader + exefs + rom)


def cia(
    application=None,
    signature=0x10004,
    boot=5,
    title=TITLE,
    ticket_title=None,
    encrypted=False,
    info_ranges=None,
    patch_chunks=None,
    patch_body=None,
    manual_size=512,
    with_meta=True,
    certificates=b'',
):
    if application is None:
        application = ncch()
    body_at = {0x10000: 0x240, 0x10003: 0x240, 0x10001: 0x140, 0x10004: 0x140, 0x10002: 0x40, 0x10005: 0x40}[signature]
    ticket = bytearray(body_at + 0x164 + 0x14)
    put(ticket, 0, '>I', signature)
    put(ticket, body_at + 0x9C, '>Q', title if ticket_title is None else ticket_title)
    put(ticket, body_at + 0x164, '>HHIIHHI', 1, 0x14, 0x14, 0x14, 0, 0x14, 0)
    chunks = bytearray(3 * 0x30)
    manual = b'M' * manual_size
    for i, (index, content) in enumerate([(0, manual), (5, application), (7, b'absent')]):
        put(chunks, i * 0x30, '>IHHQ', 0x1111 * (i + 1), index, int(encrypted and i == 1), len(content))
        chunks[i * 0x30 + 16 : (i + 1) * 0x30] = sha(content)
    if patch_chunks:
        patch_chunks(chunks)
    body = bytearray(0x9C4)
    put(body, 0x4C, '>Q', title)
    put(body, 0x9C, '>HHH', 1, 3, boot)
    for i, (first, count) in enumerate(info_ranges or [(0, 1), (1, 2)]):
        put(body, 0xC4 + i * 0x24, '>HH', first, count)
        body[0xC8 + i * 0x24 : 0xE8 + i * 0x24] = sha(chunks[first * 0x30 : (first + count) * 0x30])
    body[0xA4:0xC4] = sha(body[0xC4:])
    if patch_body:
        patch_body(body)
    tmd = bytearray(body_at) + body + chunks
    put(tmd, 0, '>I', signature)
    header = bytearray(0x2020)
    put(
        header,
        0,
        '<IHHIIIIQ',
        0x2020,
        0,
        0,
        len(certificates),
        len(ticket),
        len(tmd),
        0x400 if with_meta else 0,
        len(manual) + len(application),
    )
    header[0x20] = 0x84  # present indices 0 and 5; index 7 deliberately absent
    archive = header.ljust(0x2040, b'\0')
    for section in [certificates, ticket, tmd]:
        archive += section
        archive += b'\0' * (align(len(archive), 64) - len(archive))
    archive += manual + application
    if with_meta:
        archive += b'\0' * (align(len(archive), 64) - len(archive)) + b'\0' * 0x400
    return bytes(archive)
