#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Every host test: the A/B record (C and Python agree), the menu's keys, the command line helpers,
# bootctl, and the OS example (examples/update-slot.sh). Needs a C compiler, zlib and python3; bootctl-test.sh also needs busybox, gawk or
# Docker (it says SKIP otherwise).
set -eu
D=$(cd "$(dirname "$0")" && pwd)
for t in ab-test menu-test cmdline-test bootctl-test example-test; do
	echo "== $t"
	"$D/$t.sh"
done
echo "== all host tests passed"
