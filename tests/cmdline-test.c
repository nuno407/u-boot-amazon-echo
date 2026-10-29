// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of the kernel's command line (abboot/cmdline.c): the prior stage's line with this
 * U-Boot image's header command line replaced by the booted image's, and the parameters of
 * CONFIG_AB_HEADER_KEYS that the booted header sets taken from it alone. The output is compared
 * with cmdline-test.expected. The lines are shaped like the Echo boards' bootloader lines, with
 * made-up serial numbers and partition GUIDs.
 *
 *   cc -DAB_HOST -I../abboot cmdline-test.c ../abboot/cmdline.c
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "abboot.h"

#define KEYS	"root= rootfstype= rootflags= init= rdinit= rootwait ro|rw skip_initramfs"

static u8 boot_part[4096];		/* the boot partition's first blocks */
static int have_boot;

int ab_read_part(const char *name, ulong blocks, void *buf)
{
	if (strcmp(name, "boot") || !have_boot || blocks > 8)
		return -EIO;
	memcpy(buf, boot_part, blocks * 512);
	return 0;
}

static void set_hdr(u8 *at, const char *cmdline)
{
	struct ab_bootimg_hdr *h = (void *)at;

	memset(h, 0, sizeof(*h));
	memcpy(h->magic, AB_BOOT_MAGIC, 8);
	h->page_size = 2048;
	strncpy(h->cmdline, cmdline, sizeof(h->cmdline));
}

/* Lines like the Echo Show 8 2nd gen bootloader's (LK's), on a normal and on a recovery boot. */
#define UB	"bootopt=64S3,32N2,64N2 console=ttyS0,921600 earlycon consoleblank=0 clk_ignore_unused " \
		"pd_ignore_unused regulator_ignore_unused loglevel=7 panic=5 " \
		"sysctl.kernel.panic_on_rcu_stall=1 mem=0x7e150000"
#define ROOT	"skip_initramfs rootwait ro init=/init root=PARTUUID=00000000-0000-4000-8000-000000000001 " \
		"rootfstype=ext4"
#define LK_HEAD	"console=tty0 console=ttyS0,921600n1 vmalloc=496M slub_max_order=0 slub_debug=OFZPU "
#define LK_VB	"androidboot.verifiedbootstate=orange"
#define LK_NORMAL	LK_HEAD LK_VB " " ROOT " " UB " androidboot.serialno=0000000000000000 " \
			"androidboot.bootreason=reboot"
#define LK_RECOVERY	LK_HEAD LK_VB " " UB " root=/dev/ram androidboot.serialno=0000000000000000 " \
			"androidboot.bootreason=reboot"
/* The Echo Show 5 2nd gen's: root=/dev/ram, the initramfs picks the root. */
#define CR	"bootopt=64S3,32N2,64N2 console=ttyMT0,921600n1 firmware_class.path=/vendor/firmware " \
		"androidboot.selinux=permissive buildvariant=userdebug clk_ignore_unused pd_ignore_unused"
#define LK_CRONOS	"fps=6000 vram=7340032 " CR " root=/dev/ram androidboot.serialno=1111111111111111"

struct tcase {
	const char *name;
	const char *lk;		/* the prior stage's /chosen/bootargs */
	const char *boot;	/* the boot partition's header command line; NULL: no boot image */
	const char *slot;	/* the booted image's header command line */
	int at;			/* the boot partition's header offset */
};

static const struct tcase cases[] = {
	{ "athena normal boot, slot header as U-Boot's", LK_NORMAL, UB, UB, 0 },
	{ "athena normal boot, slot header with its root group", LK_NORMAL, UB, UB " " ROOT, 0 },
	{ "athena recovery boot, slot header as U-Boot's", LK_RECOVERY, UB, UB, 0 },
	{ "athena recovery boot, slot header with its root group", LK_RECOVERY, UB, UB " " ROOT, 0 },
	{ "athena recovery boot, a slot header of its own", LK_RECOVERY, UB,
	  "bootopt=64S3,32N2,64N2 console=ttyS0,921600 loglevel=4 x.y=1", 0 },
	{ "slot header with rw and its own init", LK_NORMAL, UB, UB " rw init=/sbin/init", 0 },
	{ "slot header with trailing spaces", LK_RECOVERY, UB "   ", "console=ttyS0,921600 quiet   ", 0 },
	{ "U-Boot's header not in the line", LK_HEAD LK_VB " root=/dev/ram foo=1", UB, "x=1 root=/dev/sda", 0 },
	{ "no boot image in the boot partition", LK_RECOVERY, NULL, "x=1", 0 },
	{ "no boot image, the slot header in the line", "a " UB " root=/dev/ram b", NULL, UB " root=/dev/sdb", 0 },
	{ "empty slot header", LK_RECOVERY, UB, "", 0 },
	{ "rootwaitx is not rootwait", "a rootwait rootwaitx " UB, UB, UB " rootwait", 0 },
	{ "cronos, the header at 0x400, slot header as U-Boot's", LK_CRONOS, CR, CR, 0x400 },
	{ "cronos, a slot header of its own", LK_CRONOS, CR, "console=ttyMT0,921600n1 slot=1", 0x400 },
};

int main(void)
{
	static u8 slot[2048];
	char out[AB_CMDLINE_MAX];
	int i, ret;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct tcase *c = &cases[i];

		memset(boot_part, 0xa5, sizeof(boot_part));
		have_boot = 1;
		if (c->boot)
			set_hdr(boot_part + c->at, c->boot);
		else
			memset(boot_part, 0, sizeof(boot_part));
		set_hdr(slot, c->slot);
		out[0] = '\0';
		ret = ab_header_line(c->lk, (void *)slot, "boot", KEYS, out, sizeof(out));
		printf("= %s: %d\n=   %s\n", c->name, ret, out);
	}
	/* The pure part. */
	ret = ab_cmdline_build("a b c", "b", "x y", "", out, sizeof(out));
	printf("= build a b c, b -> x y: %d\n=   %s\n", ret, out);
	ret = ab_cmdline_build("ro a b root=1 c", "b", "rw root=2", KEYS, out, sizeof(out));
	printf("= build, ro|rw and root=: %d\n=   %s\n", ret, out);
	return 0;
}
