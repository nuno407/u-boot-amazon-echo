#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# examples/update-slot.sh against a store directory and a misc image: install into the slot not
# running (set-active, label, kernel where the boot layer reads it), then commit (mark-good).
# Needs the same awk as bootctl-test.sh (busybox or gawk; SKIP otherwise).
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fail() { echo "example-test: FAIL: $*"; exit 1; }
AB="python3 -I $D/tools/abctl.py"
mkdir -p "$W/bin"
if command -v gawk >/dev/null 2>&1; then ln -s "$(command -v gawk)" "$W/bin/awk"
elif command -v busybox >/dev/null 2>&1; then ln -s "$(command -v busybox)" "$W/bin/awk"
else echo "example-test: SKIP: needs busybox or gawk"; exit 0
fi
truncate -s 1M "$W/misc"
cat > "$W/bootctl" <<EOS
#!/bin/sh
BOOTCTL_CONF=/nonexistent BOOTCTL_CMDLINE="$W/cmdline" exec sh "$D/tools/bootctl" -d "$W/misc" "\$@"
EOS
chmod +x "$W/bootctl"
export PATH="$W/bin:$PATH" STORE="$W/store" BOOTCTL="$W/bootctl"
"$W/bootctl" init > /dev/null
"$W/bootctl" mark-good a > /dev/null
echo "console=ttyS0 ab.slot=a ab.kernel=a" > "$W/cmdline"
mkdir -p "$W/root/etc"; echo hello > "$W/root/etc/hello"
tar -czf "$W/rootfs.tar.gz" -C "$W/root" .
echo "not a real image" > "$W/boot.img"
field() { dd if="$W/misc" bs=512 skip=4 count=1 2>/dev/null | $AB show |
	sed -n "s/^slot $1: priority *\([0-9]*\) *tries \([0-9]\) *successful \([01]\).*/p\1 t\2 s\3/p"; }

sh "$D/examples/update-slot.sh" install "$W/boot.img" "$W/rootfs.tar.gz" "test label" > /dev/null
[ "$(cat "$W/store/slots/b/boot/label")" = "test label" ] || fail "label"
cmp -s "$W/boot.img" "$W/store/slots/b/boot/boot.img" || fail "boot.img"
[ -f "$W/store/slots/b/etc/hello" ] || fail "rootfs"
[ "$(field b)" = "p15 t3 s0" ] && [ "$(field a)" = "p14 t3 s1" ] || fail "install: a $(field a), b $(field b)"
echo "ok: install into slot b: files in place, b on trial"

# The boot layer now boots b (one try taken); the OS of slot b decides it works and commits.
echo "console=ttyS0 ab.slot=b ab.kernel=b" > "$W/cmdline"
sh "$D/examples/update-slot.sh" commit > /dev/null
[ "$(field b)" = "p15 t3 s1" ] || fail "commit: b $(field b)"
echo "ok: commit marks the running slot good"
echo "example-test: all passed"
