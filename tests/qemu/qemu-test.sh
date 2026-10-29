#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The whole boot flow in QEMU: U-Boot with abboot (the example board, boards/qemu-arm64/) boots a
# virtio disk with the partitions abboot expects, and stand-in kernels (payload.S) that print
# whose kernel they are and power the machine off. Between boots the test reads and edits the A/B
# record with tools/abctl.py and tools/bootctl, as an OS would.
#
#   tests/qemu/qemu-test.sh <u-boot.bin> [menu]
#
# Runs in the build image (uboot/docker/Dockerfile): qemu-system-aarch64, gcc, dtc, mkfs.ext4,
# sgdisk, gawk. "menu": the build has CONFIG_AB_MENU, so the menu's countdown runs before each
# boot (QEMU gets a bochs display; nothing presses a key).
set -eu
UBOOT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
MENU=${2:-}
Q=$(cd "$(dirname "$0")" && pwd)
D=$(cd "$Q/../.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fail() { echo "qemu-test: FAIL: $*"; [ -f "$W/log" ] && tail -40 "$W/log"; exit 1; }
AB="python3 -I $D/tools/abctl.py"
mkdir -p "$W/bin"
ln -s "$(command -v gawk)" "$W/bin/awk"

# --- The stand-in kernels. ---
gcc -c -o "$W/payload.o" "$Q/payload.S"
ld -Ttext=0 -e _start -o "$W/payload.elf" "$W/payload.o"
objcopy -O binary "$W/payload.elf" "$W/payload.bin"
dtc -q -I dts -O dtb -o "$W/slot.dtb" "$Q/slot.dts"
img() { python3 -I "$Q/mkbootimg.py" --payload "$W/payload.bin" --dtb "$W/slot.dtb" --tag "$1" --out "$2"; }
img "slot-a" "$W/a.img"
img "slot-b" "$W/b.img"
img "rescue" "$W/rescue.img"
img "abboot-qemu-self" "$W/self.img"	# carries CONFIG_AB_SELF_MARKER: a "U-Boot image"

# --- The disk: boot (U-Boot's own, empty here), misc, recovery, system (the store). ---
# Sectors: boot 2048+2048, misc 4096+2048, recovery 6144+8192, system 14336+65536.
P_MISC=4096; P_REC=6144; P_SYS=14336
mkdir -p "$W/store/slots/a/boot" "$W/store/slots/b/boot"
cp "$W/a.img" "$W/store/slots/a/boot/boot.img"
cp "$W/b.img" "$W/store/slots/b/boot/boot.img"
echo "test build a" > "$W/store/slots/a/boot/label"
echo "test build b" > "$W/store/slots/b/boot/label"
truncate -s 1M "$W/misc"
truncate -s 4M "$W/recovery"
dd if="$W/rescue.img" of="$W/recovery" conv=notrunc 2>/dev/null
truncate -s 41M "$W/disk.img"
sgdisk -n 1:2048:4095 -c 1:boot -n 2:4096:6143 -c 2:misc -n 3:6144:14335 -c 3:recovery \
	-n 4:14336:79871 -c 4:system "$W/disk.img" >/dev/null 2>&1 || fail "sgdisk"

# Write misc, recovery and a fresh store image into the disk.
put() {
	dd if="$W/misc" of="$W/disk.img" bs=512 seek=$P_MISC conv=notrunc 2>/dev/null
	dd if="$W/recovery" of="$W/disk.img" bs=512 seek=$P_REC conv=notrunc 2>/dev/null
	rm -f "$W/store.img"
	mkfs.ext4 -q -F -d "$W/store" "$W/store.img" 32M > /dev/null 2>&1
	dd if="$W/store.img" of="$W/disk.img" bs=512 seek=$P_SYS conv=notrunc 2>/dev/null
}
# misc back out of the disk after a boot.
get() { dd if="$W/disk.img" of="$W/misc" bs=512 skip=$P_MISC count=2048 2>/dev/null; }

N=0
# boot [timeout]: one boot in QEMU, the console in $W/log. 0: QEMU exited (the payload powered off).
boot() {
	put
	N=$((N + 1))
	set -- ${MENU:+-device bochs-display,romfile= -display none}
	set +e
	timeout "${TIMEOUT:-120}" qemu-system-aarch64 -M virt -cpu cortex-a57 -m 1G -nographic \
		-no-reboot -nic none -bios "$UBOOT" -drive if=none,file="$W/disk.img",format=raw,id=hd0 \
		-device virtio-blk-device,drive=hd0 "$@" < /dev/null > "$W/log" 2>&1
	rc=$?
	set -e
	tr -d '\r' < "$W/log" > "$W/log.txt"; mv "$W/log.txt" "$W/log"
	cp "$W/log" "$W/log.$N"
	get
}
ran() { grep -q "^PAYLOAD $1" "$W/log" || fail "boot $N: expected the $1 kernel"; }
said() { grep -qF "$1" "$W/log" || fail "boot $N: no \"$1\" on the console"; }
field() { dd if="$W/misc" bs=512 skip=4 count=1 2>/dev/null | $AB show |
	sed -n "s/^slot $1: priority *\([0-9]*\) *tries \([0-9]\) *successful \([01]\).*/p\1 t\2 s\3/p"; }
last() { dd if="$W/misc" bs=512 skip=4 count=1 2>/dev/null | $AB show | sed -n 's/^last booted: //p'; }
# bootctl <booted slot> <args...>: bootctl on the misc image, as the OS of that slot would run it.
bootctl() {
	echo "console=ttyAMA0 ab.slot=$1 ab.kernel=$1" > "$W/cmdline"; shift
	PATH="$W/bin:$PATH" BOOTCTL_CONF=/nonexistent BOOTCTL_CMDLINE="$W/cmdline" \
		sh "$D/tools/bootctl" -d "$W/misc" "$@" > /dev/null || fail "bootctl $*"
}
args() { sed -n 's/^abboot: bootargs: //p' "$W/log"; }

# 1. A first boot, misc empty: the default record is written, slot a booted on trial.
boot
ran slot-a
said "misc record none"
said 'slot a label "test build a"'
[ "$(field a)" = "p15 t2 s0" ] && [ "$(field b)" = "p14 t3 s0" ] && [ "$(last)" = a ] ||
	fail "first boot: a $(field a), b $(field b), last $(last)"
args | grep -q "ab.slot=a ab.kernel=a" || fail "command line: $(args)"
args | grep -q "^console=ttyAMA0 panic=5 ab.slot" || fail "command line: $(args)"
said "abboot-qemu: 2 nodes from QEMU's tree added"
python3 -c "import sys; b=open(sys.argv[1],'rb').read(); sys.exit(0 if b'UBC1' in b[1016*512:1018*512] and b[896*512:896*512+7]==b'UBLOG01' else 1)" "$W/misc" ||
	fail "no crumbs or console record in misc"
echo "ok 1: first boot: default record written, slot a on trial, crumbs and console record in misc"

# 2. The OS marks slot a good: the next boot is a's again, and nothing is written.
bootctl a mark-good
boot
ran slot-a
said "record unchanged, not written"
[ "$(field a)" = "p15 t2 s1" ] || fail "a good: a $(field a)"
echo "ok 2: a good slot boots without a write"

# 3. An update into slot b (set-active b) whose OS never marks it good: three tries, then back to a.
bootctl a set-active b
for t in 2 1 0; do
	boot
	ran slot-b
	[ "$(field b)" = "p15 t$t s0" ] || fail "trial of b: b $(field b), want t$t"
done
boot
ran slot-a
# With the menu the note is drawn above it on the screen, not printed on the console.
[ -n "$MENU" ] || said "Slot B failed, back to slot A"
[ "$(field b)" = "p0 t0 s0" ] && [ "$(field a)" = "p14 t2 s1" ] && [ "$(last)" = a ] ||
	fail "fallback: a $(field a), b $(field b)"
boot
ran slot-a
grep -q "Slot B failed" "$W/log" && fail "the fallback was announced twice"
echo "ok 3: three tries of slot b, then back to slot a, announced once"

# 4. Slot b set active again, its kernel replaced by a U-Boot image: refused, slot a booted whole.
cp "$W/self.img" "$W/store/slots/b/boot/boot.img"
bootctl a set-active b
boot
ran slot-a
said "a U-Boot image"
said "Booting slot A instead"
args | grep -q "ab.slot=a ab.kernel=a" || fail "command line: $(args)"
[ "$(field b)" = "p15 t2 s0" ] || fail "b's try was not taken: b $(field b)"
echo "ok 4: a slot holding a U-Boot image is never booted; the other slot boots instead"

# 5. Slot b's kernel missing: the same, "no kernel of its own".
rm "$W/store/slots/b/boot/boot.img"
boot
ran slot-a
said "Slot B has no kernel of its own"
echo "ok 5: a slot without a kernel: the other slot boots"

# 6. Both slots bad: the rescue kernel, no ab.slot=.
cp "$W/b.img" "$W/store/slots/b/boot/boot.img"
bootctl a mark-bad a
bootctl a mark-bad b
boot
ran rescue
said "No slot can start"
args | grep -q "ab.kernel=rescue" && ! args | grep -q "ab.slot=" || fail "rescue command line: $(args)"
echo "ok 6: no bootable slot: the rescue kernel, without ab.slot="

# 7. No rescue kernel either: U-Boot stays (no reset loop); QEMU is stopped by the timeout.
truncate -s 0 "$W/recovery"; truncate -s 4M "$W/recovery"
TIMEOUT=40 boot
[ $rc = 124 ] || fail "QEMU exited ($rc): U-Boot did not stay"
said "staying here"
echo "ok 7: no kernel at all: the stay screen, no reset"

# 8. Other data in misc 2048-4095 and no record: booted from the default record, nothing written.
dd if="$W/rescue.img" of="$W/recovery" conv=notrunc 2>/dev/null
truncate -s 0 "$W/misc"; truncate -s 1M "$W/misc"
printf 'someone else' | dd of="$W/misc" bs=1 seek=3000 conv=notrunc 2>/dev/null
before=$(sha256sum "$W/misc" | cut -d' ' -f1)
boot
ran slot-a
said "never written"
dd if="$W/misc" bs=1 skip=2048 count=2048 2>/dev/null | cmp -s - <(dd if=/dev/zero bs=1 count=952 2>/dev/null; printf 'someone else'; dd if=/dev/zero bs=1 count=1084 2>/dev/null) ||
	fail "misc 2048-4095 was written"
echo "ok 8: misc 2048-4095 holding other data is never written"

[ -n "$MENU" ] && { grep -q "abboot: menu: timeout" "$W/log.1" || fail "the menu did not count down"; echo "ok: the menu counted down on every boot"; }
echo "qemu-test: all passed ($N boots)"
