#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host test of the kernel's device tree (abboot/dt.c, ab_dt_merge; tests/dt-test.c): a slot's tree
# (tests/dt/slot.dts) gets what the prior stage's tree (prior.dts) adds to the packed one (base.dts),
# with CONFIG_AB_DT_CARRY "tuning", compared with tests/dt/expected.dts. Then without CARRY: the
# slot's own tuning stays. Needs dtc and libfdt: run by uboot/build.sh in its Docker image, with
# libfdt from the U-Boot tree (LIBFDT, default /src/u-boot/scripts/dtc/libfdt).
set -eu
D=$(cd "$(dirname "$0")" && pwd)
L=${LIBFDT:-/src/u-boot/scripts/dtc/libfdt}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -O1 -Wall -Werror -DAB_HOST -I"$L" -I"$D/../abboot" -o "$T/dt-test" "$D/dt-test.c" "$D/../abboot/dt.c" \
	"$L/fdt.c" "$L/fdt_ro.c" "$L/fdt_rw.c" "$L/fdt_wip.c" "$L/fdt_strerror.c"
for t in base slot prior; do dtc -q -I dts -O dtb -o "$T/$t.dtb" "$D/dt/$t.dts"; done
"$T/dt-test" "$T/base.dtb" "$T/slot.dtb" "$T/prior.dtb" "$T/out.dtb" tuning
dtc -q -I dtb -O dts -s "$T/out.dtb" > "$T/out.dts"
diff "$D/dt/expected.dts" "$T/out.dts" || { echo "dt-test: FAIL: the merged tree differs"; exit 1; }
"$T/dt-test" "$T/base.dtb" "$T/slot.dtb" "$T/prior.dtb" "$T/out2.dtb"
dtc -q -I dtb -O dts -s "$T/out2.dtb" | grep -q "tuning = \[00 00 00 00 00 00\]" ||
	{ echo "dt-test: FAIL: without CARRY the slot's tuning was replaced"; exit 1; }
echo "dt-test: the prior stage's additions merged, phandles left out, CARRY honored"
