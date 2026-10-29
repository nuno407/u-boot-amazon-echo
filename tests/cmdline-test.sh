#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host test of the kernel's command line (abboot/cmdline.c; tests/cmdline-test.c), its lines compared
# with cmdline-test.expected. CMDLINE_TEST_WRITE=1 writes that file anew (check the diff first).
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -Wall -Werror -DAB_HOST -I"$D/../abboot" -o "$T/cmdline-test" "$D/cmdline-test.c" "$D/../abboot/cmdline.c"
"$T/cmdline-test" > "$T/out"
grep -v "^= " "$T/out" || true	# the code's own log
grep '^= ' "$T/out" > "$T/lines"
if [ "${CMDLINE_TEST_WRITE:-}" ]; then cp "$T/lines" "$D/cmdline-test.expected"; echo "cmdline-test: expected written"; exit 0; fi
diff "$D/cmdline-test.expected" "$T/lines" || { echo "cmdline-test: the command lines changed"; exit 1; }
echo "cmdline-test: $(grep -c '^= .*: ' "$T/lines") command lines byte for byte as expected"
