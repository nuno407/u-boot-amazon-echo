#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read and edit the boot layer's A/B record (32 bytes at misc byte 2048, in the layout of
AOSP's bootloader_control) on a computer, from a copy of misc.

The reference for the layout the boot layer's U-Boot code (abboot/ab.c) and bootctl
(tools/bootctl) use (SPEC.md, "The A/B record"). It works on misc block 4 (bytes 2048-2559) as
dd gives it, so the record is at offset 0 of its input:

    ssh root@DEVICE 'dd if=/dev/disk/by-partlabel/misc bs=512 skip=4 count=1 2>/dev/null' |
        abctl.py show

    dd if=misc.img bs=512 skip=4 count=1 2>/dev/null |
        abctl.py set b --tries 0 --successful 0 > blk4.bin
    dd if=blk4.bin of=misc.img bs=512 seek=4 conv=notrunc

Commands:
    show                         decode stdin, print the record (exit 1 if it is not valid)
    set SLOT [--priority N] [--tries N] [--successful 0|1] [--last SLOT]
                                 stdin's block with the record changed, to stdout; a block
                                 without a valid record starts from a fresh one (both slots 0)
    default                      the default record U-Boot and `bootctl init` write (a priority
                                 15, b 14, both 3 tries, neither successful), 32 bytes to stdout

Every write zeroes reserved1 (bytes 20-27), as AOSP leaves it.
"""
import struct
import sys
import zlib

MAGIC = 0x42414342


def crc(b):
    return zlib.crc32(b[:28]) & 0xffffffff


def decode(raw):
    """dict of the record, or raises ValueError."""
    if len(raw) < 32:
        raise ValueError('short: %d bytes' % len(raw))
    if struct.unpack_from('<I', raw, 4)[0] != MAGIC:
        raise ValueError('no magic (all zero: %s)' % (not any(raw[:32])))
    if struct.unpack_from('<I', raw, 28)[0] != crc(raw):
        raise ValueError('bad crc32')
    if raw[8] != 1 or (raw[9] & 7) < 2:
        raise ValueError('version %d, nb_slot %d' % (raw[8], raw[9] & 7))
    last = raw[1:2].decode() if raw[0:1] == b'_' and raw[1:2] in (b'a', b'b') else '-'
    slots = {}
    for i, s in enumerate('ab'):
        b0, b1 = raw[12 + 2 * i], raw[13 + 2 * i]
        slots[s] = dict(priority=b0 & 15, tries=(b0 >> 4) & 7, successful=b0 >> 7, verity=b1 & 1)
    return dict(last=last, slots=slots, recovery_tries=(raw[9] >> 3) & 7)


def encode(rec, raw=bytes(32)):
    """rec into raw (32 bytes or more), keeping the bytes the record does not own."""
    b = bytearray(raw)
    b[0:4] = b'_' + rec['last'].encode() + b'\0\0' if rec['last'] in ('a', 'b') else bytes(4)
    struct.pack_into('<I', b, 4, MAGIC)
    b[8] = 1
    b[9] = (b[9] & ~7 & 0xff) | 2
    for i, s in enumerate('ab'):
        x = rec['slots'][s]
        b[12 + 2 * i] = (x['priority'] & 15) | (x['tries'] & 7) << 4 | (0x80 if x['successful'] else 0)
    b[20:28] = bytes(8)
    struct.pack_into('<I', b, 28, crc(bytes(b)))
    return bytes(b)


def bootable(x):
    return x['priority'] > 0 and (x['successful'] or x['tries'] > 0) and not x['verity']


def word(x):
    return 'bad' if not bootable(x) else ('good' if x['successful'] else 'trial')


def default():
    return dict(last='a', slots={s: dict(priority=15 - i, tries=3, successful=0, verity=0)
                                 for i, s in enumerate('ab')})


def show(rec):
    print('last booted: %s' % rec['last'])
    for s in 'ab':
        x = rec['slots'][s]
        print('slot %s: priority %2d  tries %d  successful %d  verity %d  -> (%s)' % (
            s, x['priority'], x['tries'], x['successful'], x['verity'], word(x)))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'show':
        raw = sys.stdin.buffer.read()
        try:
            rec = decode(raw)
        except ValueError as e:
            print('no valid record: %s; bytes: %s' % (e, raw[:32].hex()))
            return 1
        show(rec)
        return 0
    if cmd == 'default' and len(argv) == 2:
        rec = default()
        so, sys.stdout = sys.stdout, sys.stderr
        show(rec)
        sys.stdout = so
        sys.stdout.buffer.write(encode(rec))
        return 0
    if cmd == 'set' and len(argv) >= 3 and argv[2] in ('a', 'b'):
        raw = sys.stdin.buffer.read() or bytes(512)
        try:
            rec = decode(raw)
        except ValueError:
            rec = dict(last='-', slots={s: dict(priority=0, tries=0, successful=0, verity=0)
                                        for s in 'ab'})
        x = rec['slots'][argv[2]]
        it = iter(argv[3:])
        for opt in it:
            val = next(it)
            if opt == '--priority':
                x['priority'] = int(val)
            elif opt == '--tries':
                x['tries'] = int(val)
            elif opt == '--successful':
                x['successful'] = int(val)
            elif opt == '--last':
                rec['last'] = val
            else:
                raise SystemExit('unknown option %s' % opt)
        out = encode(rec, raw)
        print('new record:', file=sys.stderr)
        so, sys.stdout = sys.stdout, sys.stderr
        show(decode(out))
        sys.stdout = so
        sys.stdout.buffer.write(out)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
