# u-boot-amazon-echo

A small addition to [U-Boot](https://u-boot.org) that gives a device two copies of its system, so
that an update that does not work can never leave the device unable to start.

This is an independent project, not affiliated with or endorsed by Amazon. "Amazon", "Echo" and
"Echo Show" are trademarks of their owner and are used here only to say which devices this code
runs on.

The repository has two parts:

1. **The boot layer** (`abboot/`, `tools/`): works on any arm64 device that runs U-Boot. Most of
   this README is about it.
2. **Boards** (`boards/`): the settings that fit the boot layer to one device. There are two
   Amazon Echo boards, the [Echo Show 8 (2nd gen)](boards/amazon-echo-show-8-gen2/README.md) and
   the [Echo Show 5 (2nd gen)](boards/amazon-echo-show-5-gen2/README.md), and an example board
   for QEMU (a free PC emulator) that the tests run on. A board is configuration only: no C
   code of its own.

## Words used here

- **Bootloader**: the small program that runs first when a device powers on and starts the
  operating system. U-Boot is a widely used open-source bootloader.
- **Kernel**: the core of the operating system (here, Linux). It is started by the bootloader.
- **Partition**: a named section of the device's storage (its eMMC chip), like a drive on a PC.
- **A/B slots**: two copies of the system, called slot A and slot B. One is running; an update is
  written into the other. If the updated copy does not start, the device goes back to the one
  that worked.
- **misc partition**: a small partition that bootloaders and the OS use to leave notes for each
  other. The boot layer keeps its 32-byte **A/B record** there: which slot to start, how many
  tries a new slot has left, and which slots are known good.
- **Store**: one ext4 partition (the usual Linux filesystem) that holds both slots as directories.
- **Watchdog**: a hardware timer that restarts the device unless software keeps resetting it. If
  the system hangs, the watchdog restarts the device, and the boot layer counts that as a failed
  try.
- **Rescue kernel**: a separate, known-good system in its own partition, started when neither
  slot can start.
- **Device tree**: a file that describes the hardware to the kernel (memory, buttons, screen).

## How it works

Think of the device as three layers, each knowing nothing about the one above it:

| Layer | What it is | What it knows |
| --- | --- | --- |
| **Boot** | U-Boot with this boot layer | the A/B record and where each slot keeps its kernel |
| **OS** | Linux, its files, the update tool | the boot layer, through the `bootctl` tool |
| **App** | whatever the device is for | nothing about booting |

Because the boot layer reads only the A/B record and two files per slot, it can start any Linux
system that follows those two rules. The OS, in turn, only ever talks to the boot layer through
`bootctl`.

### What happens at power-on

1. U-Boot reads the A/B record from the misc partition. On the very first boot there is none,
   and it writes a default one: slot A first, slot B second, both "on trial".
2. It picks the slot with the higher priority. A slot on trial loses one try each time it is
   started. A slot whose tries have run out, without the OS ever saying "this works", has failed:
   U-Boot switches to the other slot and says so on the screen ("Slot A failed, back to slot B").
3. If the board has a screen and buttons, a menu shows both slots for 5 seconds: Volume Up or
   Down moves the bar, both keys together start the selected slot. Without a key press, the
   picked slot starts.
4. U-Boot loads the slot's kernel from `/slots/a/boot/boot.img` (or `b`) on the store. If that
   file is missing or damaged, it starts the other slot instead, then the rescue kernel. If
   nothing at all can start, it shows a message and waits, rather than restarting in a loop.
5. It starts the kernel with `ab.slot=a` (or `b`) on the kernel command line, so the OS knows
   which slot it is running in, and with the watchdog running.
6. The OS, once it decides it works (that is up to the OS), runs `bootctl mark-good`. From then
   on that slot starts without using tries.

The full rules, byte by byte, are in [SPEC.md](SPEC.md).

### The slot convention

On the store partition:

    /slots/a/boot/boot.img    slot A's kernel: an Android boot image (gzip'd arm64 kernel with
                              its device tree appended, and an initramfs)
    /slots/a/boot/label       optional: one line shown in the menu, e.g. a build date
    /slots/b/boot/...         the same for slot B

Everything else in `/slots/a/` (usually the slot's whole root filesystem) belongs to the OS. The
boot layer reads nothing else.

### bootctl

`tools/bootctl` is a shell script for the device's OS (it needs only `sh` and busybox awk or
gawk). It finds the misc partition by name and reads or changes the A/B record:

    bootctl status              show the record and which slot is running
    bootctl current             print the running slot (a or b)
    bootctl set-active b        try slot b on the next boot (3 tries)
    bootctl mark-good           the running slot works: stop counting its tries
    bootctl mark-bad a          never start slot a (until set-active again)
    bootctl init                write the default record on a device that has none

It refuses to write over anything that is not its own record. `examples/update-slot.sh` shows how
an update tool uses it. `tools/abctl.py` reads and edits the same record on a computer, from a
copy of the misc partition.

## Porting the boot layer to a board

A board needs U-Boot already working on it (serial console, storage). A board is then
configuration only: the Echo boards in `boards/` show the whole of it.

1. **Add the boot layer to your U-Boot tree** (tested with U-Boot v2026.10):

       uboot/install.sh path/to/u-boot

   This copies `abboot/` to `boot/abboot/` and applies `uboot/patches/0001` and `0003` (the
   second only adds debug output for `CONFIG_AB_DEBUG_BOOT` builds). Add `--hooks` to also apply
   `uboot/patches/0002` (needed only for crash records, see below).
2. **Kconfig** (`board/<vendor>/<board>/Kconfig`): your board selects `ABBOOT` and `AB_BOARD_INIT`
   (abboot then provides the board's start-up functions), plus what the device needs, for
   example `SAVE_PREV_BL_FDT_ADDR` when a previous bootloader hands over a device tree.
3. **defconfig**: `CONFIG_BOOTCOMMAND="abboot"` and the options that differ from the defaults
   (each option's help text in `abboot/Kconfig` says more):
   - where things are: `CONFIG_AB_DEV_IFACE` and `CONFIG_AB_DEV_NUM` (default `mmc 0`), the
     partition names `CONFIG_AB_MISC_PART`, `CONFIG_AB_STORE_PART`, `CONFIG_AB_SELF_PART`,
     `CONFIG_AB_RESCUE_PART` (defaults `misc`, `system`, `boot`, `recovery`);
   - `CONFIG_AB_SELF_MARKER`: a string unique to your U-Boot build, so it never boots itself;
   - `CONFIG_AB_SLOT_ARGS`: the watchdog contract for your watchdog driver (SPEC.md);
   - `CONFIG_AB_STAY_HINT`: what to tell the user when nothing can start;
   - the keys: `CONFIG_AB_KEY_UP_REG`/`_BIT`, `CONFIG_AB_KEY_DOWN_REG`/`_BIT` (one register bit
     each), `CONFIG_AB_KEYS_ACTIVE_LOW`;
   - the screen: `CONFIG_AB_MENU` (needs a video driver), `CONFIG_AB_SCREEN_CLEAR`,
     `CONFIG_AB_SCREEN_NEED_RESERVED`, `CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG`;
   - the kernel: `CONFIG_AB_HEADER_KEYS` (the default suits a Linux root), `CONFIG_AB_DT_MERGE`,
     `CONFIG_AB_DT_CARRY`;
   - the reset after a failure: `CONFIG_AB_RESET_PSCI` or `CONFIG_AB_RESET_WDT`;
   - diagnostics: `CONFIG_AB_CRUMBS` and its misc blocks, `CONFIG_AB_CRUMBS_RAM`.

   Test builds add `CONFIG_AB_TESTSLOT`, `CONFIG_AB_TEST_MARKER`, `CONFIG_AB_STICKY`,
   `CONFIG_AB_READONLY`, `CONFIG_AB_RESCUE_FILE` or `CONFIG_AB_INIT` (`uboot/variants/` and
   `uboot/build.sh --variants`).
4. **Device tree** (U-Boot's own): memory, the storage controller, the watchdog, the UART, and
   the screen as a `simple-framebuffer` node (address and size of the page drawn into, width,
   height, stride, format, `rot`).
5. **Environment** (`board/<vendor>/<board>/<board>.env`): `kernel_addr_r` (2 MB aligned),
   `fdt_addr_r` (above it: the kernel is unpacked between the two), `loadaddr` (where the boot
   image is read), all in free RAM.
6. **Packing**, if U-Boot is started as the kernel of a boot image: `tools/pack-uboot.py` with
   the board's options (`--header-at`, `--min-kernel`).
7. **The OS side**: ship `tools/bootctl` in your root filesystem, put each slot's kernel at
   `slots/<x>/boot/boot.img`, feed the watchdog, and run `bootctl mark-good` once the system works.

The hooks behind these options are weak functions declared in `abboot/abboot.h`. A board that
needs code (a key behind an I2C chip, for example) can override one; `boards/qemu-arm64/` does,
as an example.

**Crash records** (optional): with `CONFIG_AB_CRUMBS`, U-Boot writes a short note per boot stage
and its own console output into unused blocks of misc, so you can see after a restart how far the
last boot got. SPEC.md, "Diagnostics", has the format.

## Echo boards

| Board | Device | Chip |
| --- | --- | --- |
| [`boards/amazon-echo-show-8-gen2/`](boards/amazon-echo-show-8-gen2/README.md) | Echo Show 8 (2nd gen), "athena" | MediaTek MT8183 |
| [`boards/amazon-echo-show-5-gen2/`](boards/amazon-echo-show-5-gen2/README.md) | Echo Show 5 (2nd gen), "cronos" | MediaTek MT8163 |

Both are useful only on a device already unlocked with the community's public tools, whose
bootloader then starts U-Boot from the boot partition in place of the kernel. Each board's
README.md says how to build, test safely and install, and its PROVENANCE.md where every address,
number and rule comes from: public source code, the device's own running Linux, or black-box
tests. Nothing derived from Amazon's firmware or other proprietary material is in this
repository. The packing tool takes everything specific to one device (its device tree, its boot
image header, and on the Echo Show 5 the unlock's loader in front of the header) from a copy of
that device's own boot partition, at packing time.

`boards/mediatek/` holds the U-Boot changes both boards need: the watchdog's full 31 s timeout,
the framebuffer in the MMU map, and the Kconfig and device tree lines of the two boards.

## Building and testing

Host tests (need a C compiler, zlib and Python 3; the bootctl tests also need busybox, gawk or
Docker):

    tests/run.sh

U-Boot with the boot layer (needs Docker):

    uboot/build.sh [--boards "qemu echo-show-8-gen2 echo-show-5-gen2"]

This clones U-Boot v2026.10 into a Docker volume, installs the boot layer and the boards, and
builds into `out/`: for QEMU two variants (with and without the menu), for each Echo board four
(`test-ro`, `test`, `test-sticky`, `final`; `--variants` and `--config` pick others, see the
script). It then runs the device tree test and boots each QEMU variant 12 times from a test disk
(`tests/qemu/qemu-test.sh`): first boot, a good slot, an update that never works and the
fallback, a missing or wrong kernel, the rescue kernel, nothing to start, and a misc partition
that holds someone else's data. The kernels in these tests are tiny stand-ins
(`tests/qemu/payload.S`) that print their name and power QEMU off.

The build is reproducible: the same commit of this repository and the same options give the same
bytes.

## Safety notes

Replacing or adding a bootloader is the riskiest change you can make to a device.

- **Know how to recover first.** Before you put any U-Boot on a device, make sure you have a way
  back that does not need the device to boot (for example the SoC's USB download mode) and a
  full backup of every partition you will touch, kept off the device.
- **Test without writing.** `CONFIG_AB_READONLY` builds a U-Boot that reads and follows the
  record but never writes it. Use it first.
- **Keep the rescue kernel.** It is what starts when both slots are broken. Do not put a test
  build of U-Boot in the rescue partition unless you set `CONFIG_AB_RESCUE_FILE`.
- **Never write misc by hand** except through `bootctl` or `abctl.py`: other software may keep its
  own data in other parts of that partition.
- The boot layer only reads the store; it never repairs a damaged filesystem. The OS must finish
  and sync its writes before it restarts.
- This software comes with no warranty (see LICENSE).

## License

The boot layer, the boards and the patches are built into U-Boot and are licensed, like U-Boot,
under GPL-2.0-or-later. `tools/bootctl`, `tools/abctl.py`, `tools/pack-uboot.py` and
`examples/update-slot.sh` are under the MIT license, so any OS can ship them. Details in [LICENSE](LICENSE); each file has an SPDX
line.

The A/B record uses the byte layout of AOSP's `struct bootloader_control`, and boot images use the
Android boot image header layout written by AOSP's `mkbootimg`. Both formats are published by the
Android Open Source Project. The code here was written from those published layouts; it does not
copy AOSP's headers (Apache-2.0) or U-Boot's own `android_ab.c`.
