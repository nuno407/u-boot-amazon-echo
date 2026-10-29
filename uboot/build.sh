#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build mainline U-Boot v2026.10 with abboot for the QEMU example board and the Echo boards, and run
# the QEMU tests.
#
#   uboot/build.sh [--out DIR] [--no-test] [--boards "LIST"] [--variants "LIST"]
#                  [--config FILE]... [--boot IMAGE]
#
# Everything but the packing runs in Docker (uboot/docker/Dockerfile, image
# u-boot-amazon-echo-build). The U-Boot tree is a clone in the Docker volume u-boot-amazon-echo-src,
# checked against the commit of the v2026.10 tag; each run resets it, installs abboot and the boards
# (uboot/install.sh --hooks --example-board --echo-boards) and builds out of tree.
#
# --boards (default: all three):
#   qemu              qemu_arm64_abboot_defconfig: abboot, the diagnostics in misc, no menu; and
#                     qemu-menu, the same with CONFIG_AB_MENU on QEMU's bochs display. Then the
#                     device tree test (tests/dt-test.sh, which needs the tree's libfdt) and the
#                     QEMU tests (tests/qemu/qemu-test.sh), unless --no-test.
#   echo-show-8-gen2  amazon_athena_defconfig (boards/amazon-echo-show-8-gen2/)
#   echo-show-5-gen2  amazon_cronos_defconfig (boards/amazon-echo-show-5-gen2/)
#
# --variants, for the Echo boards (default "test-ro test test-sticky final"): each is NAME, one of
#   final        for the boot partition: the defconfig as it is
#   test         for the recovery partition (uboot/variants/test.config)
#   test-ro      test, never writing the A/B record (test + readonly)
#   test-sticky  test, misc's boot-recovery set again before the kernel (test + sticky)
# or NAME=SPEC, SPEC a list of Kconfig fragments joined by "+": a name from uboot/variants/
# (test, readonly, sticky) or a path to a fragment file, applied in that order on the board's
# defconfig. An empty SPEC (NAME=) is the defconfig as it is.
#
# --config FILE (repeatable): a Kconfig fragment applied to every Echo variant after its own
# fragments, for a build's own values (for example CONFIG_AB_SELF_MARKER or CONFIG_AB_DT_CARRY).
# Fragments are lines as in .config: CONFIG_X=y, CONFIG_X="text", CONFIG_X=0x10,
# "# CONFIG_X is not set"; the build stops if a line does not hold in the final .config.
#
# --boot IMAGE (with one Echo board): pack each variant into a boot image with tools/pack-uboot.py,
# from IMAGE, a copy of your own device's boot partition, with the board's packing options. The
# packed images hold your device's own data: keep them with your backups.
#
# DIR (default ./out) gets, per variant, u-boot-<board>-<variant>.bin, .config-<board>-<variant>
# and System.map-<board>-<variant> (u-boot-qemu.bin, u-boot-qemu-menu.bin for QEMU), the packed
# uboot-<board>-<variant>.img with --boot, and SHA256SUMS.
set -eu
UBOOT_TAG=v2026.10
UBOOT_SHA=5508406582f6f7120dce0c4b3059819a41788db2
IMAGE=u-boot-amazon-echo-build
VOLUME=u-boot-amazon-echo-src

D=$(cd "$(dirname "$0")/.." && pwd)
OUT=$D/out; TEST=1; BOARDS="qemu echo-show-8-gen2 echo-show-5-gen2"
VARIANTS="test-ro test test-sticky final"; CONFIGS=; BOOT=
USAGE="usage: $0 [--out DIR] [--no-test] [--boards LIST] [--variants LIST] [--config FILE]... [--boot IMAGE]"
while [ $# -gt 0 ]; do
	case $1 in
	--out) OUT=$2; shift 2;;
	--no-test) TEST=; shift;;
	--boards) BOARDS=$2; shift 2;;
	--variants) VARIANTS=$2; shift 2;;
	--config) [ -f "$2" ] || { echo "$0: no such fragment: $2" >&2; exit 2; }
		CONFIGS="$CONFIGS $(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2;;
	--boot) BOOT=$2; shift 2;;
	*) echo "$USAGE" >&2; exit 2;;
	esac
done
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# The Echo boards: defconfig, and the options pack-uboot.py needs for the board's boot images.
board_defconfig() {
	case $1 in
	echo-show-8-gen2) echo amazon_athena_defconfig;;
	echo-show-5-gen2) echo amazon_cronos_defconfig;;
	*) return 1;;
	esac
}
board_pack() {
	case $1 in
	echo-show-8-gen2) echo "";;
	echo-show-5-gen2) echo "--header-at 0x400";;
	esac
}
ECHO=
for b in $BOARDS; do
	case $b in
	qemu) ;;
	*) board_defconfig "$b" >/dev/null || { echo "$0: unknown board $b" >&2; exit 2; }
		ECHO="$ECHO $b";;
	esac
done
if [ -n "$BOOT" ]; then
	[ -f "$BOOT" ] || { echo "$0: no such boot image: $BOOT" >&2; exit 2; }
	[ "$(echo $ECHO | wc -w)" -eq 1 ] || { echo "$0: --boot needs exactly one Echo board" >&2; exit 2; }
fi

# The fragments, staged for Docker: frag/<n>.config, and plan.txt with one line per Echo build,
# "<board> <defconfig> <variant> <fragment>...".
S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT
mkdir "$S/frag"
N=0
stage() {	# stage FILE: copies it in, and appends its name in Docker to $line
	N=$((N + 1))
	cp "$1" "$S/frag/$N.config"
	line="$line /frag/$N.config"
}
: > "$S/plan.txt"
for b in $ECHO; do
	for v in $VARIANTS; do
		name=${v%%=*}
		case $v in
		*=*) spec=${v#*=};;
		final) spec=;;
		test) spec=test;;
		test-ro) spec=test+readonly;;
		test-sticky) spec=test+sticky;;
		*) echo "$0: unknown variant $v (NAME=SPEC defines one)" >&2; exit 2;;
		esac
		case $name in ''|*[!A-Za-z0-9_.-]*) echo "$0: bad variant name '$name'" >&2; exit 2;; esac
		line="$b $(board_defconfig "$b") $name"
		for f in $(echo "$spec" | tr '+' ' '); do
			case $f in
			*/*|*.config) [ -f "$f" ] || { echo "$0: no such fragment: $f" >&2; exit 2; }; p=$f;;
			*) p=$D/uboot/variants/$f.config
				[ -f "$p" ] || { echo "$0: no such variant fragment: $f" >&2; exit 2; };;
			esac
			stage "$p"
		done
		for f in $CONFIGS; do stage "$f"; done
		echo "$line" >> "$S/plan.txt"
	done
done
case " $BOARDS " in *" qemu "*) QEMU=1;; *) QEMU=;; esac

docker image inspect $IMAGE >/dev/null 2>&1 || docker build -q -t $IMAGE "$D/uboot/docker"
docker volume create $VOLUME >/dev/null

docker run --rm --platform linux/arm64 -v $VOLUME:/src -v "$D:/host:ro" -v "$OUT:/out" \
	-v "$S/frag:/frag:ro" -v "$S/plan.txt:/plan.txt:ro" \
	-e UBOOT_TAG=$UBOOT_TAG -e UBOOT_SHA=$UBOOT_SHA -e TEST=$TEST -e QEMU=$QEMU -w /src $IMAGE sh -euc '
	git config --global --add safe.directory "*"
	[ -d u-boot/.git ] || git clone -q --depth 1 --branch $UBOOT_TAG https://github.com/u-boot/u-boot.git u-boot
	cd u-boot
	[ "$(git rev-parse HEAD)" = "$UBOOT_SHA" ] || { echo "u-boot is not $UBOOT_TAG ($UBOOT_SHA)"; exit 1; }
	git checkout -q -f HEAD && git clean -q -fdx
	sh /host/uboot/install.sh --hooks --example-board --echo-boards .
	: > .scmversion   # the version reads "U-Boot 2026.10", not "-dirty"
	# U-Boot runs its scripts with whatever "awk" is: mawk, the Debian default, so that the build does
	# not depend on gawk being installed (with gawk the default environment comes out in another
	# order). The QEMU tests pick gawk themselves.
	mkdir -p /tmp/awk && ln -sf /usr/bin/mawk /tmp/awk/awk && export PATH=/tmp/awk:$PATH
	export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)

	# Every line of a fragment must hold in the final .config.
	check() {
		sed -e "/^[[:space:]]*$/d" -e "/^#[[:space:]]*CONFIG_[A-Za-z0-9_]* is not set/!{/^#/d}" "$2" |
		while IFS= read -r l; do
			case $l in
			"# CONFIG_"*) s=${l#\# }; s=${s%% *}
				! grep -q "^$s=" "$1" || { echo "$3: $l did not stick"; exit 1; };;
			*) grep -qxF "$l" "$1" || { echo "$3: $l did not stick"; exit 1; };;
			esac
		done
	}

	if [ -n "$QEMU" ]; then
		for v in qemu qemu-menu; do
			o=/src/out-$v
			rm -rf $o
			make -s O=$o qemu_arm64_abboot_defconfig
			# QEMU has no simple-framebuffer: the menu draws on the bochs display, unchecked.
			[ $v = qemu-menu ] && scripts/config --file $o/.config --enable AB_MENU \
				--enable VIDEO_FONT_16X32 --enable VIDEO_FONT_8X16 \
				--disable AB_SCREEN_NEED_RESERVED --disable AB_SCREEN_CLEAR
			make -s O=$o olddefconfig
			grep -q "^CONFIG_ABBOOT=y" $o/.config || { echo "$v: ABBOOT did not stick"; exit 1; }
			[ $v = qemu ] || grep -q "^CONFIG_AB_MENU=y" $o/.config || { echo "$v: AB_MENU did not stick"; exit 1; }
			make -s O=$o -j$(nproc) KCFLAGS=-Werror u-boot.bin
			cp $o/u-boot.bin /out/u-boot-$v.bin
			cp $o/.config /out/.config-$v
			echo "built $v: $(stat -c %s $o/u-boot.bin) bytes"
		done
	fi

	while read -r board defconfig v frags; do
		o=/src/out-$v
		rm -rf $o
		make -s O=$o $defconfig
		[ -z "$frags" ] || scripts/kconfig/merge_config.sh -m -O $o $o/.config $frags > /dev/null
		make -s O=$o olddefconfig
		grep -q "^CONFIG_ABBOOT=y" $o/.config || { echo "$board $v: ABBOOT did not stick"; exit 1; }
		for f in $frags; do check $o/.config $f "$board $v"; done
		make -s O=$o -j$(nproc) KCFLAGS=-Werror u-boot.bin
		cp $o/u-boot.bin /out/u-boot-$board-$v.bin
		cp $o/.config /out/.config-$board-$v
		nm -n $o/u-boot > /out/System.map-$board-$v   # symbols, for an exception crumb pc
		echo "built $board $v: $(stat -c %s $o/u-boot.bin) bytes"
		rm -rf $o
	done < /plan.txt

	if [ -n "$QEMU" ] && [ -n "$TEST" ]; then
		sh /host/tests/dt-test.sh
		bash /host/tests/qemu/qemu-test.sh /out/u-boot-qemu.bin
		bash /host/tests/qemu/qemu-test.sh /out/u-boot-qemu-menu.bin menu
	fi
'

if [ -n "$BOOT" ]; then
	b=$(echo $ECHO)
	while read -r _ _ v _; do
		marker=$(sed -n 's/^CONFIG_AB_SELF_MARKER="\(.*\)"$/\1/p' "$OUT/.config-$b-$v")
		python3 -I "$D/tools/pack-uboot.py" --boot "$BOOT" --uboot "$OUT/u-boot-$b-$v.bin" \
			--out "$OUT/uboot-$b-$v.img" --marker "$marker" $(board_pack "$b")
	done < "$S/plan.txt"
fi
cd "$OUT" && ls u-boot-*.bin uboot-*.img 2>/dev/null | xargs shasum -a 256 > SHA256SUMS && cat SHA256SUMS
