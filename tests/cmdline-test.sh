#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host test of the kernel command line helpers (abboot/cmdline.c; tests/cmdline-test.c).
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -Wall -Werror -DAB_HOST -I"$D/../abboot" -o "$T/cmdline-test" "$D/cmdline-test.c" "$D/../abboot/cmdline.c"
"$T/cmdline-test"
