// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of the menu's keys (abboot/menu.c, ab_keys_step): key sequences sampled every 10 ms,
 * as the menu samples board_ab_keys(), and the events they make.
 *
 *   cc -DAB_HOST -I../abboot menu-test.c ../abboot/menu.c
 *
 * A script is a list of (time in ms, raw keys) changes; each case checks the events in order and
 * the time of each.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "abboot.h"

#define U	AB_KEY_UP
#define D	AB_KEY_DOWN
#define B	(AB_KEY_UP | AB_KEY_DOWN)

struct change { ulong at; unsigned int raw; };
struct event { enum ab_key_event ev; ulong at; };

static const char *const names[] = { "none", "touch", "chord", "up", "down", "ok" };
static int failures;

/*
 * Run a script from time 0 to end (10 ms samples); keys raw at the menu's start; the events (not
 * AB_EV_NONE) into got. Stops after AB_EV_OK, as the menu does. Returns how many.
 */
static int run(unsigned int start, const struct change *c, int nc, ulong end, struct event *got)
{
	struct ab_keys k;
	unsigned int raw = start;
	ulong t;
	int i = 0, n = 0;

	ab_keys_init(&k, start);
	for (t = 0; t <= end; t += 10) {
		enum ab_key_event ev;

		while (i < nc && c[i].at <= t)
			raw = c[i++].raw;
		ev = ab_keys_step(&k, raw);
		if (ev == AB_EV_NONE)
			continue;
		got[n].ev = ev;
		got[n++].at = t;
		if (ev == AB_EV_OK)
			break;
	}
	return n;
}

static void check(const char *name, unsigned int start, const struct change *c, int nc, ulong end,
		  const struct event *want, int nw)
{
	struct event got[64];
	int n = run(start, c, nc, end, got), i;
	int ok = n == nw;

	for (i = 0; ok && i < n; i++)
		ok = got[i].ev == want[i].ev && got[i].at == want[i].at;
	printf("%-4s %s:", ok ? "ok" : "FAIL", name);
	for (i = 0; i < n; i++)
		printf(" %s@%lu", names[got[i].ev], got[i].at);
	printf("\n");
	if (!ok) {
		printf("     want:");
		for (i = 0; i < nw; i++)
			printf(" %s@%lu", names[want[i].ev], want[i].at);
		printf("\n");
		failures++;
	}
}

#define CHECK(name, start, end, c, w) \
	check(name, start, c, sizeof(c) / sizeof(c[0]), end, w, sizeof(w) / sizeof(w[0]))

/* For the chords: no move, ever; OK, last, at ok_at (the last key's release debounced). */
static void no_move(const char *name, unsigned int start, const struct change *c, int nc, ulong end,
		    ulong ok_at)
{
	struct event got[64];
	int n = run(start, c, nc, end, got), i;
	int ok = n && got[n - 1].ev == AB_EV_OK && got[n - 1].at == ok_at;

	for (i = 0; i < n; i++)
		if (got[i].ev == AB_EV_UP || got[i].ev == AB_EV_DOWN)
			ok = 0;
	if (!ok) {
		printf("FAIL %s:", name);
		for (i = 0; i < n; i++)
			printf(" %s@%lu", names[got[i].ev], got[i].at);
		printf(" (want ok@%lu, no move)\n", ok_at);
		failures++;
	}
}

int main(void)
{
	/* Debounce: a change counts at its third sample in a row (20 ms after it, 10 ms samples). */

	/* A tap of Up (80 ms): touch when it is down, the move when it is let go. */
	static const struct change tap_up[] = { { 100, U }, { 180, 0 } };
	static const struct event tap_up_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_UP, 200 } };
	CHECK("single up (tap)", 0, 1000, tap_up, tap_up_w);

	static const struct change tap_down[] = { { 100, D }, { 200, 0 } };
	static const struct event tap_down_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_DOWN, 220 } };
	CHECK("single down (tap)", 0, 1000, tap_down, tap_down_w);

	/* Two taps: two moves. */
	static const struct change two[] = { { 100, D }, { 160, 0 }, { 400, D }, { 460, 0 } };
	static const struct event two_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_DOWN, 180 },
					      { AB_EV_TOUCH, 420 }, { AB_EV_DOWN, 480 } };
	CHECK("two downs", 0, 1000, two, two_w);

	/* A hold of 2 s: nothing while held, one move at release. */
	static const struct change hold_up[] = { { 100, U }, { 2100, 0 } };
	static const struct event hold_up_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_UP, 2120 } };
	CHECK("hold up 2 s", 0, 3000, hold_up, hold_up_w);

	static const struct change hold[] = { { 100, D }, { 30100, 0 } };
	static const struct event hold_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_DOWN, 30120 } };
	CHECK("hold down 30 s", 0, 31000, hold, hold_w);

	/* Up held, Down pressed 1 s later; Up let go first, then Down: OK at the last release. */
	static const struct change c_uu[] = { { 100, U }, { 1100, B }, { 1500, D }, { 1800, 0 } };
	static const struct event c_uu_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_CHORD, 1120 },
					       { AB_EV_OK, 1820 } };
	CHECK("up held, down 1 s later, up let go first", 0, 3000, c_uu, c_uu_w);

	/* ... Down let go first. */
	static const struct change c_ud[] = { { 100, U }, { 1100, B }, { 1500, U }, { 1800, 0 } };
	static const struct event c_ud_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_CHORD, 1120 },
					       { AB_EV_OK, 1820 } };
	CHECK("up held, down 1 s later, down let go first", 0, 3000, c_ud, c_ud_w);

	/* ... both let go together. */
	static const struct change c_ub[] = { { 100, U }, { 1100, B }, { 1800, 0 } };
	static const struct event c_ub_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_CHORD, 1120 },
					       { AB_EV_OK, 1820 } };
	CHECK("up held, down 1 s later, both let go", 0, 3000, c_ub, c_ub_w);

	/* Down first, Up 1 s later, either release order. */
	static const struct change c_du[] = { { 100, D }, { 1100, B }, { 1500, D }, { 1800, 0 } };
	static const struct event c_du_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_CHORD, 1120 },
					       { AB_EV_OK, 1820 } };
	CHECK("down held, up 1 s later, up let go first", 0, 3000, c_du, c_du_w);

	static const struct change c_dd[] = { { 100, D }, { 1100, B }, { 1500, U }, { 1800, 0 } };
	static const struct event c_dd_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_CHORD, 1120 },
					       { AB_EV_OK, 1820 } };
	CHECK("down held, up 1 s later, down let go first", 0, 3000, c_dd, c_dd_w);

	/* Both on the same sample, held 2 s: OK at release. */
	static const struct change c_same[] = { { 100, B }, { 2100, 0 } };
	static const struct event c_same_w[] = { { AB_EV_CHORD, 120 }, { AB_EV_OK, 2120 } };
	CHECK("both together, held 2 s", 0, 3000, c_same, c_same_w);

	/*
	 * Rolled: Down pressed at 140, Up let go at 150. Down is debounced down at 160, before Up's
	 * release counts (170): both were down at 160, the chord; OK when Down is let go.
	 */
	static const struct change c_roll[] = { { 100, U }, { 140, B }, { 150, D }, { 300, 0 } };
	static const struct event c_roll_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_CHORD, 160 },
						 { AB_EV_OK, 320 } };
	CHECK("chord, rolled", 0, 1000, c_roll, c_roll_w);

	/* A chord let go one key at a time, the first pressed again before the other is up: OK. */
	static const struct change c_re[] = { { 100, U }, { 300, B }, { 500, D }, { 600, B },
					      { 700, 0 } };
	static const struct event c_re_w[] = { { AB_EV_TOUCH, 120 }, { AB_EV_CHORD, 320 },
					       { AB_EV_OK, 720 } };
	CHECK("chord, a key pressed again", 0, 1000, c_re, c_re_w);

	/* A key held when the menu starts (held for the bootloader before U-Boot): nothing, even its release. */
	static const struct change held[] = { { 500, 0 } };
	static const struct event held_w[] = { { AB_EV_NONE, 0 } };
	check("held at start", D, held, 1, 1000, held_w, 0);

	/* ... and then a press counts. */
	static const struct change held2[] = { { 500, 0 }, { 700, U }, { 760, 0 } };
	static const struct event held2_w[] = { { AB_EV_TOUCH, 720 }, { AB_EV_UP, 780 } };
	CHECK("held at start, then up", D, 1000, held2, held2_w);

	/* Up pressed while the start key is still held: nothing until both are let go. */
	static const struct change held3[] = { { 300, B }, { 500, U }, { 600, 0 }, { 800, D },
					       { 850, 0 } };
	static const struct event held3_w[] = { { AB_EV_TOUCH, 820 }, { AB_EV_DOWN, 870 } };
	CHECK("held at start, other pressed", D, 1000, held3, held3_w);

	/* Both held at start: nothing, not even a chord. */
	static const struct change held4[] = { { 300, D }, { 500, 0 } };
	check("both held at start", B, held4, 2, 1000, held_w, 0);

	/* Bounce: 10 ms glitches never count. */
	static const struct change bounce[] = { { 100, U }, { 110, 0 }, { 200, D }, { 220, 0 },
						{ 400, U }, { 410, 0 }, { 420, U }, { 470, 0 } };
	static const struct event bounce_w[] = { { AB_EV_TOUCH, 440 }, { AB_EV_UP, 490 } };
	CHECK("bounce", 0, 1000, bounce, bounce_w);

	/*
	 * Every chord: either key first, the second 0 ms to 3 s later, either key let go first,
	 * 0 ms to 1 s apart. Always OK at the last release, never a move.
	 */
	{
		static const ulong lags[] = { 0, 10, 50, 140, 150, 160, 300, 1000, 3000 };
		static const ulong gaps[] = { 0, 10, 100, 1000 };
		int first, last, li, gi, cases = 0;

		for (first = 0; first < 2; first++)
			for (last = 0; last < 2; last++)
				for (li = 0; li < 9; li++)
					for (gi = 0; gi < 4; gi++) {
						ulong t2 = 100 + lags[li], t3 = t2 + 500;
						ulong t4 = t3 + gaps[gi];
						struct change c[] = {
							{ 100, first ? D : U }, { t2, B },
							{ t3, gaps[gi] ? (last ? D : U) : 0 }, { t4, 0 } };
						char name[64];

						snprintf(name, sizeof(name),
							 "chord %s first, lag %lu, gap %lu",
							 first ? "down" : "up", lags[li], gaps[gi]);
						no_move(name, 0, c, 4, t4 + 1000, t4 + 20);
						cases++;
					}
		printf("%-4s %d chords, either key first, any lag, any release order: OK at the last "
		       "release, no move\n", failures ? "?" : "ok", cases);
	}

	if (failures) {
		printf("menu-test: %d FAILED\n", failures);
		return 1;
	}
	printf("menu-test: all passed\n");
	return 0;
}
