#!/bin/sh
# SPDX-License-Identifier: MIT
# An example of the OS side of the boot layer (SPEC.md, "What the OS does"): put a new system into
# the slot that is not running, tell the boot layer to try it, and confirm it once it works.
# A real OS adds its own checks (signatures, free space, what "healthy" means).
#
#   update-slot.sh install <boot.img> <rootfs.tar.gz> [label]
#   update-slot.sh commit
#
# STORE: where the store partition is mounted, read-write (default /mnt/store).
# BOOTCTL: the bootctl to run (default: bootctl on PATH).
set -eu
STORE=${STORE:-/mnt/store}
BOOTCTL=${BOOTCTL:-bootctl}
die() { echo "update-slot: $*" >&2; exit 1; }

booted=$($BOOTCTL current) || die "not started by the boot layer (no ab.slot= on the command line)"
case $booted in a) other=b;; b) other=a;; *) die "unexpected slot $booted";; esac

case ${1:-} in
install)
	[ $# -ge 3 ] || die "usage: update-slot.sh install <boot.img> <rootfs.tar.gz> [label]"
	img=$2; tar=$3; label=${4:-$(date '+%-d %b %Y')}
	s=$STORE/slots/$other
	# The slot is first marked bad, so a half-written slot is never booted.
	$BOOTCTL -q mark-bad "$other" || true
	rm -rf "$s"
	mkdir -p "$s/boot"
	tar -xzf "$tar" -C "$s"
	cp "$img" "$s/boot/boot.img"
	printf '%s\n' "$label" > "$s/boot/label"
	sync
	$BOOTCTL set-active "$other"
	echo "update-slot: slot $other installed; it is tried on the next boot"
	;;
commit)
	$BOOTCTL mark-good "$booted"
	;;
*)
	die "usage: update-slot.sh install <boot.img> <rootfs.tar.gz> [label] | commit"
	;;
esac
