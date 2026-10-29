#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Put the abboot boot layer into a U-Boot source tree (tested against v2026.10):
#
#   uboot/install.sh [--hooks] [--example-board] [--echo-boards] <u-boot tree>
#
#   - copies abboot/ to <tree>/boot/abboot/ and applies uboot/patches/0001 (Kconfig, Makefile) and
#     0003 (debug output from the boot path, only for CONFIG_AB_DEBUG_BOOT builds);
#   - --hooks: also uboot/patches/0002, the panic and exception hooks (only for CONFIG_AB_CRUMBS);
#   - --example-board: also the QEMU example board (boards/qemu-arm64/): its hooks into
#     board/emulation/qemu-arm/, its defconfig into configs/, and its Makefile patch;
#   - --echo-boards: also the two Echo boards (boards/amazon-echo-show-8-gen2/ and
#     boards/amazon-echo-show-5-gen2/): each board's u-boot/ directory copied over the tree (board/,
#     configs/, arch/arm/dts/, include/configs/), then boards/mediatek/*.patch (the MediaTek
#     watchdog and memory-map changes, and the Kconfig and device tree lines of both boards).
#
# The tree should be clean (git checkout -f; git clean -fdx): the patches are applied with
# git apply, which refuses to apply twice.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
HOOKS=; EXAMPLE=; ECHO=
USAGE="usage: $0 [--hooks] [--example-board] [--echo-boards] <u-boot tree>"
while [ $# -gt 1 ]; do
	case $1 in
	--hooks) HOOKS=1;;
	--example-board) EXAMPLE=1;;
	--echo-boards) ECHO=1;;
	*) echo "$USAGE" >&2; exit 2;;
	esac
	shift
done
[ $# -eq 1 ] && [ -f "$1/boot/Kconfig" ] || { echo "$USAGE" >&2; exit 2; }
T=$(cd "$1" && pwd)

mkdir -p "$T/boot/abboot"
cp "$D"/abboot/*.c "$D"/abboot/*.h "$D/abboot/Kconfig" "$D/abboot/Makefile" "$T/boot/abboot/"
(cd "$T" && git apply "$D/uboot/patches/0001-boot-build-the-abboot-boot-layer.patch")
(cd "$T" && git apply "$D/uboot/patches/0003-boot-debug-output-from-the-boot-path.patch")
[ -z "$HOOKS" ] || (cd "$T" && git apply "$D/uboot/patches/0002-arm-panic-and-exception-hooks-for-boards.patch")
if [ -n "$EXAMPLE" ]; then
	cp "$D/boards/qemu-arm64/abboot-qemu.c" "$T/board/emulation/qemu-arm/"
	cp "$D/boards/qemu-arm64/qemu_arm64_abboot_defconfig" "$T/configs/"
	(cd "$T" && git apply "$D/boards/qemu-arm64/0001-qemu-arm-build-the-abboot-example-hooks.patch")
fi
if [ -n "$ECHO" ]; then
	for b in amazon-echo-show-8-gen2 amazon-echo-show-5-gen2; do
		cp -R "$D/boards/$b/u-boot/." "$T/"
	done
	for p in "$D"/boards/mediatek/*.patch; do
		(cd "$T" && git apply "$p")
	done
fi
echo "abboot installed in $T${HOOKS:+ (with the panic and exception hooks)}${EXAMPLE:+ (with the QEMU example board)}${ECHO:+ (with the Echo boards)}"
