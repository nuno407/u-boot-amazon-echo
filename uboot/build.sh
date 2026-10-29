#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build mainline U-Boot v2026.10 with abboot for the QEMU example board, and run the QEMU tests.
#
#   uboot/build.sh [--out DIR] [--no-test]
#
# Everything runs in Docker (uboot/docker/Dockerfile, image u-boot-amazon-echo-build). The U-Boot
# tree is a clone in the Docker volume u-boot-amazon-echo-src, checked against the commit of the
# v2026.10 tag; each run resets it, installs abboot (uboot/install.sh) and builds out of tree:
#   qemu        qemu_arm64_abboot_defconfig: abboot, the diagnostics in misc, no menu
#   qemu-menu   the same with CONFIG_AB_MENU on QEMU's bochs display
# DIR (default ./out) gets u-boot-<variant>.bin and .config-<variant>.
set -eu
UBOOT_TAG=v2026.10
UBOOT_SHA=5508406582f6f7120dce0c4b3059819a41788db2
IMAGE=u-boot-amazon-echo-build
VOLUME=u-boot-amazon-echo-src

D=$(cd "$(dirname "$0")/.." && pwd)
OUT=$D/out; TEST=1
while [ $# -gt 0 ]; do
	case $1 in
	--out) OUT=$2; shift 2;;
	--no-test) TEST=; shift;;
	*) echo "usage: $0 [--out DIR] [--no-test]" >&2; exit 2;;
	esac
done
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

docker image inspect $IMAGE >/dev/null 2>&1 || docker build -q -t $IMAGE "$D/uboot/docker"
docker volume create $VOLUME >/dev/null

docker run --rm --platform linux/arm64 -v $VOLUME:/src -v "$D:/host:ro" -v "$OUT:/out" \
	-e UBOOT_TAG=$UBOOT_TAG -e UBOOT_SHA=$UBOOT_SHA -e TEST=$TEST -w /src $IMAGE sh -euc '
	git config --global --add safe.directory "*"
	[ -d u-boot/.git ] || git clone -q --depth 1 --branch $UBOOT_TAG https://github.com/u-boot/u-boot.git u-boot
	cd u-boot
	[ "$(git rev-parse HEAD)" = "$UBOOT_SHA" ] || { echo "u-boot is not $UBOOT_TAG ($UBOOT_SHA)"; exit 1; }
	git checkout -q -f HEAD && git clean -q -fdx
	sh /host/uboot/install.sh --hooks --example-board .
	: > .scmversion
	export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
	for v in qemu qemu-menu; do
		o=/src/out-$v
		rm -rf $o
		make -s O=$o qemu_arm64_abboot_defconfig
		[ $v = qemu-menu ] && scripts/config --file $o/.config --enable AB_MENU \
			--enable VIDEO_FONT_16X32 --enable VIDEO_FONT_8X16
		make -s O=$o olddefconfig
		grep -q "^CONFIG_ABBOOT=y" $o/.config || { echo "$v: ABBOOT did not stick"; exit 1; }
		[ $v = qemu ] || grep -q "^CONFIG_AB_MENU=y" $o/.config || { echo "$v: AB_MENU did not stick"; exit 1; }
		make -s O=$o -j$(nproc) KCFLAGS=-Werror u-boot.bin
		cp $o/u-boot.bin /out/u-boot-$v.bin
		cp $o/.config /out/.config-$v
		echo "built $v: $(stat -c %s $o/u-boot.bin) bytes"
	done
	if [ -n "$TEST" ]; then
		bash /host/tests/qemu/qemu-test.sh /out/u-boot-qemu.bin
		bash /host/tests/qemu/qemu-test.sh /out/u-boot-qemu-menu.bin menu
	fi
'
