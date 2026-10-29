// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of abboot/cmdline.c: the booted image's header command line in place of U-Boot's own
 * in the line the previous bootloader built (ab_header_swap, ab_cmdline_swap, ab_hdr_cmdline),
 * against a stub of the partition U-Boot was started from.
 *
 *   cc -DAB_HOST -I../abboot cmdline-test.c ../abboot/cmdline.c
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "abboot.h"

/* --- Stub: the "boot" partition's first blocks. --- */

static u8 boot_part[4096];
static bool have_boot;

int ab_read_part(const char *name, ulong blocks, void *buf)
{
	if (strcmp(name, "boot") || !have_boot || blocks > 8)
		return -ENODEV;
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

static int fails;

static void expect(const char *name, const char *got, const char *want)
{
	if (strcmp(got, want)) {
		printf("FAIL %s:\n  got  \"%s\"\n  want \"%s\"\n", name, got, want);
		fails++;
	} else {
		printf("ok   %s\n", name);
	}
}

/* The bootloader's line: its own tokens around the header command line of the image it started. */
#define UB	"console=ttyS0,115200 loglevel=7 panic=5"
#define LINE(mid)	"console=tty0 vmalloc=496M " mid " root=/dev/ram bootreason=reboot"

int main(void)
{
	static u8 slot[2048];
	char buf[AB_CMDLINE_MAX], out[sizeof(((struct ab_bootimg_hdr *)0)->cmdline) + 1];

	/* The header at byte 0: U-Boot's header command line replaced by the slot's. */
	memset(boot_part, 0, sizeof(boot_part));
	have_boot = true;
	set_hdr(boot_part, UB);
	set_hdr(slot, "console=ttyS0,115200 loglevel=4 quiet");
	expect("header at 0, the slot's own line", ab_header_swap(LINE(UB), (void *)slot, "boot"),
	       LINE("console=ttyS0,115200 loglevel=4 quiet"));

	/* The same line in both: nothing to do. */
	set_hdr(slot, UB);
	expect("the same header line", ab_header_swap(LINE(UB), (void *)slot, "boot"), LINE(UB));

	/* Trailing spaces in either header are not part of the line. */
	set_hdr(boot_part, UB "   ");
	set_hdr(slot, "x=1   ");
	expect("trailing spaces", ab_header_swap(LINE(UB), (void *)slot, "boot"), LINE("x=1"));

	/* An empty slot header line: U-Boot's taken out. */
	set_hdr(boot_part, UB);
	set_hdr(slot, "");
	expect("empty slot header", ab_header_swap(LINE(UB), (void *)slot, "boot"), LINE(""));

	/* U-Boot's header line not in the bootloader's line: kept as it is. */
	set_hdr(slot, "y=2");
	expect("not in the line", ab_header_swap("console=tty0 root=/dev/ram", (void *)slot, "boot"),
	       "console=tty0 root=/dev/ram");

	/* The header at 0x400, behind a 1 KB stub. */
	memset(boot_part, 0, sizeof(boot_part));
	memset(boot_part, 0xa5, 0x400);
	set_hdr(boot_part + 0x400, "console=ttyS0 stub=1");
	set_hdr(slot, "console=ttyS0 slot=1");
	expect("header at 0x400", ab_header_swap("a console=ttyS0 stub=1 root=/dev/ram", (void *)slot, "boot"),
	       "a console=ttyS0 slot=1 root=/dev/ram");

	/* No boot image in the partition, or no partition: the line as it is. */
	memset(boot_part, 0, sizeof(boot_part));
	expect("no boot image", ab_header_swap("keep me", (void *)slot, "boot"), "keep me");
	have_boot = false;
	expect("no partition", ab_header_swap("keep", (void *)slot, "boot"), "keep");

	/* ab_cmdline_swap's three outcomes. */
	if (ab_cmdline_swap("a b c", "", "x", buf, sizeof(buf)) != 0 ||
	    ab_cmdline_swap("a b c", "b", "b", buf, sizeof(buf)) != 0 ||
	    ab_cmdline_swap("a b c", "q", "x", buf, sizeof(buf)) != -1 ||
	    ab_cmdline_swap("a b c", "b", "x y", buf, sizeof(buf)) != 1 || strcmp(buf, "a x y c")) {
		printf("FAIL ab_cmdline_swap\n");
		fails++;
	} else {
		printf("ok   ab_cmdline_swap: nothing to do, not found, replaced\n");
	}

	/* ab_hdr_cmdline: a full 512-byte line without a NUL is still terminated. */
	memset(((struct ab_bootimg_hdr *)slot)->cmdline, 'z', 512);
	ab_hdr_cmdline((void *)slot, out);
	if (strlen(out) != 512) {
		printf("FAIL ab_hdr_cmdline: %zu bytes\n", strlen(out));
		fails++;
	} else {
		printf("ok   ab_hdr_cmdline: a full header line\n");
	}

	printf("cmdline-test: %s\n", fails ? "FAILED" : "all passed");
	return !!fails;
}
