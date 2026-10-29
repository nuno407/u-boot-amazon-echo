#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# bootctl (tools/bootctl) under a POSIX shell and an awk with and()/xor()/rshift(), every record it
# writes decoded with abctl.py, and its round trip with it:
#  - without a record, nothing in misc changes (exit 3), whatever the command;
#  - init writes the default record (the same bytes as abctl.py default and ab.c's ab_default),
#    and only into an empty area;
#  - set-active, mark-good, mark-bad, set: the fields SPEC.md gives, the CRC right, the other bytes
#    of misc and of the record kept, reserved1 zeroed, no write when nothing changes;
#  - current: ab.slot=, and nothing else;
#  - a record abctl.py made is read back by bootctl status as abctl.py shows it.
#
# The shell and awk: busybox if it is installed (as on most small Linux root filesystems), else
# sh with gawk, else Docker's alpine image (busybox). BOOTCTL_RUNNER=busybox|gawk|docker picks one.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
AB="python3 -I $D/tools/abctl.py"
fail() { echo "bootctl-test: FAIL: $*"; exit 1; }
cp "$D/tools/bootctl" "$W/bootctl"

RUNNER=${BOOTCTL_RUNNER:-}
if [ -z "$RUNNER" ]; then
	if command -v busybox >/dev/null 2>&1; then RUNNER=busybox
	elif command -v gawk >/dev/null 2>&1; then RUNNER=gawk
	elif command -v docker >/dev/null 2>&1; then RUNNER=docker
	else echo "bootctl-test: SKIP: needs busybox, gawk or docker"; exit 0
	fi
fi
mkdir -p "$W/bin"
case $RUNNER in
busybox) for a in awk dd od tr sed grep cat; do ln -s "$(command -v busybox)" "$W/bin/$a"; done;;
gawk) ln -s "$(command -v gawk)" "$W/bin/awk";;
esac
echo "bootctl-test: running bootctl with $RUNNER"

# misc: 512 KB with something in each area around the record.
mk() {
	python3 -c "import sys; b=bytearray(512*1024); b[0:13]=b'boot-recovery'; b[4096:4110]=b'OTHER DATA 01 '; b[0x7f000:0x7f004]=b'UBC1'; open(sys.argv[1],'wb').write(b)" "$W/misc"
}
others() { python3 -c "import sys; b=open(sys.argv[1],'rb').read(); print(b[:2048].hex()+b[2080:].hex())" "$W/misc"; }
rec() { dd if="$W/misc" bs=512 skip=4 count=1 2>/dev/null | $AB show; }
field() { rec | sed -n "s/^slot $1: priority *\([0-9]*\) *tries \([0-9]\) *successful \([01]\).*/p\1 t\2 s\3/p"; }
raw() { dd if="$W/misc" bs=1 skip=2048 count=32 2>/dev/null | od -An -tx1 | tr -d ' \n'; }
put_raw() { dd if="$1" of="$W/misc" bs=1 seek=2048 count=32 conv=notrunc 2>/dev/null; }
N=0
# bc <cmdline> <bootctl args...>: exit status in $rc, output in $W/out
bc() {
	line=$1; shift
	# a new file each time: Docker Desktop's file sharing can serve a rewritten small file stale
	N=$((N + 1))
	printf '%s\n' "$line" > "$W/cmdline.$N"
	set +e
	case $RUNNER in
	docker)
		docker run --rm -v "$W:/w" -e BOOTCTL_MISC=/w/misc -e BOOTCTL_CMDLINE=/w/cmdline.$N \
			-e BOOTCTL_CONF=/nonexistent alpine:3.20 sh /w/bootctl "$@" > "$W/out" 2>&1;;
	busybox)
		PATH="$W/bin:$PATH" BOOTCTL_MISC="$W/misc" BOOTCTL_CMDLINE="$W/cmdline.$N" \
			BOOTCTL_CONF=/nonexistent busybox sh "$W/bootctl" "$@" > "$W/out" 2>&1;;
	*)
		PATH="$W/bin:$PATH" BOOTCTL_MISC="$W/misc" BOOTCTL_CMDLINE="$W/cmdline.$N" \
			BOOTCTL_CONF=/nonexistent sh "$W/bootctl" "$@" > "$W/out" 2>&1;;
	esac
	rc=$?
	set -e
}
sha() { python3 -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"; }

# 1. No record: nothing changes, exit 3, a clear message (none with -q).
mk; before=$(sha "$W/misc")
for c in "status" "set-active a" "mark-good b" "mark-bad a" "set a priority=1"; do
	bc "ab.slot=a" $c
	[ $rc = 3 ] || { cat "$W/out"; fail "$c without a record: exit $rc"; }
done
grep -q "no A/B record" "$W/out" || fail "no message without a record"
bc "" -q mark-good a; [ $rc = 3 ] && [ ! -s "$W/out" ] || fail "-q is not quiet"
[ "$(sha "$W/misc")" = "$before" ] || fail "misc written without a record"
echo "ok: no record, misc untouched"

# 2. init: the default record, only into an empty area.
o0=$(others)
bc "" init; [ $rc = 0 ] || { cat "$W/out"; fail "init"; }
[ "$(raw)" = "$($AB default 2>/dev/null | od -An -tx1 | tr -d ' \n')" ] || fail "init wrote $(raw)"
[ "$(raw)" = 5f61000042434142010200003f003e000000000000000000000000005a0fd7c0 ] || fail "init is not ab.c's default"
[ "$(others)" = "$o0" ] || fail "init changed misc outside the record"
s1=$(sha "$W/misc"); bc "" init; [ $rc = 0 ] && [ "$(sha "$W/misc")" = "$s1" ] || fail "a second init"
mk; printf 'x' | dd of="$W/misc" bs=1 seek=3000 conv=notrunc 2>/dev/null
bc "" init; [ $rc = 1 ] || fail "init over other data: exit $rc"
echo "ok: init"

# 3. The edits, on a record with a p15 t2 s1, b p14 t0 s1, last a, and junk in reserved1,
# with a verity bit, recovery tries and slot_info[2] set, which must be kept.
mk
python3 - "$W/r" <<'EOF'
import struct, sys, zlib
b = bytearray(32)
b[0:2] = b'_a'; struct.pack_into('<I', b, 4, 0x42414342); b[8] = 1; b[9] = 2 | 3 << 3
b[12] = 15 | 2 << 4 | 0x80; b[14] = 14 | 0x80; b[15] = 0; b[16] = 0x5a
struct.pack_into('<I', b, 20, 0x12345678)
struct.pack_into('<I', b, 28, zlib.crc32(bytes(b[:28])))
open(sys.argv[1], 'wb').write(b)
EOF
put_raw "$W/r"; o0=$(others)
bc "ab.slot=a" status
grep -q "slot a: priority 15, tries 2, successful 1 (good)" "$W/out" && grep -q "booted: a" "$W/out" ||
	{ cat "$W/out"; fail "status"; }
echo "ok: status reads a record abctl.py would show the same way"
bc "ab.slot=a" set-active b
[ "$(field b)" = "p15 t3 s0" ] && [ "$(field a)" = "p14 t2 s1" ] || fail "set-active b: a $(field a), b $(field b)"
r=$(raw); [ "${r:40:16}" = 0000000000000000 ] || fail "reserved1 not zeroed: $r"
[ "${r:18:2}" = 1a ] && [ "${r:32:2}" = 5a ] || fail "recovery tries or slot_info[2] not kept: $r"
echo "ok: set-active (reserved1 zeroed, other bytes kept)"
bc "ab.slot=b ab.kernel=b" mark-good
[ "$(field b)" = "p15 t3 s1" ] || fail "mark-good (booted b): b $(field b)"
s1=$(sha "$W/misc"); bc "ab.slot=b" mark-good; [ "$(sha "$W/misc")" = "$s1" ] || fail "an unchanged mark-good wrote"
echo "ok: mark-good (the booted slot; no write when unchanged)"
bc "" mark-good; [ $rc = 1 ] || fail "mark-good with no slot booted: exit $rc"
bc "ab.slot=b" mark-bad a
[ "$(field a)" = "p0 t0 s0" ] && [ "$(field b)" = "p15 t3 s1" ] || fail "mark-bad a: a $(field a), b $(field b)"
rec | grep -q "slot a: .*(bad)" || fail "a is not bad"
echo "ok: mark-bad"
bc "" set a priority=14 tries=3
[ "$(field a)" = "p14 t3 s0" ] || fail "set a: $(field a)"
bc "" set a successful=1
[ "$(field a)" = "p14 t3 s1" ] || fail "set a successful: $(field a)"
bc "" set a priority=16; [ $rc = 2 ] || fail "set priority=16 accepted"
echo "ok: set"
[ "$(others)" = "$o0" ] || fail "misc outside 2048-2079 changed"
# the verity bit, set by hand, survives an edit
python3 - "$W/misc" <<'EOF'
import struct, sys, zlib
f = open(sys.argv[1], 'r+b'); f.seek(2048); b = bytearray(f.read(32))
b[13] |= 1; struct.pack_into('<I', b, 28, zlib.crc32(bytes(b[:28]))); f.seek(2048); f.write(b)
EOF
bc "" set-active a
rec | grep -q "slot a: .*verity 1" || fail "the verity bit was not kept"
bc "" status; grep -q "slot a: .*verity corrupted (bad)" "$W/out" || { cat "$W/out"; fail "status: verity"; }
echo "ok: a verity bit is kept and shown"
echo "ok: misc outside the record never changed"

# 4. A broken record is not written.
printf '\377' | dd of="$W/misc" bs=1 seek=2060 conv=notrunc 2>/dev/null
before=$(sha "$W/misc"); bc "" set-active b
[ $rc = 3 ] && [ "$(sha "$W/misc")" = "$before" ] || fail "a record with a bad CRC was written"
echo "ok: a broken record is not touched"

# 5. current.
bc "console=ttyS0 ab.slot=b ab.kernel=a" current; [ $rc = 0 ] && [ "$(cat "$W/out")" = b ] || fail "current ab.slot=b"
bc "console=ttyS0 root=/dev/vda2" current; [ $rc = 1 ] || fail "current without the boot layer"
bc "ab.kernel=rescue" current; [ $rc = 1 ] || fail "current on the rescue system"
echo "ok: current"
echo "bootctl-test: all passed"
