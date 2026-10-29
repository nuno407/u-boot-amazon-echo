# Echo Show 5 (2nd gen)

The boot layer (abboot) on the Amazon Echo Show 5, 2nd generation (2021). Its codename, used by
the unlock tools and here inside U-Boot, is **cronos**; its chip is a MediaTek **MT8163**, which
U-Boot does not support on its own. This board needs no SoC code beyond what U-Boot's MT8183
target already has: everything the device needs is in U-Boot's own device tree, and LK has set up
the hardware before U-Boot runs. Like the Echo Show 8 board it is configuration only.

This board is only useful on a device that is already **unlocked** with the community's public
tools (amonet and kaeru, see "Links"), so that its bootloader starts any boot image you give it.
Read the whole page, and the [Echo Show 8 page](../amazon-echo-show-8-gen2/README.md), which
explains the words used here, before you write anything to the device.

Where every address, number and rule in this board comes from, and how you can check it on your
own device: [PROVENANCE.md](PROVENANCE.md).

## How the device starts

    Boot ROM -> preloader -> LK -> amonet's loader and payload -> kaeru's LK (all unchanged)
             -> the boot partition: [amonet's 1 KB loader][Android header][U-Boot as the "kernel"]
             -> U-Boot with abboot: the A/B record in MISC, the menu, slot A or B
             -> the slot's boot image (kernel and initramfs), from the store partition

The unlock puts a small loader (amonet's) in the first 0x400 bytes of every boot image; the real
Android header follows at 0x400 and the kernel at 0x800. `tools/pack-uboot.py --header-at 0x400`
copies that loader from **your own** boot image; it is never shipped here.

Differences from the Echo Show 8:

- **The command line is LK's**, with only the header part swapped (SPEC.md, "The command line").
  It holds `root=/dev/ram`: the slot's initramfs finds the root, using `ab.slot=` from the
  command line.
- **MISC** is the partition name (upper case) here.
- **No menu on a dark boot.** On some boots (about 1 in 16 on the devices tested) LK's screen
  stays dark for a few seconds and LK's command line then carries `fps=6000`; on those boots
  nothing U-Boot draws can be seen, so U-Boot skips the menu and starts the selected slot at once
  (`CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG="fps=6000"`).
- **No UART** connector on the devices checked. Every boot stage leaves a crumb in MISC, and the
  console output goes there too (SPEC.md, "Diagnostics"); read them from Linux after the boot.
- **Reset** is the watchdog's: the device's PSCI is version 0.1, which has no system reset
  (`CONFIG_AB_RESET_WDT`).

Partitions, found by their GPT names (numbers as seen on the devices checked): `boot` (p9, U-Boot),
`MISC` (p8, the A/B record), `recovery` (p10, the rescue kernel), `system` (p12, the store).

## The menu and the keys

As on the Echo Show 8: Volume Up and Volume Down move the bar, **both together** start the
selected slot. Volume Up is GPIO 36, Volume Down GPIO 37, both pulled low while pressed. Mute is
not read: its line does not change before Linux powers the privacy circuit (tested).

**Volume Down held at power-on** starts the unlock's fallback fastboot (amonet's payload checks it
before LK reads the boot partition), so a broken boot partition cannot block it.

## When nothing can start

The same as the Echo Show 8: the slots first (SPEC.md), then the rescue kernel in the recovery
partition, then the stay screen, "No slot can start" with the hint "Hold Volume Down and plug in
USB for fastboot". U-Boot waits there, feeding the watchdog, until the device is powered off.
This board does not carry a kernel of its own inside the U-Boot image.

## Building

    uboot/build.sh --boards echo-show-5-gen2 --boot boot-backup.img

builds `test-ro`, `test`, `test-sticky` and `final` of `amazon_cronos_defconfig` into `out/`
(`u-boot-echo-show-5-gen2-*.bin`), with the same meaning as on the Echo Show 8, and packs each
(`uboot-echo-show-5-gen2-*.img`) from `boot-backup.img`, a copy of your own boot partition (it
must start with amonet's loader). By hand:

    tools/pack-uboot.py --boot boot-backup.img --header-at 0x400 \
        --uboot out/u-boot-echo-show-5-gen2-test-ro.bin --out uboot-test-ro.img \
        --marker abboot-echo-show-5-gen2-self

The packed image holds your device's own loader and device tree: keep it with your backups and
do not share it.

## Backups and the way back

1. **Fastboot without the system**: power off, hold **Volume Down**, power on with USB connected;
   the screen shows the fallback fastboot. Try it once before you change anything:
   `fastboot devices` must list the device. Its `flash boot` and `flash recovery` add amonet's
   loader to an image that lacks it, and leave an image that has it as it is.
2. Copies of `boot`, `recovery` and `MISC`, kept off the device, made and checked as on the Echo
   Show 8 (find each by `PARTNAME` in `/sys/class/block/mmcblk0p*/uevent`).
3. A working system in the slots before U-Boot goes into the boot partition.

## Testing safely: one boot from the recovery partition

Exactly as on the Echo Show 8, with the partition numbers of this device: write the test build to
`recovery` (check `PARTNAME=recovery` on `mmcblk0p10` first), write `boot-recovery` into the
first 32 bytes of `MISC` (`mmcblk0p8`, check `PARTNAME=MISC`), and reboot. If the test build
fails, the watchdog restarts the device within about 31 seconds and the next boot is the boot
partition. On the devices tested LK had already cleared the command when U-Boot looked.

## The 2 MB rule

On the Echo Show 8, LK does not start an image whose kernel area is 1.15 MB or less, and starts
every one of 2.17 MB or more ([the test, and how to repeat it](../amazon-echo-show-8-gen2/README.md#the-2-mb-rule)).
`pack-uboot.py` pads the same way here, to at least 2 MB + 64 KB (`--min-kernel`): images with a
kernel area of 2.17 MB start on this device too. Smaller ones were not tried; the same test
procedure applies. If a packed image does not come up (nothing from U-Boot, and a restart about
30 to 50 s later), pack it again with a larger `--min-kernel`, for example `0x800000`.

## Installing (the boot partition)

Only after the test builds have worked several times, with the backups and the fastboot way back
checked:

1. Put a rescue kernel into `recovery`: a boot image you know works on your device, in amonet's
   layout (for example your boot partition from before U-Boot).
2. Pack `final` and write it to `boot` (by `dd` from Linux after checking `PARTNAME=boot`, or
   `fastboot flash boot uboot-final.img`).
3. Watch the first boot.

The way back: `fastboot flash boot boot-backup.img` from the fallback fastboot (Volume Down at
power-on), or `dd` of the backup from Linux. LK then starts that kernel directly, as before.

## Files

| Path | |
| --- | --- |
| `u-boot/board/amazon/cronos/` | the board's Kconfig and its compiled-in environment |
| `u-boot/configs/amazon_cronos_defconfig` | on U-Boot's `TARGET_MT8183` |
| `u-boot/arch/arm/dts/mt8163-amazon-cronos-uboot.dts` | U-Boot's own device tree: the MT8163 devices U-Boot uses |
| `u-boot/include/configs/amazon_cronos.h` | |
| `../mediatek/*.patch` | shared with the Echo Show 8 |
| `../../tools/pack-uboot.py` | packs a build into a boot image, from your own boot image (`--header-at 0x400`) |
| `PROVENANCE.md` | where each fact comes from |

## Links

- amonet for this device (the loader layout, the fallback fastboot, the key pins): branch
  `mt8163-cronos` of https://github.com/R0rt1z2/amonet
- kaeru: https://github.com/R0rt1z2/kaeru
