# Where each fact comes from: Echo Show 5 (2nd gen)

The same three kinds of source as for the Echo Show 8 ([its PROVENANCE.md](../amazon-echo-show-8-gen2/PROVENANCE.md)
explains them and how to repeat the device reads): **(a)** public source code or documentation,
**(b)** the device's own running Linux read with standard interfaces, **(c)** black-box tests.
Nothing comes from taking the device's firmware apart, from vendor source trees, or from data
specific to one unit. The loader in the first 0x400 bytes of a boot image is copied from the
user's own boot image at packing time and is not in this repository.

Pinned links:

- amonet, branch `mt8163-cronos`: https://github.com/R0rt1z2/amonet/tree/0ce7c83d7fc98cac3eea245428df27b984151d6a
- kaeru: https://github.com/R0rt1z2/kaeru/tree/7f498d06ab58b9db975a229452bcc9d21b6f71e7
- Linux: https://github.com/torvalds/linux/tree/c339f4010dea8b0d754f84ace70cd1b089878ef8
- U-Boot v2026.10; AOSP `bootloader_message.h` and `mkbootimg.py`

On this device the boot partition is `mmcblk0p9`: `head -c 2048 /dev/mmcblk0p9 | tail -c +1025 | od -An -tx4 -N 48`
shows the real header.

## The handover from LK

| Fact | Value | Source |
| --- | --- | --- |
| The chain | Boot ROM, preloader, LK, amonet's loader and payload, kaeru's LK, then the boot partition's kernel | (a) amonet `mt8163-cronos` (`microloader/`, `lk-payload/main.c`: loads LK from `swdl`); kaeru `board/amazon/include/mt8163-cronos.h` |
| Boot image layout | amonet's loader in bytes 0-0x3ff, the real header at 0x400, the kernel at 0x800 | (a) amonet `microloader/inject_microloader.py` (keeps the original's first 0x400 bytes after its own 0x400, then the rest from 0x800) and `lk-payload/main.c` (`inject_microloader`); (b) the device's boot partition: `ANDROID!` at 0 and at 0x400 |
| Header | v0, page 2048; kernel 0x40080000, ramdisk 0x69244e00, tags 0x48000000 | (b) the boot partition header. The packer keeps the user's own |
| Header id | SHA-1 over kernel, ramdisk, second stage, each with its size | (a) AOSP mkbootimg |
| CPU state at entry | AArch64, EL2, MMU off, device tree in x0 | (b) `dmesg`: "CPU: All CPU(s) started at EL2"; (a) the arm64 Linux boot protocol; (c) U-Boot runs and finds the tree |
| 64-bit start | kaeru starts a 64-bit image as such | (a) kaeru; (b) `bootopt=64S3,32N2,64N2` in the device's header command line, kept by the packer |
| The 2 MB rule | kernel area padded to at least 2 MB + 64 KB (`pack-uboot.py --min-kernel`, its default) | (c) on the Echo Show 8; here an image padded so (kernel area 2.17 MB) starts. Smaller ones were not tried |
| Command line | LK's, with `root=/dev/ram`; the boot partition's header command line inside it | (b) `/proc/cmdline`. Slot images carry the same header command line, so abboot's swap (SPEC.md, "The command line") leaves LK's line as it is |
| What LK adds to the device tree | root `model`; all of `/chosen` and `/memory`; `/idme`; `/reserved-memory/fb-reserved-memory` | (b) the live tree compared with the boot partition's. abboot finds these by the same comparison (`CONFIG_AB_DT_MERGE`); run read-only on a computer over the boot partition's tree and the live tree, `abboot/dt.c` gives the live tree, apart from booti's `/memreserve/` |
| PSCI | version 0.1, no SYSTEM_RESET: the watchdog resets (`CONFIG_AB_RESET_WDT`) | (b) `dmesg`: "Using PSCI v0.1 Function IDs from DT", the live tree's `/psci` (cpu_suspend, cpu_off, cpu_on, affinity_info only); (a) the PSCI specification adds SYSTEM_RESET in version 0.2 |
| U-Boot target | `TARGET_MT8183` with this board's own device tree | design: that target's SoC code is `dram_init` from the device tree and a PSCI reset, which this board does not use (`CONFIG_PSCI_RESET` off) |

## Memory

| Fact | Value | Source |
| --- | --- | --- |
| RAM | 0x40000000, 0x3f200000 bytes | (b) live `/memory`, `/proc/iomem` |
| Secure monitor | 0x43000000, 0x30000 | (b) `dmesg`: "secmon@43000000", 192 KiB |
| ramoops | 0x44410000, 0xe0000 | (b) `dmesg`: "ramoops@44410000", 896 KiB |
| LK's tree | 0x48000000 (1 MB kept free) | (b) the header's tags address |
| Framebuffer block | 0x5f900000, 0x700000 | (b) `dmesg` / live tree: "fb-reserved-memory" |
| Reserved at the top | from 0x7bb80000 | (b) `/proc/iomem` |
| U-Boot's own RAM | 0x40000000 to 0x7b000000 (`CONFIG_MTK_MEM_MAP_DDR_SIZE` maps up to 0x7f000000) | design, below the reserved top |
| kernel_addr_r, fdt_addr_r, loadaddr | 0x40200000, 0x4a000000, 0x50000000 | design: free RAM; the kernel up to the secure monitor, a boot image up to 96 MB below the framebuffer |
| `CONFIG_TEXT_BASE` | 0x40200000 | design: U-Boot is built position independent (`CONFIG_POSITION_INDEPENDENT`) and runs where LK unpacks it (the header's kernel address); (c) it does |
| `CONFIG_SYS_INIT_SP_BSS_OFFSET`, `CONFIG_SYS_MALLOC_F_LEN`, `CONFIG_SYS_MALLOC_LEN` | 0x80000, 0x10000, 32 MB | design, as the Echo Show 8 board |
| Timer | 13 MHz (`CONFIG_COUNTER_FREQUENCY`) | (b) `dmesg`: the arch timer's rate |

## Devices

| Fact | Value | Source |
| --- | --- | --- |
| GPIO registers | 0x10005000 | (b) the live tree: `syscon@10005000`, `mediatek,mt8163-pctl-a-syscfg`, which the pinctrl node uses as its register map |
| GPIO data-in | 0x10005000 + 0x500 + (pin / 16) * 0x10, bit pin % 16 | (a) amonet `lk-payload/common.h`: `GPIO_DIN3 0x10005520`, and `main.c`: Volume Up "pin 36, bit 4", Volume Down "pin 37, bit 5"; the same layout as Linux's other MediaTek pinctrl drivers of that generation (`din_offset 0x500`, 16 pins a register); (c) the keys moved the menu and started a slot on a unit |
| Volume Up, Volume Down | GPIO 36, GPIO 37, active low: `CONFIG_AB_KEY_UP_REG` 0x10005520 bit 4, `CONFIG_AB_KEY_DOWN_REG` 0x10005520 bit 5 | (a) amonet (above, "GPIO_ACTIVE_LOW"); (b) the live tree's `gpio-keys` (`gpios = <... 0x24 GPIO_ACTIVE_LOW>`, 0x25) |
| Mute | not read | (c) its line stayed high under U-Boot while held |
| Keys at power-on | Volume Down alone: the fallback fastboot | (a) amonet `lk-payload/main.c` ("Volume down pressed, entering fastboot"). Hence `CONFIG_AB_STAY_HINT` "Hold Volume Down and plug in USB for fastboot" |
| Fastboot flash | `flash boot` and `flash recovery` add amonet's loader if missing | (a) amonet `lk-payload/main.c`, `cmd_flash_wrapper`, `inject_microloader` |
| MISC command | `boot-recovery` starts the recovery partition once | (a) AOSP `bootloader_message.h`; amonet's `reboot recovery` writes it (`write_boot_command("boot-recovery")`); (c) test boots. On the devices tested the command was already zero when U-Boot looked |
| Watchdog | 0x10007000, `mediatek,wdt`, 31 s, left running by LK | (b) `/proc/iomem`; `dmesg` "mtk-wdt ... timeout=31 sec"; (a) Linux `mtk_wdt.c`; (c) a hung kernel was reset by it, and `wdt expire` resets |
| The watchdog contract | `watchdog.open_timeout=120 mtk_wdt.nowayout=1 panic=5` (`CONFIG_AB_SLOT_ARGS`) | as the Echo Show 8 |
| UART | 0x11002000, 26 MHz | (b) `/proc/iomem`; LK's `earlycon=uart8250,mmio32,0x11002000` and `console=ttyS0,921600n1` on the command line; the live tree's 26 MHz `oscillator-26m` |
| eMMC controller | MSDC0 at 0x11230000; `mediatek,mt8163-mmc`, `mediatek,mt2701-mmc`; `mediatek,latch-ck = <1>` | (b) the live tree and `/proc/iomem` |
| U-Boot's compatible | `mediatek,mt7623-mmc` | (a) Linux `mtk-sd.yaml` pairs `mediatek,mt7623-mmc` with `mediatek,mt2701-mmc`; U-Boot's `mtk-sd.c` has MT7623 and no MT2701; (c) it reads every test image without error |
| eMMC clocks | source 200 MHz, bus 136.5 MHz, fixed | (b) the kernel's `/sys/kernel/debug/clk/clk_summary` (msdc30_0_sel, axi). Not re-read for this release: it needs debugfs mounted (`mount -t debugfs none /sys/kernel/debug`). A lower real source only makes the bus slower than the 26 MHz asked |
| eMMC mode | 8 bits, 26 MHz, default speed | design; (c) reads 14 MB in about a second |
| Partitions | `boot`, `MISC` (`CONFIG_AB_MISC_PART`), `recovery`, `system`, found by name | (b) `PARTNAME` in `/sys/class/block/*/uevent` (p9, p8, p10, p12 on the devices checked) |
| MISC size | 512 KB: the crumbs and the console record (`CONFIG_AB_CRUMBS`, on in this board: there is no UART connector) fit in blocks 896-1017 | (b) 1024 sectors |

## The screen

| Fact | Value | Source |
| --- | --- | --- |
| Framebuffer | 0x5f900000, the first page of fb-reserved-memory | (b) `fb-reserved-memory`; (c) the display controller's layer 0 scanned out from 0x5f900000 (read once, read-only, from a test build); the menu is drawn there |
| Size | 480x960 | (a) the device's published screen size, 960x480, held landscape; (b) `/sys/class/drm/card1-DSI-1/modes`: 480x960 |
| Stride, format | 1920 bytes, `a8b8g8r8` | (b) the live tree's `simple-framebuffer` node; the menu is white, gray and black, which look the same in either byte order |
| Rotation | `rot = 1` | (c) text upright |
| `fps=6000` | no menu that boot (`CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG`) | (b) on such boots LK's command line carries `fps=6000`; (c) the menu was not visible on those boots, and on the others it was |
| Draw only when LK reserves it | `CONFIG_AB_SCREEN_NEED_RESERVED` (the default) | design: a safety check on (b); (c) the menu shows on the devices tested |

## The build tested on devices

This board is the configuration that was tested on devices, with two values changed: the tested
builds set `CONFIG_AB_SELF_MARKER` and `CONFIG_AB_TEST_MARKER` to their own strings. With those
given back through `uboot/build.sh --config`, this repository builds the tested U-Boot byte for
byte, apart from the help text of the `abboot` command, which now names SPEC.md. U-Boot carries
no kernel of its own (the first test builds did: that is gone).

## Needs a test boot

- The stay screen with both slots bad and no rescue kernel: `test` with both slots marked bad
  (`bootctl mark-bad a; bootctl mark-bad b`) and no `/rescue.img` on the store should show "No
  slot can start" and stay; power off to leave it. (It works this way on the Echo Show 8 and in
  QEMU.)
