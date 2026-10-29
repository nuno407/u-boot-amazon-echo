#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Put the abboot boot layer into a U-Boot source tree (tested against v2026.10):
#
#   uboot/install.sh [--hooks] [--example-board] <u-boot tree>
#
#   - copies abboot/ to <tree>/boot/abboot/ and applies uboot/patches/0001 (Kconfig, Makefile);
#   - --hooks: also uboot/patches/0002, the panic and exception hooks (only for CONFIG_AB_CRUMBS);
#   - --example-board: also the QEMU example board (boards/qemu-arm64/): its hooks into
#     board/emulation/qemu-arm/, its defconfig into configs/, and its Makefile patch.
#
# The tree should be clean (git checkout -f; git clean -fdx): the patches are applied with
# git apply, which refuses to apply twice.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
HOOKS=; EXAMPLE=
while [ $# -gt 1 ]; do
	case $1 in
	--hooks) HOOKS=1;;
	--example-board) EXAMPLE=1;;
	*) echo "usage: $0 [--hooks] [--example-board] <u-boot tree>" >&2; exit 2;;
	esac
	shift
done
[ $# -eq 1 ] && [ -f "$1/boot/Kconfig" ] || { echo "usage: $0 [--hooks] [--example-board] <u-boot tree>" >&2; exit 2; }
T=$(cd "$1" && pwd)

mkdir -p "$T/boot/abboot"
cp "$D"/abboot/*.c "$D"/abboot/*.h "$D/abboot/Kconfig" "$D/abboot/Makefile" "$T/boot/abboot/"
(cd "$T" && git apply "$D/uboot/patches/0001-boot-build-the-abboot-boot-layer.patch")
[ -z "$HOOKS" ] || (cd "$T" && git apply "$D/uboot/patches/0002-arm-panic-and-exception-hooks-for-boards.patch")
if [ -n "$EXAMPLE" ]; then
	cp "$D/boards/qemu-arm64/abboot-qemu.c" "$T/board/emulation/qemu-arm/"
	cp "$D/boards/qemu-arm64/qemu_arm64_abboot_defconfig" "$T/configs/"
	(cd "$T" && git apply "$D/boards/qemu-arm64/0001-qemu-arm-build-the-abboot-example-hooks.patch")
fi
echo "abboot installed in $T${HOOKS:+ (with the panic and exception hooks)}${EXAMPLE:+ (with the QEMU example board)}"
