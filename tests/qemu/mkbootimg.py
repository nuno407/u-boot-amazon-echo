#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Android boot images (header v0) for the QEMU tests, in the shape abboot boots (SPEC.md, "The
slot convention"): the kernel part a gzip'd arm64 Image with a DTB appended, a ramdisk, the
header id mkbootimg would write.

    mkbootimg.py --payload payload.bin --tag TAG --dtb slot.dtb --out boot.img [--cmdline LINE]
"""
import argparse
import gzip
import hashlib
import io
import struct

PAGE = 2048
HDR = '<8s10I16s512s32s1024s'   # magic, sizes and addresses, name, cmdline, id, extra_cmdline


def pad(b):
    return b + bytes(-len(b) % PAGE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--payload', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--dtb', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--cmdline', default='console=ttyAMA0')
    a = ap.parse_args()

    img = bytearray(open(a.payload, 'rb').read())
    at = img.index(b'@' * 16)
    tag = a.tag.encode()
    if len(tag) > 16:
        raise SystemExit('tag longer than 16 bytes')
    img[at:at + 16] = tag.ljust(16, b' ')
    b = io.BytesIO()
    with gzip.GzipFile(filename='', mode='wb', fileobj=b, mtime=0) as g:
        g.write(bytes(img))
    kernel = b.getvalue() + open(a.dtb, 'rb').read()
    ramdisk = b'not a real ramdisk\n' * 16

    sha = hashlib.sha1()
    for part in (kernel, ramdisk, b''):
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    hdr = struct.pack(HDR, b'ANDROID!', len(kernel), 0x40080000, len(ramdisk), 0x45000000, 0,
                      0, 0x40000100, PAGE, 0, 0, b'', a.cmdline.encode(), sha.digest(), b'')
    with open(a.out, 'wb') as o:
        o.write(pad(hdr) + pad(kernel) + pad(ramdisk))


if __name__ == '__main__':
    main()
