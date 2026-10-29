#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host test of the boot layer's A/B record code (abboot/ab.c) and its agreement with abctl.py.
# (bootctl's agreement with both: tests/bootctl-test.sh.)
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -Wall -Werror -DAB_HOST -I"$D/../abboot" -o "$T/ab-test" "$D/ab-test.c" "$D/../abboot/ab.c" -lz
"$T/ab-test" > "$T/out"
cat "$T/out"
AB="python3 -I $D/../tools/abctl.py"
c=$(sed -n 's/^default //p' "$T/out")
p=$($AB default 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$c" = "$p" ] || { echo "abctl.py default: $p, ab.c: $c"; exit 1; }
c=$(sed -n 's/^b-good-good //p' "$T/out")
p=$($AB set a --priority 14 --tries 0 --successful 1 < /dev/null 2>/dev/null |
	$AB set b --priority 15 --tries 0 --successful 1 --last b 2>/dev/null | head -c 32 | od -An -tx1 | tr -d ' \n')
[ "$c" = "$p" ] || { echo "abctl.py b good good: $p, ab.c: $c"; exit 1; }
echo "ab-test.sh: ab.c and abctl.py agree"
