#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host test of the menu's keys (abboot/menu.c: Up, Down, both together; tests/menu-test.c).
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -Wall -Werror -DAB_HOST -I"$D/../abboot" -o "$T/menu-test" "$D/menu-test.c" "$D/../abboot/menu.c"
"$T/menu-test"
