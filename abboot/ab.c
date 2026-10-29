// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The boot layer's A/B record: the layout of bootloader_control, in misc. See ab.h and
 * SPEC.md.
 *
 * The record, little-endian, in the layout of AOSP's struct bootloader_control (as published in
 * AOSP's bootloader_message.h; no code is taken from it):
 *    0-3   slot_suffix "_a\0\0" / "_b\0\0"
 *    4-7   magic 0x42414342
 *    8     version 1
 *    9     bits 0-2 nb_slot (2), bits 3-5 recovery_tries_remaining
 *   10-11  reserved0 (AOSP's newer header: byte 10 bits 0-2 merge_status)
 *   12-19  slot_info[0..3], 2 bytes each: byte 0 bits 0-3 priority, 4-6 tries_remaining,
 *          7 successful_boot; byte 1 bit 0 verity_corrupted
 *   20-27  reserved1: zero
 *   28-31  crc32_le: CRC-32 (zlib's) of bytes 0-27
 */
#ifdef AB_HOST
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#else
#include <stdio.h>
#include <linux/string.h>
#include <u-boot/crc.h>
#endif
#include "ab.h"

static u32 get32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static void put32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static u32 crc(const u8 *p, unsigned int n)
{
	return crc32(0, p, n);
}

int ab_decode(const u8 *raw, struct ab_rec *r)
{
	int i;

	if (get32(raw + 4) != AB_MAGIC)
		return AB_EMAGIC;
	if (get32(raw + 28) != crc(raw, 28))
		return AB_ECRC;
	if (raw[8] != AB_VERSION || (raw[9] & 7) < 2)
		return AB_EVERSION;
	r->last = raw[0] == '_' && (raw[1] == 'a' || raw[1] == 'b') ? raw[1] - 'a' : -1;
	for (i = 0; i < 2; i++) {
		const u8 *s = raw + 12 + 2 * i;

		r->slot[i].priority = s[0] & 0xf;
		r->slot[i].tries = (s[0] >> 4) & 7;
		r->slot[i].successful = s[0] >> 7;
		r->slot[i].verity = s[1] & 1;
	}
	return AB_OK;
}

void ab_encode(const struct ab_rec *r, u8 *raw)
{
	int i;

	memset(raw, 0, 4);
	if (r->last == 0 || r->last == 1) {
		raw[0] = '_';
		raw[1] = 'a' + r->last;
	}
	put32(raw + 4, AB_MAGIC);
	raw[8] = AB_VERSION;
	raw[9] = (raw[9] & ~7) | 2;
	for (i = 0; i < 2; i++) {
		const struct ab_slot *s = &r->slot[i];
		u8 *o = raw + 12 + 2 * i;

		o[0] = (s->priority & 0xf) | (s->tries & 7) << 4 | (s->successful ? 0x80 : 0);
		/* verity_corrupted (byte 1 bit 0) is kept as it is: only the OS sets it */
	}
	memset(raw + 20, 0, 8);
	put32(raw + 28, crc(raw, 28));
}

bool ab_bootable(const struct ab_slot *s)
{
	return s->priority && (s->successful || s->tries) && !s->verity;
}

const char *ab_word(const struct ab_slot *s)
{
	if (!ab_bootable(s))
		return "bad";
	return s->successful ? "good" : "trial";
}

void ab_default(struct ab_rec *r)
{
	int i;

	for (i = 0; i < 2; i++) {
		r->slot[i].priority = AB_PRIO_MAX - i;
		r->slot[i].tries = AB_TRIES_NEW;
		r->slot[i].successful = false;
		r->slot[i].verity = false;
	}
	r->last = 0;
}

int ab_select(const struct ab_rec *r, int *failed)
{
	int top, other;

	*failed = -1;
	if (!r->slot[0].priority && !r->slot[1].priority)
		return -1;
	top = r->slot[1].priority > r->slot[0].priority ? 1 : 0;
	other = !top;
	if (ab_bootable(&r->slot[top]))
		return top;
	*failed = top;
	return ab_bootable(&r->slot[other]) ? other : -1;
}

void ab_take(struct ab_rec *r, int chosen, int failed)
{
	if (failed == 0 || failed == 1)
		r->slot[failed].priority = 0;
	if (chosen != 0 && chosen != 1)
		return;
	if (!r->slot[chosen].successful && r->slot[chosen].tries)
		r->slot[chosen].tries--;
	r->last = chosen;
}
