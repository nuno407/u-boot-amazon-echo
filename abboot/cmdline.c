// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * abboot: the boot image's header command line in the kernel's command line (abboot.h, SPEC.md
 * "The command line").
 *
 * Some bootloaders build the kernel's command line around the header command line of the image
 * they started. When U-Boot is packed as the kernel of such an image, that is U-Boot's header
 * command line. A slot's kernel should get its own image's header command line in that place.
 *
 * Built on the host too (tests/cmdline-test.c, with -DAB_HOST, ab_read_part from the test).
 */
#ifdef AB_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARCH_DMA_MINALIGN	64
#define memalign(a, n)		aligned_alloc(a, n)
#else
#include <malloc.h>
#include <memalign.h>
#include <stdio.h>
#include <linux/string.h>
#endif
#include "abboot.h"

void ab_hdr_cmdline(const struct ab_bootimg_hdr *h, char *out)
{
	int n;

	memcpy(out, h->cmdline, sizeof(h->cmdline));
	out[sizeof(h->cmdline)] = '\0';
	n = strlen(out);
	while (n && out[n - 1] == ' ')
		out[--n] = '\0';
}

int ab_cmdline_swap(const char *line, const char *was, const char *now, char *buf, size_t size)
{
	const char *at;

	if (!was[0] || !strcmp(was, now))
		return 0;
	at = strstr(line, was);
	if (!at)
		return -1;
	snprintf(buf, size, "%.*s%s%s", (int)(at - line), line, now, at + strlen(was));
	return 1;
}

const char *ab_header_swap(const char *line, const struct ab_bootimg_hdr *h, const char *part)
{
	static char buf[AB_CMDLINE_MAX], was[sizeof(h->cmdline) + 1], now[sizeof(h->cmdline) + 1];
	const struct ab_bootimg_hdr *ph;
	u8 *blk = memalign(ARCH_DMA_MINALIGN, 2048);
	int ret;

	/*
	 * The header at 0x400 behind a 1 KB stub (some devices need one in front of every boot
	 * image), else at byte 0 (where 0x400 falls in the header's zero padding).
	 */
	was[0] = '\0';
	if (blk && !ab_read_part(part, 4, blk)) {
		ph = (const void *)(blk + 0x400);
		if (memcmp(ph->magic, AB_BOOT_MAGIC, 8))
			ph = (const void *)blk;
		if (!memcmp(ph->magic, AB_BOOT_MAGIC, 8))
			ab_hdr_cmdline(ph, was);
	}
	free(blk);
	ab_hdr_cmdline(h, now);
	ret = ab_cmdline_swap(line, was, now, buf, sizeof(buf));
	if (ret < 0)
		printf("abboot: %s's header command line is not in the line: kept as it is\n", part);
	else if (ret > 0)
		printf("abboot: the image's header command line in place of %s's\n", part);
	return ret > 0 ? buf : line;
}
