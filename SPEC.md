# The boot layer: specification

This is the exact behavior of the boot layer: the record it keeps, the files it reads, what U-Boot
does on each boot, and what the operating system (OS) is expected to do. [README.md](README.md)
is the friendlier introduction.

## Three layers

| Layer | What | Knows about |
| --- | --- | --- |
| **Boot** | U-Boot with `abboot`, the A/B record, the slot convention, the menu, the rescue kernel, the watchdog contract, `bootctl` | only the A/B record and the slot convention |
| **OS** | the kernel, the root filesystem, the init system, the update tool, the health check | the boot layer, through `bootctl` and the kernel command line |
| **App** | whatever the device is for | nothing about booting |

The boot layer reads only the A/B record in misc and the slot convention on the store. It can
boot any OS that puts its slots where the convention says and keeps the watchdog contract. The OS
tells it what it wants through `bootctl`; nothing else in the OS touches the record.

## The A/B record

32 bytes at **byte 2048 of the misc partition**, little-endian, in the layout of AOSP's
`struct bootloader_control` (the same place AOSP puts it: right after the 2 KB
`bootloader_message`). Bytes 2048-4095 hold the record and nothing else: no writer touches
2080-4095, and no writer writes a record there unless the area already holds a valid record or is
entirely zero.

| Byte | Field | |
| --- | --- | --- |
| 0-3 | `slot_suffix` | `_a\0\0` or `_b\0\0`: the slot U-Boot booted last (written by U-Boot only) |
| 4-7 | `magic` | `42 43 41 42` (0x42414342, "BCAB") |
| 8 | `version` | 1 |
| 9 | bits 0-2 `nb_slot`, bits 3-5 `recovery_tries_remaining` | nb_slot 2; the rest kept |
| 10-11 | `reserved0` (newer AOSP: byte 10 bits 0-2 `merge_status`) | kept |
| 12, 14 | slot a, slot b, byte 0: bits 0-3 `priority`, 4-6 `tries_remaining`, 7 `successful_boot` | |
| 13, 15 | slot a, slot b, byte 1: bit 0 `verity_corrupted` | kept, and honored: a corrupted slot is not bootable |
| 16-19 | `slot_info[2..3]` | kept |
| 20-27 | `reserved1` | **zero**: every writer zeroes it |
| 28-31 | `crc32_le` | CRC-32 (zlib's) of bytes 0-27 |

A slot is **bootable** when its priority is above 0, it is successful or has tries left, and it is
not verity-corrupted. The words the menu and the tools use: **good** (bootable, successful),
**trial** (bootable, not yet successful), **bad** (not bootable).

The **default record**: slot a priority 15, slot b priority 14, both 3 tries, neither
successful, last booted a. U-Boot writes it where 2048-4095 is all zero (the first boot through
the boot layer); `bootctl init` writes the same bytes. A record with a bad CRC, magic or version
next to an otherwise empty area is treated as none and replaced. One next to other data is never
written: U-Boot then boots from the default record kept in RAM and writes nothing.

Three implementations, kept in agreement by the host tests: `abboot/ab.c` (U-Boot, also built on
the host for `tests/ab-test.c`), `tools/bootctl` (shell and awk, on the device) and
`tools/abctl.py` (Python, on a computer).

## The slot convention

The **store** is one ext4 partition (`CONFIG_AB_STORE_PART`, default `system`). Slot `<x>`
(`a` or `b`) has a directory `slots/<x>/` at the store's root. The boot layer reads, read only:

| Path on the store | | |
| --- | --- | --- |
| `/slots/<x>/boot/boot.img` | required to boot the slot's own kernel | an Android boot image, header version 0 or 1, the header at byte 0 or at 0x400 (behind a 1 KB stub that some devices need in front of every boot image; offsets stay page offsets from byte 0), page size 2048-16384. The kernel part is a **gzip'd arm64 Image with the device tree (DTB) appended** (uncompressed, found by searching backwards from the end); the ramdisk is the slot's own initramfs; no second stage. The header id (mkbootimg's SHA-1) must match |
| `/slots/<x>/boot/label` | optional | one line, printable ASCII, the first 31 bytes shown: the menu says "Slot A (good) - &lt;label&gt;". The OS writes it when it installs the slot |

Nothing else on the store is read: no state file, no release file, no OS path. What else the slot
directory holds (usually the slot's whole root filesystem) is the OS's business. So are the
kernel's modules, if it has any: they may live in the slot, which is why U-Boot never runs one
slot's kernel with the other slot's root. A slot whose kernel cannot be used gives way to the other
slot whole (below).

U-Boot does not replay the ext4 journal: the OS writes slot files completely and syncs them, and
has the store unmounted, read-only or synced before the reboot that uses them.

The **rescue kernel** is a boot image of the same kind in its own partition
(`CONFIG_AB_RESCUE_PART`, default `recovery`), or, in test builds that run from that partition, a
file on the store (`CONFIG_AB_RESCUE_FILE`, for example `/rescue.img`).

## What U-Boot does (abboot)

The `abboot` command (`abboot/boot.c`), run as the boot command:

1. **The previous stage's device tree** (`board_ab_prior_fdt`; by default the one at
   `$prevbl_fdt_addr`, `CONFIG_SAVE_PREV_BL_FDT_ADDR`) is copied away, before anything can be
   loaded over it. abboot also reads back the device tree packed into its own boot image (in
   `CONFIG_AB_SELF_PART`), for step 8.
2. **The record** from misc (`CONFIG_AB_MISC_PART`); the default record if there is none.
3. **The pick**: the slot of highest priority (a tie goes to a). If it is not bootable it has
   **failed** (its tries ran out without a successful boot): the other slot is picked if
   bootable, and the failed one goes to priority 0, so the fallback is announced once ("Slot A
   failed, back to slot B", shown for at least `CONFIG_AB_WARN_MS`). No bootable slot: step 7.
4. **The menu** (`CONFIG_AB_MENU`, `abboot/menu.c`): "Slot A (word) - label" and "Slot B ...",
   always in that order, the bar on the pick; a bad slot gray and not selectable; "Boot in 5s.",
   counting down; a gray hint at the bottom (`CONFIG_AB_MENU_HINT`). Up or Down moves the bar,
   **both together** start the entry ("The menu's keys", below). Any key press stops the
   countdown; after that, 60 s without a press boots the selected entry, so an unattended device
   never waits forever. A manual choice is for this boot only: it changes no priority, and takes
   a try from a slot on trial like the pick would. When `board_ab_menu_visible()` says the screen
   shows nothing this boot, there is no menu: the pick boots at once ("The screen", below).
5. **The kernel**: the slot's own `boot/boot.img`. If it is missing or fails a check ("Slot A has
   no kernel of its own", "Slot A's kernel cannot be used"):
   - the other slot whole, if it is bootable ("Booting slot B instead", `ab.slot=b
     ab.kernel=b`); the first slot loses a try, as with any failure;
   - failing that, the rescue kernel ("Using the rescue kernel", `ab.kernel=rescue`);
   - failing that, the **stay screen** (U-Boot carries no kernel of its own): "No slot can start"
     and `CONFIG_AB_STAY_HINT`. U-Boot stays there, the watchdog fed, until the device is powered
     off: a reset would make the same decision from the same record and partitions, a loop.

   U-Boot never boots an image whose kernel holds `CONFIG_AB_SELF_MARKER` (a U-Boot image), and
   never loads `CONFIG_AB_SELF_PART` (the partition U-Boot is in) as a kernel.
6. **The try** (`ab_take`): the chosen slot loses one try unless it is successful, and becomes
   `slot_suffix`. The 32 bytes are written back (block 4 read, changed, written) only if they
   changed, so booting a good slot writes nothing. `CONFIG_AB_READONLY` never writes.
7. **No bootable slot**: "No slot can start", the rescue kernel, "Starting the rescue system";
   no `ab.slot=` (the OS's own fallback decides), no watchdog contract; the record is left as it
   is.
8. **The kernel's device tree** ("The device tree", below): the image's own DTB, opened with
   `CONFIG_AB_FDT_GROW` bytes of room, with what the previous stage added to the tree packed into
   U-Boot's image (`board_ab_fixup_fdt`; `abboot/dt.c`).
9. **The command line** ("The command line", below): the previous stage's `/chosen/bootargs`
   with the booted image's header command line in place of U-Boot's own, and the parameters of
   `CONFIG_AB_HEADER_KEYS` that header sets taken from it alone (`board_ab_bootargs`;
   `abboot/cmdline.c`), or the image's header command line if there is no previous tree. Then
   `init=$CONFIG_AB_INIT` if set, then for a slot the watchdog contract (`CONFIG_AB_SLOT_ARGS`,
   each token left out if its key is already there), then **`ab.slot=<a|b>`** (a slot's boot
   only) and **`ab.kernel=<a|b|rescue>`** (whose kernel runs), then a test build's marker
   (`CONFIG_AB_TESTSLOT`: `CONFIG_AB_TEST_MARKER`, default `ab.test=1`; `board_ab_extra_args`).
10. `booti`, with the ramdisk and device tree left in place and the watchdog running. Should
    anything fail after a kernel was chosen ("Could not start"): 10 s, then a reset
    (`CONFIG_AB_RESET_PSCI`: PSCI's SYSTEM_RESET, then the watchdog; `CONFIG_AB_RESET_WDT`: the
    watchdog only, for firmware whose PSCI has no SYSTEM_RESET).

### The command line

Some bootloaders build the kernel command line around the header command line of the boot image
they started. When U-Boot is packed as the kernel of such an image (the packer keeps the device's
own header), the line U-Boot is handed contains U-Boot's own header command line. abboot
(`ab_header_line`, `abboot/cmdline.c`) reads that line from the boot image in
`CONFIG_AB_SELF_PART` and puts the booted image's own header command line in its place. Then it
lets that header decide the parameters listed in `CONFIG_AB_HEADER_KEYS` (default `root=
rootfstype= rootflags= init= rdinit= rootwait ro|rw skip_initramfs`: a `key=` or a flag; `a|b` is
one group): a token of such a parameter elsewhere in the bootloader's line is dropped when the
header sets that parameter, so that the image decides, for example, its root. Parameters that may
appear more than once (`console=`) are never touched. If U-Boot's header line is not found in the
line, the line is kept as it is.

On the Echo Show 5 2nd gen every image carries the boot partition's header command line, so the
swap changes nothing and the line is the bootloader's (`root=/dev/ram`: the initramfs picks the
root). On the Echo Show 8 2nd gen the bootloader puts a root group in its line on a normal boot
and `root=/dev/ram` on a recovery boot; an image that needs its own root carries it in its header
command line (the OS writes it there when it installs the slot).

### The device tree

The previous stage hands U-Boot the device tree packed into U-Boot's own boot image (the device's
own boot image DTB, which the packer keeps) with its own additions. abboot reads that packed tree
back from the partition it was started from (appended to the kernel, searched backwards from the
end of the kernel area over its last 2 MB) and copies into the booted image's tree every property
of the previous stage's tree that the packed tree lacks or holds with another value, at the same
path (nodes made as needed, phandles left out, nothing deleted; `CONFIG_AB_DT_MERGE`). No names
are listed. On the Echo boards the additions are the root `model`, `/chosen`, `/memory`, the
device's factory data (`/idme`) and reserved memory blocks.

Per-device data that is in the packed tree itself is no addition. `CONFIG_AB_DT_CARRY` names such
properties (space-separated, default empty): each is copied from the previous stage's tree into
the booted tree's node that holds the same property with the same length. An OS that keeps, for
example, calibration data in the device's boot image tree names that property here. Without a
packed tree (none found) the booted image's own tree stands in for it. `tests/dt-test.sh` checks
the rule.

### The menu's keys

Two keys, Up and Down (on a phone-style device, the volume keys), and their chord: both together
are OK. The board gives the raw state only: `CONFIG_AB_KEY_UP_REG`/`_BIT` and
`CONFIG_AB_KEY_DOWN_REG`/`_BIT`, one MMIO bit each, active low unless `CONFIG_AB_KEYS_ACTIVE_LOW`
is off, read by the default `board_ab_keys()` (`abboot/board.c`; a register of 0 is no key).
Debouncing and the chord are `ab_keys_step` in `abboot/menu.c`, the same for every board, sampled
every 10 ms:

- Each key is debounced on its own: a change counts after three samples in a row (20-30 ms).
- A key already down when the menu appears (held for the bootloader before U-Boot) counts for
  nothing, and nothing counts until every key has been let go.
- **A key acts only when it is let go**; a press does nothing but stop the countdown. There is no
  timing window and no move on hold: while one key is held, the other may follow at any time. A
  press runs from the first key down to every key up.
- **Both keys down together at any moment of a press** (either first, the second at any delay):
  the chord, **OK when the last key is released**, whichever goes first, and no move for that
  press. While both are down the selected bar is inverted, so the user sees that letting go will
  start it.
- **Only one key down from press to release: one move**, at release, however long it was held.
  No repeat.

`tests/menu-test.sh` runs these cases on the host.

### The screen

The screen is the U-Boot device tree's `simple-framebuffer` node (address, size, stride, format,
rotation: the board's DTS). Before the menu draws, its `reg` region, and only that, is cleared to
opaque black (`CONFIG_AB_SCREEN_CLEAR`).

`board_ab_menu_visible()` is asked once, early in abboot. By default it is false when the previous
stage's tree does not reserve that region in `/reserved-memory` (`CONFIG_AB_SCREEN_NEED_RESERVED`:
U-Boot draws only into memory the previous stage set aside for its screen), or when the previous
stage's command line holds the token `CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG` (empty by default; the Echo
Show 5 2nd gen board sets `fps=6000`, which its bootloader's line carries on the boots whose screen
shows nothing). When it is false, abboot draws nothing at all: no video device is probed, there is
no menu (the pick boots at once), the notes ("Slot A failed, back to slot B", ...) go to the serial
console only and are not waited for, and the stay screen is not drawn, but U-Boot stays there all
the same, the watchdog fed.

### The watchdog contract

A hardware watchdog is a timer that resets the device unless software keeps restarting
("feeding") it; it is how a device recovers from a hang without anyone pressing a button.

The watchdog is never stopped. U-Boot takes over a watchdog the previous stage left running (or
starts one, `CONFIG_WATCHDOG_AUTOSTART`), feeds it while it works and waits, and starts the kernel
with it running. For a slot's boot the command line carries `CONFIG_AB_SLOT_ARGS`, for example:

    watchdog.open_timeout=120 <driver>.nowayout=1 panic=5

That is the boot layer's contract with the OS: the kernel feeds the watchdog only until user space
opens it, and for at most 120 s; once opened it cannot be closed; a kernel panic resets the device
in 5 s. **The OS must open and feed `/dev/watchdog` within 120 s of a slot's boot**, or the device
resets and U-Boot counts the try. The rescue system's command line has none of it: it must not
reset under the person using it.

### Diagnostics

With `CONFIG_AB_CRUMBS`, abboot leaves a trail in misc that can be read after a reset, from Linux
or from a copy of misc:

- **Crumbs**: one 32-byte entry per boot stage in misc blocks `CONFIG_AB_CRUMBS_BLOCK` and the
  next (default 1016-1017): `UBC1`, the stage number, `+` (ok) or `!` (failed), a little-endian
  u32 of milliseconds since the SoC came out of reset, 20 bytes of text. Entry n is stage n, a
  fresh set each boot.
- **The console record** (`CONFIG_CONSOLE_RECORD`): the newest U-Boot console output in
  `CONFIG_AB_LOG_BLOCKS` blocks from `CONFIG_AB_LOG_FIRST` (default 896-1015): a 16-byte header
  (`UBLOG01\0`, u32 length, u32 1 if the buffer overflowed), then the text. Written before
  booti, on failure, and on an exception or panic (with `uboot/patches/0002`).
- With `CONFIG_AB_CRUMBS_RAM`, the same stages are also kept in a RAM page from before
  relocation on.

Each area is written only while it is empty or already holds the boot layer's data.

The stage numbers (`abboot/crumbs.h`):

| | | | |
| --- | --- | --- | --- |
| 0 early init (RAM only) | 1 board init, misc found | 2 misc command seen (test builds) | 3 misc command cleared |
| 4 late init | 5 video probe | 6 video device | 7 video console probe |
| 8 video ready | 9 first text | 10 text drawn | 11 console record writing |
| 12 console record written | 13 abboot started | 14 previous stage's tree | 15 (unused) |
| 16 kernel loaded | 17 command line | 18 booti | 19 failure |
| 20 exception ESR | 21 exception ELR | 22 exception PC (relocated) | 23 exception FAR |
| 24 panic | 25 menu result | 26 record and pick | 27 record written |
| 28 whose kernel | 29 device tree | 30 test build's sticky command | 31 rescue kernel |

## bootctl

`tools/bootctl`: POSIX sh and an awk with `and()`, `xor()` and `rshift()` (busybox awk or gawk),
nothing else. The OS ships it and calls it; it is the only way the OS changes the record.

    bootctl [-q] [-p PARTNAME | -d DEVICE] <command>

| Command | Record |
| --- | --- |
| `status` | decoded: last booted, each slot's priority, tries, successful, verity, word; the slot booted (`ab.slot=`) and whose kernel runs (`ab.kernel=`) |
| `current` | prints the slot the boot layer booted, from `ab.slot=` in `/proc/cmdline`; exit 1 if none (no boot layer, or its rescue system) |
| `set-active <a\|b>` | that slot priority 15, tries 3, successful 0; the other priority 14 (its tries and successful bit kept) |
| `mark-good [a\|b]` | successful 1, tries kept; default: the slot booted (`current`) |
| `mark-bad <a\|b>` | priority 0, tries 0, successful 0 |
| `set <a\|b> [priority=N] [tries=N] [successful=0\|1]` | those fields only |
| `init` | the default record, only where misc 2048-4095 is all zero; nothing if a record is there |

misc is found by PARTNAME (the partition's name in the GPT): `MISC_PARTNAME` in
`/etc/bootctl.conf`, `-p`, or by default the first of `misc` and `MISC`. Every command but `init`
needs a valid record: **without one it changes nothing**, says so (not with `-q`) and exits 3. A
write reads block 4, changes the 32 bytes (reserved1 zeroed, every byte it does not own kept),
writes it back with `conv=notrunc,fsync`, and reads it back; an edit that changes nothing writes
nothing.

Exit status: 0 done, 1 error, 2 usage, 3 no record.

## What the OS does

The boot layer expects the OS to:

| When | The OS | bootctl |
| --- | --- | --- |
| installing an update into slot x (the one not running) | marks x bad first, writes the slot's files, puts its boot image at `slots/x/boot/boot.img` and a label at `slots/x/boot/label`, syncs | `bootctl mark-bad x`, then `bootctl set-active x` |
| the new slot has booted and works (the OS's own health check) | | `bootctl mark-good` |
| the user wants the other slot | | `bootctl set-active x` (on trial again) |
| the running slot is known broken | | `bootctl mark-bad <running>` (U-Boot then picks the other) |
| every slot boot | opens and feeds `/dev/watchdog` within the open timeout | |
| booted with `ab.kernel=rescue` | does not mark any slot good: the slot's own kernel may be what failed | |

`examples/update-slot.sh` does the first two rows; `tests/example-test.sh` runs it.

## Porting a board

A board is configuration: a Kconfig file and an environment file in `board/<vendor>/<board>/`, a
defconfig, a U-Boot DTS and its packing options. Every hook of `abboot/abboot.h` has a default made
from Kconfig values and the U-Boot device tree (`abboot/board.c`, `boot.c`, `menu.c`), and stays
weak only for a board that needs code; such code should be hardware support the U-Boot tree lacks
(SoC setup, a driver), never boot policy. The Echo boards (`boards/amazon-echo-show-*`) are
configuration only; the QEMU example board (`boards/qemu-arm64/abboot-qemu.c`) shows the hooks a
board can override in C. The steps are in [README.md](README.md#porting-the-boot-layer-to-a-board).

## Tests

On the host (`tests/run.sh`):

- `ab-test.sh`: `abboot/ab.c` built for the host: the pick, tries, fallback, mark-bad,
  corruption, kept bytes, reserved1 zeroed; and the same bytes as `abctl.py` for the default
  record and another.
- `bootctl-test.sh`: bootctl under busybox or gawk, each record decoded with `abctl.py`: no record
  means no write; init only into an empty area, the same bytes as ab.c; set-active, mark-good,
  mark-bad, set; the other bytes kept; `current`.
- `menu-test.sh`: the menu's keys (`ab_keys_step`): taps, holds, chords in every order and delay
  (144 combinations), keys held when the menu appears, bounces.
- `cmdline-test.sh`: `abboot/cmdline.c`: the header at 0 and at 0x400, the swap's outcomes, the
  header keys (root group, `ro|rw`, `init=`, a flag that only looks like one), lines shaped like
  the Echo boards' bootloaders', byte for byte as `cmdline-test.expected`.
- `example-test.sh`: `examples/update-slot.sh` against a store directory and a misc image.

In the build image (`uboot/build.sh`): `dt-test.sh`, `abboot/dt.c` with the U-Boot tree's libfdt:
a slot's tree gets the previous stage's additions and the `CONFIG_AB_DT_CARRY` properties, and
nothing else.

In QEMU (`uboot/build.sh`, `tests/qemu/qemu-test.sh`): U-Boot v2026.10 with abboot on the example
board boots a virtio disk 12 times, with and without the menu: the first boot writing the default
record, a good slot booting without a write, three tries and the fallback, a slot holding a
U-Boot image or no kernel, the rescue kernel, the stay screen, and misc holding other data.
