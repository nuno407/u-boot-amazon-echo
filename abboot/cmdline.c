// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * abboot: the boot image's header command line in the kernel's command line (abboot.h,
 * SPEC.md "The command line").
 *
 * The prior stage (on the Echo boards, the device's bootloader) builds the kernel's command line
 * around the header command line of the image it booted, which is U-Boot's: pack-uboot.py copies the header of the kernel image U-Boot
 * replaced. A slot's kernel should get its own image's header command line in that place.
 *
 * Built on the host too (tests/args-test.c, with -DAB_HOST, ab_read_part from the test).
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

/* The key of token t (n bytes): up to and with its '=', or the whole token. */
static bool alt_match(const char *t, int n, const char *alt, int al)
{
	if (al && alt[al - 1] == '=')
		return n >= al && !strncmp(t, alt, al);
	return n == al && !strncmp(t, alt, al);
}

/* The group of keys (space-separated, a|b one group) token t belongs to, or NULL; *glen set. */
static const char *group_of(const char *t, int n, const char *keys, int *glen)
{
	const char *g = keys;

	while (*g) {
		const char *a;
		int gl;

		while (*g == ' ')
			g++;
		gl = strcspn(g, " ");
		for (a = g; a < g + gl; ) {
			int al = strcspn(a, "| ");

			if (al > g + gl - a)
				al = g + gl - a;
			if (alt_match(t, n, a, al)) {
				*glen = gl;
				return g;
			}
			a += al + 1;
		}
		g += gl;
	}
	return NULL;
}

/* Whether s has a token of group g (glen bytes). */
static bool has_group(const char *s, const char *g, int glen)
{
	while (*s) {
		const char *t, *a;
		int n;

		while (*s == ' ')
			s++;
		t = s;
		n = strcspn(s, " ");
		s += n;
		if (!n)
			continue;
		for (a = g; a < g + glen; ) {
			int al = strcspn(a, "| ");

			if (al > g + glen - a)
				al = g + glen - a;
			if (alt_match(t, n, a, al))
				return true;
			a += al + 1;
		}
	}
	return false;
}

/* The tokens of s[0..n) into out at len, but those of a group in keys that now sets. */
static int copy_filtered(char *out, int len, size_t size, const char *s, int n, const char *now,
			 const char *keys)
{
	const char *end = s + n;

	while (s < end) {
		const char *t, *g;
		int tn, gl;

		while (s < end && *s == ' ')
			s++;
		t = s;
		while (s < end && *s != ' ')
			s++;
		tn = s - t;
		if (!tn)
			continue;
		g = group_of(t, tn, keys, &gl);
		if (g && has_group(now, g, gl))
			continue;
		if (len + tn + 2 >= size)
			break;
		if (len)
			out[len++] = ' ';
		memcpy(out + len, t, tn);
		len += tn;
		out[len] = '\0';
	}
	return len;
}

int ab_cmdline_build(const char *line, const char *was, const char *now, const char *keys, char *out,
		     size_t size)
{
	const char *at = NULL, *find = was[0] ? was : now;
	int len = 0, ret = 0, n;

	if (find[0])
		at = strstr(line, find);
	if (!at) {
		snprintf(out, size, "%s", line);
		return was[0] ? -1 : 0;
	}
	if (strcmp(was, now) && was[0])
		ret = 1;
	out[0] = '\0';
	len = copy_filtered(out, len, size, line, at - line, now, keys);
	n = strlen(now);
	if (n && len + n + 2 < size) {
		if (len)
			out[len++] = ' ';
		memcpy(out + len, now, n + 1);
		len += n;
	}
	at += strlen(find);
	copy_filtered(out, len, size, at, strlen(at), now, keys);
	return ret;
}

int ab_header_line(const char *line, const struct ab_bootimg_hdr *h, const char *part, const char *keys,
		   char *out, size_t size)
{
	static char was[sizeof(h->cmdline) + 1], now[sizeof(h->cmdline) + 1];
	const struct ab_bootimg_hdr *ph;
	u8 *blk = memalign(ARCH_DMA_MINALIGN, 2048);
	int ret;

	/*
	 * The header at 0x400 behind a 1 KB stub (some devices need one in front of every boot
	 * image), else at byte 0 (where the header is at 0, 0x400 lies in its zero padding).
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
	ret = ab_cmdline_build(line, was, now, keys, out, size);
	if (ret < 0)
		printf("abboot: %s's header command line is not in the line: kept as it is\n", part);
	else if (ret > 0)
		printf("abboot: the image's header command line in place of %s's\n", part);
	return ret;
}
