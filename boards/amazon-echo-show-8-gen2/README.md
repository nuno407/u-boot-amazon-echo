# Echo Show 8 (2nd gen)

The boot layer (abboot) on the Amazon Echo Show 8, 2nd generation (2021). Its codename, used by
the unlock tools and here inside U-Boot, is **athena**; its chip is a MediaTek **MT8183**.

This board is only useful on a device that is already **unlocked** with the community's public
tools (kaeru, see "Links"), so that its bootloader starts any boot image you give it. Read the
whole page before you write anything to the device.

Where every address, number and rule in this board comes from, and how you can check it on your
own device: [PROVENANCE.md](PROVENANCE.md).

## How the device starts

    Boot ROM -> preloader -> LK, the device's bootloader, with kaeru (unchanged)
             -> the boot partition: an Android boot image whose "kernel" is U-Boot
             -> U-Boot with abboot: the A/B record in misc, the menu, slot A or B
             -> the slot's kernel, from the store partition

Words used on this page:

- **LK** ("Little Kernel"): the bootloader on the device. It loads the boot partition, fills in a
  device tree and starts the kernel. **kaeru** is the community patch to LK that unlocks it.
- **Boot image**: the Android format of the boot partition: a 2 KB header, then the kernel (here a
  gzip'd arm64 kernel with its device tree appended), then the ramdisk.
- **Recovery partition**: a second partition that holds a boot image. LK starts it instead of the
  boot partition when the first 32 bytes of misc say `boot-recovery` (the Android convention), or
  when Volume Up is held at power-on (kaeru).

U-Boot stands exactly where the kernel stood: LK loads the boot image, unpacks U-Boot to the
header's kernel address and starts it at EL2 with the MMU off and LK's device tree in register x0,
as it would start Linux. U-Boot then reads the slot's own boot image from the store, copies into
its device tree what LK added to the boot partition's (memory, reserved regions, the device's
factory data in `/idme`, `/chosen`; found by comparing the two trees, SPEC.md "The device tree"),
builds the command line from LK's (SPEC.md, "The command line"), and starts it.

The board is configuration only: a Kconfig file, an environment file, a defconfig, U-Boot's own
device tree, and the packing options. The boot itself is abboot's, the same on every board.

Partitions this board uses, found by their GPT names (the numbers are the ones seen on the devices
checked): `boot` (p20, U-Boot), `misc` (p6, the A/B record), `recovery` (p7, the rescue kernel),
`system` (p21, the store with the slots).

## The menu and the keys

Volume Up and Volume Down move the bar; **both together** start the selected slot. U-Boot reads
the keys straight from the GPIO controller (Volume Up is GPIO 93, Volume Down GPIO 92, both
pulled low while pressed); LK has already set the pins up. Mute is not read.

Keys held at power-on belong to LK, not to U-Boot: **Volume Up alone** starts the recovery
partition, **Volume Down alone** starts kaeru's fastboot mode. A key still held when the menu
appears does nothing until it is let go.

The screen is LK's framebuffer, which LK leaves on screen; U-Boot only writes pixels into its
first page and never touches a display register. U-Boot draws only when LK's device tree reserves
that memory as LK's framebuffer, so a build with the wrong address shows nothing rather than
writing over memory it does not own.

## Building

    uboot/build.sh --boards echo-show-8-gen2 --boot boot-backup.img

builds four variants of `amazon_athena_defconfig` into `out/` (`u-boot-echo-show-8-gen2-*.bin`) and
packs each into a boot image (`uboot-echo-show-8-gen2-*.img`), from `boot-backup.img`, a copy of
**your own** boot partition (see "Before anything" for how to make it):

| Variant | For | What it does |
| --- | --- | --- |
| `test-ro` | the recovery partition, first test | everything, but never writes the A/B record |
| `test` | the recovery partition | the full boot layer |
| `test-sticky` | the recovery partition, fallback tests | sets `boot-recovery` again before the kernel starts, so a failing kernel comes back to this U-Boot |
| `final` | the boot partition | the full boot layer, no test code |

The test builds clear misc's `boot-recovery` first thing, add `ab.test=1` to the kernel command
line, and take the rescue kernel from `/rescue.img` on the store (the recovery partition holds the
test build itself). `--config FILE` adds your own settings to every variant (for example
`CONFIG_AB_CRUMBS=y` for the diagnostics in misc, SPEC.md "Diagnostics").

To pack a build by hand:

    tools/pack-uboot.py --boot boot-backup.img \
        --uboot out/u-boot-echo-show-8-gen2-test-ro.bin --out uboot-test-ro.img \
        --marker abboot-echo-show-8-gen2-self

The packed image keeps your boot image's header, command line, device tree and ramdisk, so LK
treats it like your kernel. It holds your device's own device tree: keep it with your backups and
do not share it.

## Before anything: backups and a way back

1. **A way back that does not need the device to boot.** On this device that is kaeru's fastboot:
   power off, hold **Volume Down alone**, power on, keep holding until the screen shows fastboot;
   connect USB. kaeru's fastboot can flash `boot` and `recovery` (it protects only the
   bootloader partitions). Try it once now, before you change anything: `fastboot devices` must
   list the device. If even that fails, the last resort is the MediaTek boot ROM tools the unlock
   itself used.
2. **Copies of every partition you will touch**, kept off the device: `boot`, `recovery` and
   `misc` at least. As root on the device, for each: find it by name
   (`grep PARTNAME /sys/class/block/mmcblk0p*/uevent`), then `cat /dev/mmcblk0pN > name.img` and
   copy the file off. Check each copy with `sha256sum` on both sides.
3. A working system in the slots (`/slots/a/boot/boot.img`, SPEC.md) before U-Boot goes into the
   boot partition: U-Boot only boots slots, the rescue kernel, or nothing.

## Testing safely: one boot from the recovery partition

Nothing below writes the boot partition. If the test build fails in any way, the next boot is the
boot partition, as before.

1. Build and pack `test-ro` (above) and copy it to the device.
2. Write it to the recovery partition, checking the name first:

       grep -q '^PARTNAME=recovery$' /sys/class/block/mmcblk0p7/uevent &&
           dd if=uboot-test-ro.img of=/dev/mmcblk0p7 bs=1M conv=fsync

3. Ask LK for one boot of the recovery partition: write `boot-recovery` into the 32-byte command
   at the start of misc, and nothing else:

       grep -q '^PARTNAME=misc$' /sys/class/block/mmcblk0p6/uevent &&
           { printf boot-recovery; head -c 19 /dev/zero; } |
           dd of=/dev/mmcblk0p6 bs=32 count=1 conv=notrunc,fsync

4. Reboot. U-Boot clears the command at once (on the devices tested it was already clear: LK or
   kaeru clears it), shows the menu and boots a slot. In Linux, `ab.test=1` on `/proc/cmdline`
   says the test build started it.
5. If U-Boot hangs, the watchdog LK left running restarts the device within about 31 seconds, and
   the next boot is the boot partition.

When `test-ro` works, repeat with `test`, then with `test-sticky` for the fallback (a slot whose
kernel panics should be tried three times, then the other slot starts). Put your rescue image
back into the recovery partition when you are done: the test builds overwrote it.

## The 2 MB rule

LK does not start a boot image whose kernel area (the gzip'd kernel plus the appended device tree)
is too small. This was found by black-box tests, timing the round trip of a one-shot recovery
boot: an image LK does not start leaves the screen dark until the watchdog restarts the device,
about 45 seconds longer than one it starts. Images with a kernel area of 1.15 MB or less were
never started; images of 2.17 MB or more always were. A real kernel is far larger, but U-Boot
gzip'd is only about 0.2 MB, so `pack-uboot.py` pads the payload with random (incompressible)
bytes after U-Boot until the kernel area is at least 2 MB + 64 KB (`--min-kernel`). Why LK needs
it was not looked into, and is not needed: the padding is enough.

To repeat the test on your own device (each run is one safe test boot, as above):

1. Pack the same `test-ro` build twice: once as usual, once with `--min-kernel 0` (no padding).
   For a closer bracket, also `--min-kernel 0x120000` (about 1.1 MB) and `--min-kernel 0x230000`.
2. Boot each once from the recovery partition and time it from `reboot` to the first answer from
   the device (for example a ping or ssh loop, one try a second).
3. Expected: the padded image shows the menu and comes back in about the time of a plain reboot
   plus the menu's 5 s; the unpadded one shows nothing from U-Boot and comes back 30 to 50 s
   later, through the watchdog, on the boot partition.

## Installing (the boot partition)

Only after the test builds have worked several times, with the backups and the fastboot way back
checked:

1. Put a rescue kernel into the recovery partition: a boot image you know works. When LK starts
   the recovery partition by itself (Volume Up at power-on) it gives the kernel `root=/dev/ram`;
   if your rescue system needs another root, put it into the image's header command line, which
   abboot and LK both use.
2. Pack `final` and write it to `boot` (by `dd` from Linux after checking `PARTNAME=boot`, or
   `fastboot flash boot uboot-final.img`).
3. Watch the first boot: the menu, the countdown, the slot.

The way back is the same either way: `fastboot flash boot boot-backup.img` from kaeru's fastboot
(Volume Down at power-on), or `dd` of the backup from Linux. LK then starts that kernel directly,
as before.

## What U-Boot does when nothing can start

The slots first (SPEC.md), then the rescue kernel in the recovery partition, then the stay screen:
"No slot can start" and "Hold Volume Down at power-on for fastboot". U-Boot waits there, feeding
the watchdog, until the device is powered off.

## Settings

- `CONFIG_AB_DT_CARRY` (empty by default): properties to copy from LK's device tree into each
  slot's although the boot partition's own tree already holds them, for per-device data your OS
  keeps in the boot partition's device tree but not in the slots' (SPEC.md, "The device tree").
- The defconfig sets the watchdog contract `watchdog.open_timeout=120 mtk_wdt.nowayout=1 panic=5`
  with the 31 s watchdog timeout (`boards/mediatek/0001`), the keys (`CONFIG_AB_KEY_*`), the
  reset (`CONFIG_AB_RESET_PSCI`) and `CONFIG_AB_SELF_MARKER="abboot-echo-show-8-gen2-self"`.
- A slot whose kernel cannot be used gives way to the other slot whole; U-Boot carries no kernel
  of its own.

## Files

| Path | |
| --- | --- |
| `u-boot/board/amazon/athena/` | the board's Kconfig and its compiled-in environment |
| `u-boot/configs/amazon_athena_defconfig` | based on U-Boot's `mt8183_pumpkin_defconfig` |
| `u-boot/arch/arm/dts/mt8183-amazon-athena-uboot.dts` | U-Boot's own device tree, on U-Boot's `mt8183.dtsi` |
| `u-boot/include/configs/amazon_athena.h` | |
| `../mediatek/*.patch` | the watchdog's 31 s timeout, the framebuffer in the MMU map, the Kconfig and device tree lines of the Echo boards |
| `../../tools/pack-uboot.py` | packs a build into a boot image, from your own boot image (no options needed for this board) |
| `PROVENANCE.md` | where each fact comes from |

## Links

- kaeru (LK patches; the athena key and boot-mode handling): https://github.com/R0rt1z2/kaeru
- U-Boot's MT8183 support, and U-Boot started from an Android boot image on other phones:
  `doc/board/qualcomm/` in the U-Boot source.
