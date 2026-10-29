# Where each fact comes from: Echo Show 8 (2nd gen)

Every address, number and rule this board uses, with its source. Only three kinds of source are
used, and every entry says which:

- **(a) public**: published source code or documentation, linked below. Mainline Linux and U-Boot,
  AOSP, and the public unlock projects (kaeru, amonet).
- **(b) the device's own running Linux**, read with standard interfaces: the live device tree
  (`/sys/firmware/fdt`), `/proc/iomem`, `dmesg`, `/proc/cmdline`, `/sys/class/block`, the header of
  the device's own boot partition. Anyone with root on an unlocked device can repeat these reads.
- **(c) black-box tests**: what the device did when given a test image, described so that it can
  be repeated.

Nothing here comes from taking apart the device's firmware (LK, the secure firmware, Fire OS), from
vendor source trees, or from data specific to one unit (serial numbers, partition GUIDs, MAC
addresses). The packing tools read everything device-specific from the user's own boot image at
packing time; nothing of it is in this repository.

The links pin a commit, so that what they show does not change:

- Linux: https://github.com/torvalds/linux/tree/c339f4010dea8b0d754f84ace70cd1b089878ef8
- kaeru: https://github.com/R0rt1z2/kaeru/tree/7f498d06ab58b9db975a229452bcc9d21b6f71e7
- U-Boot v2026.10 (5508406582f6f7120dce0c4b3059819a41788db2)
- AOSP: `bootable/recovery/bootloader_message/include/bootloader_message/bootloader_message.h`
  and `system/tools/mkbootimg/mkbootimg.py` at https://android.googlesource.com/

## How to repeat the (b) checks

As root on the device (any Linux that boots on it):

    cat /sys/firmware/fdt > live.dtb            # then, on a computer: dtc -I dtb -O dts live.dtb
    cat /proc/iomem
    dmesg | grep -iE 'reserved mem|EL2|psci|arch_timer|simple-framebuffer|mtk-wdt'
    grep PARTNAME /sys/class/block/mmcblk0p*/uevent
    head -c 2048 /dev/mmcblk0p20 | od -An -tx4 -N 48    # the boot partition's header

For "what LK adds": take the device tree appended to the kernel in your boot partition (the bytes
right after the gzip stream) and compare it with `live.dtb`. Every node and property in the live
tree but not in the boot partition's was added by LK. abboot makes the same comparison at every
boot (SPEC.md, "The device tree"), so no list of them is in the board.

## The handover from LK

| Fact | Value | Source |
| --- | --- | --- |
| The chain | Boot ROM, preloader, LK with kaeru, then the boot partition's kernel | (a) kaeru README and `board/amazon/` (athena support) |
| CPU state at entry | AArch64, EL2 | (b) `dmesg`: "CPU: All CPU(s) started at EL2" |
| MMU and caches off, device tree in x0 | the arm64 Linux boot protocol | (a) Linux `Documentation/arch/arm64/booting.rst`: LK starts Linux kernels, which require it. (c) U-Boot runs and finds the tree in x0 (`CONFIG_SAVE_PREV_BL_FDT_ADDR`) |
| 64-bit start | kaeru starts an image as 64-bit | (a) kaeru `board-mt8183-common.c` ("Boot image requests a 64-bit kernel, forcing it"); the header command line of the device's own boot image starts with `bootopt=64S3,32N2,64N2` (b), and the packer keeps it |
| Boot image format | Android header v1 at byte 0, page 2048, gzip'd arm64 Image with the device tree appended, ramdisk | (a) AOSP mkbootimg; (b) the device's boot partition header |
| Header addresses | kernel 0x40200000, ramdisk 0x55000000, tags (device tree) 0x54000000 | (b) the boot partition header (`od` above). The packer keeps the user's own header |
| Header id | SHA-1 over kernel, ramdisk, second stage and recovery DTBO, each with its size | (a) AOSP mkbootimg |
| The 2 MB rule | the kernel area must be large; padded to at least 2 MB + 64 KB (`pack-uboot.py --min-kernel`, its default) | (c) see "The 2 MB rule" in README.md: kernel areas of 1.15 MB or less were not started, 2.17 MB or more were, timed over one-shot recovery boots. Why LK needs it was not looked into, and is not needed: the padding is enough |
| image_size in the Image header | at least 2 MB and the payload, plus 0x90000 | design: room for U-Boot's early stack and heap (`CONFIG_SYS_INIT_SP_BSS_OFFSET` 0x80000 + `CONFIG_SYS_MALLOC_F_LEN` 0x10000) |
| Header command line in LK's | LK puts the started image's header command line inside its own | (b) `/proc/cmdline` holds the boot image header's command line between LK's own tokens; (c) the slot's header line replaces it (`ab_header_line`, SPEC.md "The command line") |
| Root tokens of a normal boot | `skip_initramfs rootwait ro init=/init root=PARTUUID=<system> rootfstype=ext4` after `androidboot.verifiedbootstate=` | (b) `/proc/cmdline` of a normal boot of the boot partition. The board stores none of it |
| Root of a recovery boot | `root=/dev/ram` instead | (c) a test boot from the recovery partition: the kernel stopped at "Cannot open root device /dev/ram" when the slot image named no root of its own. Hence `CONFIG_AB_HEADER_KEYS` (its default): a root in the slot image's header command line replaces LK's |
| What LK adds to the device tree | root `model`; all of `/chosen` and `/memory`; `/idme`, `/firmware`, `/md_attr_node`, `/vpu_core0`; `/reserved-memory/mblock-*`, `reserve-memory-dram_r0_dummy_read`, `zone-movable-cma-memory` | (b) the live tree compared with the boot partition's tree (above). abboot finds these by the same comparison (`CONFIG_AB_DT_MERGE`, the default); run read-only on a computer over the boot partition's tree and the live tree, `abboot/dt.c` gives the live tree, line for line, apart from booti's `/memreserve/` for the ramdisk |
| Per-device data already in the boot partition's tree | not copied (`CONFIG_AB_DT_CARRY` empty) | design: an OS that keeps such data there (for example audio tuning it put into the device's boot image) names those properties in `CONFIG_AB_DT_CARRY` |

## Memory

| Fact | Value | Source |
| --- | --- | --- |
| RAM | 2 GB at 0x40000000 | (b) the live tree's `/memory`; `/proc/iomem` |
| U-Boot's own RAM | the lower 1 GB, U-Boot relocates below 0x80000000 | design: everything LK and the secure world keep is at 0xbb12f000 and above, or in the blocks below |
| SCP memory | 0x50000000, 0x2900000 | (a) Linux `mt8183-kukui.dtsi`, `scp_mem_reserved`; (b) `dmesg`: "memory@50000000", 41 MiB |
| LK's tree and what follows | 0x54000000 to 0x545fffff | (b) the header's tags address 0x54000000; `mrdump_rsvmem=...,0x54000000,0x200000,...` on LK's command line; the kernel's ramoops at 0x54410000, 896 KiB (`dmesg`) |
| BL31 | 0x54600000, 0x40000 | (b) `dmesg`: "mblock-1-atf-reserved", 0x54600000..0x5463ffff |
| Framebuffer block | 0xbe150000, 0xdb0000 bytes | (b) `dmesg` / live tree: "mblock-4-framebuffer", 0xbe150000..0xbeefffff |
| The framebuffer in U-Boot's MMU map | the whole block, normal non-cacheable (`CONFIG_MTK_MEM_MAP_EXTRA_*`) | (c) without it U-Boot crashed while simplefb mapped the framebuffer at run time; with it, it ran |
| kernel_addr_r, fdt_addr_r, loadaddr | 0x40200000, 0x48000000, 0x49000000 | design: 2 MB aligned for booti, in free RAM below the SCP memory (the boot image is at most `CONFIG_AB_IMAGE_MAX`, 96 MB) |
| `CONFIG_TEXT_BASE` | 0x40200000 | (b) the boot partition header's kernel address (above): LK unpacks the kernel there |
| `CONFIG_MTK_MEM_MAP_DDR_SIZE` | 0x40000000 | design: U-Boot's own RAM (above) |
| `CONFIG_SYS_INIT_SP_BSS_OFFSET`, `CONFIG_SYS_MALLOC_F_LEN`, `CONFIG_SYS_MALLOC_LEN` | 0x80000, 0x10000, 32 MB | design, as U-Boot's MT8183 boards |
| `CONFIG_AB_CRUMBS_RAM`, if you turn the diagnostics on | for example 0x600ff000 | design: a page in RAM U-Boot owns and no one uses before the kernel |

## Devices

| Fact | Value | Source |
| --- | --- | --- |
| GPIO controller | 0x10005000 | (a) Linux and U-Boot `mt8183.dtsi`, `pio: pinctrl@10005000`; (b) `/proc/iomem` |
| GPIO data-in registers | 0x10005000 + 0x200 + (pin / 32) * 0x10, bit pin % 32 | (a) Linux `drivers/pinctrl/mediatek/pinctrl-mt8183.c`: `mt8183_pin_di_range = PIN_FIELD(0, 192, 0x200, 0x10, 0, 1)` |
| Volume Up, Volume Down | GPIO 93, GPIO 92, active low: `CONFIG_AB_KEY_UP_REG` 0x10005220 bit 29, `CONFIG_AB_KEY_DOWN_REG` 0x10005220 bit 28, from the data-in rule above | (a) kaeru `board/amazon/include/mt8183-athena.h` (`GPIO_KEY_VOLUME_UP 93`, `..._DOWN 92`) and its comment "The keys pull the line low while held"; (b) the live tree's `gpio-keys` (`gpios = <&pio 93 GPIO_ACTIVE_LOW>`, 92); (c) the menu moved and started with the keys on a device |
| Keys at power-on | Volume Up alone: recovery; Volume Down alone: fastboot; both: nothing | (a) kaeru `board-mt8183-common.c`, `boot_mode_select`. Hence `CONFIG_AB_STAY_HINT` "Hold Volume Down at power-on for fastboot" |
| misc command | 32 bytes at offset 0, `boot-recovery` starts the recovery partition once | (a) AOSP `bootloader_message.h` (`struct bootloader_message`, `command`); kaeru reads it (`read_and_set_bootmode_from_message`, `lib/bootloader_message.c`). (c) on the devices tested the command was already zero when U-Boot looked |
| misc use by kaeru | the 2 KB `bootloader_message` at offset 0 | (a) kaeru `lib/bootloader_message.c`; the A/B record at 2048 is outside it (SPEC.md) |
| Watchdog | 0x10007000; left running by LK | (a) Linux and U-Boot `mt8183.dtsi`, `watchdog@10007000`; (c) a test payload that only loops (`b .`) was reset about 31 s later without anything feeding it |
| Watchdog timeout | 31 s, the 11-bit length field | (a) Linux `drivers/watchdog/mtk_wdt.c`: `WDT_MAX_TIMEOUT 31`, `WDT_LENGTH_TIMEOUT(n) ((n) << 5)`; (b) `dmesg`: "mtk-wdt ...: Watchdog enabled (timeout=31 sec, nowayout=1)" |
| `mtk_wdt.nowayout` | the driver's module parameter | (a) Linux `mtk_wdt.c` |
| The watchdog contract | `watchdog.open_timeout=120 mtk_wdt.nowayout=1 panic=5` (`CONFIG_AB_SLOT_ARGS`) | (a) Linux `drivers/watchdog/watchdog_core.c` and `watchdog_dev.c` (`open_timeout`), `mtk_wdt.c` (`nowayout`), `kernel/panic.c` (`panic=`); the numbers are design (SPEC.md) |
| Reset | PSCI's SYSTEM_RESET, then the watchdog (`CONFIG_AB_RESET_PSCI`) | (a) U-Boot's MT8183 code resets through PSCI; (b) `dmesg`: "PSCIv1.0 detected in firmware" |
| UART | 0x11002000, 26 MHz, 921600 baud | (a) Linux and U-Boot `mt8183.dtsi` (`uart0`), U-Boot `mt8183_pumpkin_defconfig` (921600) |
| Timer | arch timer at 13 MHz (`CONFIG_COUNTER_FREQUENCY`) | (a) U-Boot `mt8183_pumpkin_defconfig`; (b) `dmesg`: "arch_timer: cp15 timer running at 13.00MHz" |
| eMMC | MSDC0 at 0x11230000, `mediatek,mt8183-mmc`, clocks from U-Boot's MT8183 clock driver | (a) U-Boot `mt8183.dtsi`; (b) `/proc/iomem` |
| eMMC mode | 8 bits, 52 MHz high speed, no HS200 | design, (c) reads every test image without error |
| Partitions | `boot`, `misc`, `recovery`, `system`, found by name | (b) `PARTNAME` in `/sys/class/block/*/uevent` (p20, p6, p7, p21 on the devices checked) |
| misc size | 512 KB (the crumbs at blocks 896-1017 fit) | (b) `/sys/class/block/mmcblk0p6/size`: 1024 sectors |

## The screen

| Fact | Value | Source |
| --- | --- | --- |
| Framebuffer address | 0xbe150000, the first page of LK's block | (b) "mblock-4-framebuffer" (above); (c) the menu appears there |
| Size | 800x1280 | (a) the device's published screen size, 1280x800, held landscape; (b) `/sys/class/drm` modes under Linux; (c) the menu fills the screen |
| Stride, format | 3200 bytes, `a8r8g8b8` | (c) the menu's text and its gray bar show right; stride is 4 bytes a pixel, no gap |
| Rotation | `rot = 1` | (c) the text reads upright on the landscape device |
| Clear only the first page | 800 * 1280 * 4 bytes | (c) clearing the whole 0xdb0000 block froze the display on LK's last frame |
| Draw only when LK reserves it | `CONFIG_AB_SCREEN_NEED_RESERVED` (the default) | design: a safety check on (b); (c) the menu shows on the devices tested |

## The build tested on devices

This board is the configuration that was tested on devices, with four values changed. The tested
builds set `CONFIG_AB_SELF_MARKER` and `CONFIG_AB_TEST_MARKER` to their own strings, named one
property of their OS's own device tree in `CONFIG_AB_DT_CARRY`, and turned the diagnostics on
(`CONFIG_AB_CRUMBS`, `CONFIG_AB_CRUMBS_RAM`, `CONFIG_CONSOLE_RECORD`, `CONFIG_BOOTDELAY=0`). With
those values given back through `uboot/build.sh --config`, this repository builds the tested
U-Boot byte for byte, apart from the help text of the `abboot` command, which now names SPEC.md.
None of the facts above depends on the four values.

## Needs a test boot

- `CONFIG_AB_DT_CARRY` empty, as here: built, and checked on a computer (`tests/dt-test.sh`); the
  devices tested ran with one property named.
