// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of abboot/ab.c, the boot layer's A/B record: the scenarios of SPEC.md, and
 * records printed in hex for tests/ab-test.sh to compare with abctl.py and bootctl.
 *
 *   cc -DAB_HOST -I../abboot ab-test.c ../abboot/ab.c -lz
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ab.h"

static void hex(const char *name, const u8 *raw)
{
	int i;

	printf("%s ", name);
	for (i = 0; i < 32; i++)
		printf("%02x", raw[i]);
	printf("\n");
}

/* One boot as boot.c does it: the pick, then the try. Returns the slot booted, -1 none. */
static int boot(struct ab_rec *r, int *failed)
{
	int s = ab_select(r, failed);

	if (s >= 0)
		ab_take(r, s, *failed);
	return s;
}

static void set(struct ab_rec *r, int i, int p, int t, int s)
{
	r->slot[i].priority = p;
	r->slot[i].tries = t;
	r->slot[i].successful = s;
	r->slot[i].verity = false;
}

int main(void)
{
	struct ab_rec r, d;
	u8 raw[32];
	int f, s, i;

	/* The default record; encode, decode round trip; the hex goes to abctl.py. */
	memset(raw, 0, 32);
	ab_default(&r);
	ab_encode(&r, raw);
	hex("default", raw);
	assert(ab_decode(raw, &d) == AB_OK);
	assert(d.last == 0 && d.slot[0].priority == 15 && d.slot[1].priority == 14);
	assert(d.slot[0].tries == 3 && d.slot[1].tries == 3 && !d.slot[0].successful);
	/* Both good, b first: a typical record after both slots have booted. */
	r.last = 1;
	set(&r, 0, 14, 0, 1);
	set(&r, 1, 15, 0, 1);
	memset(raw, 0, 32);
	ab_encode(&r, raw);
	hex("b-good-good", raw);

	/* Corruption is caught. */
	raw[13] ^= 1;
	assert(ab_decode(raw, &d) == AB_ECRC);
	memset(raw, 0, 32);
	assert(ab_decode(raw, &d) == AB_EMAGIC);

	/* The default record: a three boots on trial, and with no commit b's turn, then none. */
	ab_default(&r);
	for (i = 2; i >= 0; i--) {
		assert(boot(&r, &f) == 0 && f == -1);
		assert(r.slot[0].tries == i);
	}
	s = boot(&r, &f);
	assert(s == 1 && f == 0 && r.slot[0].priority == 0);	/* "Slot A failed, back to slot B" */

	/* Both good: the first one, every boot, no try taken. */
	set(&r, 0, 14, 0, 1);
	set(&r, 1, 15, 0, 1);
	for (i = 0; i < 5; i++) {
		assert(boot(&r, &f) == 1 && f == -1);
		assert(r.slot[1].tries == 0 && r.slot[1].successful);
	}

	/* A new slot b (bootctl set-active b) whose kernel never comes up: three boots, then a. */
	set(&r, 0, 14, 0, 1);
	set(&r, 1, 15, 3, 0);
	assert(!strcmp(ab_word(&r.slot[1]), "trial"));
	for (i = 2; i >= 0; i--) {
		assert(boot(&r, &f) == 1 && f == -1);
		assert(r.slot[1].tries == i);
	}
	assert(!strcmp(ab_word(&r.slot[1]), "bad"));
	s = boot(&r, &f);
	assert(s == 0 && f == 1);			/* "Slot B failed, back to slot A" */
	assert(r.slot[1].priority == 0 && r.last == 0);
	s = boot(&r, &f);
	assert(s == 0 && f == -1);			/* announced once: b is now just (bad) */

	/* ... and the same slot b marked good after its first boot: good from then on. */
	set(&r, 0, 14, 0, 1);
	set(&r, 1, 15, 3, 0);
	assert(boot(&r, &f) == 1 && r.slot[1].tries == 2);
	r.slot[1].successful = true;			/* bootctl mark-good */
	for (i = 0; i < 5; i++)
		assert(boot(&r, &f) == 1 && f == -1 && r.slot[1].tries == 2);

	/* A manual choice of the other slot takes its try and changes no priority. */
	set(&r, 0, 15, 0, 1);
	set(&r, 1, 14, 3, 0);
	assert(ab_select(&r, &f) == 0 && f == -1);
	ab_take(&r, 1, f);
	assert(r.slot[1].tries == 2 && r.slot[0].priority == 15 && r.slot[1].priority == 14);
	assert(ab_select(&r, &f) == 0);

	/* mark-bad: priority 0, the other is picked, and that is no failure. */
	set(&r, 0, 0, 0, 0);
	set(&r, 1, 14, 0, 1);
	assert(boot(&r, &f) == 1 && f == -1);

	/* Both bad: none. */
	set(&r, 0, 15, 0, 0);
	set(&r, 1, 14, 0, 0);
	assert(ab_select(&r, &f) == -1);
	set(&r, 0, 15, 1, 0);
	set(&r, 1, 0, 0, 0);
	assert(boot(&r, &f) == 0 && r.slot[0].tries == 0);
	assert(boot(&r, &f) == -1 && f == 0);		/* "No slot can start" */

	/* encode keeps the bytes it does not own, and zeroes reserved1. */
	memset(raw, 0, 32);
	raw[9] = 3 << 3;				/* recovery tries */
	raw[13] = 1;					/* slot a verity_corrupted */
	raw[16] = 0x5a;					/* slot_info[2] */
	raw[20] = 0xe3;					/* reserved1[0] */
	raw[25] = 0x77;					/* reserved1[5] */
	set(&r, 0, 15, 0, 1);
	set(&r, 1, 14, 0, 1);
	ab_encode(&r, raw);
	assert(ab_decode(raw, &d) == AB_OK);
	assert((raw[9] >> 3) == 3 && raw[16] == 0x5a && raw[20] == 0 && raw[25] == 0 && d.slot[0].verity);
	assert(ab_select(&d, &f) == 1 && f == 0);	/* a verity-corrupted: failed, back to b */

	printf("ab-test: all passed\n");
	return 0;
}
