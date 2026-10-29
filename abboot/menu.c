// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * abboot: the screen and the boot menu, on whatever U-Boot video device the board has (for example
 * simplefb over a framebuffer the previous bootloader left running). SPEC.md, "The menu".
 *
 * The menu, centered: one line per slot, "Slot A (good) - <label>", the selected one dark gray
 * on a white bar, the others white (gray when they cannot be chosen); "Boot in 5s." below,
 * counting down; a small gray hint at the bottom (CONFIG_AB_MENU_HINT). Up and Down move
 * the bar, both together start the selected entry, each when let go (board_ab_keys: AB_KEY_UP,
 * AB_KEY_DOWN; the logic is ab_keys_step, below). Keys are sampled every 10 ms. Any press stops the
 * countdown; after that, 60 s without one boots the selected entry, so an unattended unit never
 * waits for ever. The watchdog is fed throughout (udelay runs U-Boot's cyclic feed).
 *
 * With -DAB_HOST only the keys' logic is built (tests/menu-test.c).
 */
#ifndef AB_HOST
#include <dm.h>
#include <dm/device-internal.h>
#include <stdio.h>
#include <time.h>
#include <video.h>
#include <video_console.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/string.h>
#endif
#include "abboot.h"
#ifndef AB_HOST
#include "crumbs.h"
#endif

/*
 * --- The keys: Up, Down, and both together. ---
 *
 * Each key is debounced on its own: a change counts after three 10 ms samples in a row. A key
 * already down when the menu appears (held for the bootloader before U-Boot) counts for nothing,
 * and nothing counts until every key has been let go.
 *
 * A key acts only when it is let go; pressing does nothing but stop the countdown. A press runs
 * from the first key down to every key up. If both keys were down together at any moment of it
 * (either first, the second at any delay: there is no window), it is the chord, OK, made when the
 * last key is released, whichever that is, and no move is made for that press. If only one key
 * was down, it is one move when it is released, however long it was held; no repeat. While both
 * are down the bar shows it (inverted), so the user knows letting go will start the entry.
 */
static unsigned int key_bit(int i)
{
	return i ? AB_KEY_DOWN : AB_KEY_UP;
}

void ab_keys_init(struct ab_keys *k, unsigned int raw)
{
	k->state = raw & AB_KEY_ALL;
	k->count[0] = k->count[1] = 0;
	k->armed = !k->state;
	k->first = 0;
	k->chord = false;
}

static enum ab_key_event move(unsigned int key)
{
	return key == AB_KEY_UP ? AB_EV_UP : AB_EV_DOWN;
}

enum ab_key_event ab_keys_step(struct ab_keys *k, unsigned int raw)
{
	enum ab_key_event ev = AB_EV_NONE;
	int i;

	for (i = 0; i < 2; i++) {
		unsigned int b = key_bit(i);

		if (!(raw & b) == !(k->state & b)) {
			k->count[i] = 0;
		} else if (++k->count[i] >= 3) {
			k->state ^= b;
			k->count[i] = 0;
		}
	}
	if (!k->armed) {
		k->armed = !k->state;
		return AB_EV_NONE;
	}
	if (k->chord) {
		/* The chord: nothing counts until the last key is let go, then OK. */
		if (k->state)
			return AB_EV_NONE;
		k->chord = false;
		k->first = 0;
		return AB_EV_OK;
	}
	if (k->state == AB_KEY_ALL) {
		k->chord = true;
		return AB_EV_CHORD;
	}
	if (k->first && k->state != k->first) {
		/* The key let go, the other never with it: its move. */
		ev = move(k->first);
		k->first = 0;
	}
	if (!k->first && k->state) {
		/* A key down (or, rarely, the other one in the very sample the first went up). */
		k->first = k->state;
		if (ev == AB_EV_NONE)
			ev = AB_EV_TOUCH;
	}
	return ev;
}

#ifndef AB_HOST
#define WHITE	0xffffffff
#define BLACK	0xff000000
#define GRAY	0xff808080
#define BAR_FG	0xff333333	/* dark gray on the white bar */
#define BIG	"16x32"
#define SMALL	"8x16"

void ab_wait_ms(ulong ms)
{
	ulong t = get_timer(0);

	while (get_timer(t) < ms)
		udelay(10000);	/* schedule() inside: the watchdog stays fed */
}

bool ab_screen_visible(void)
{
	static int visible = -1;

	if (visible < 0) {
		visible = board_ab_menu_visible();
		printf("abboot: screen %s\n", visible ? "shown" : "not shown: no menu, no notes");
	}
	return visible;
}

#if !IS_ENABLED(CONFIG_VIDEO)
void ab_screen_clear(void)
{
}

void ab_say(const char *msg)
{
	printf("abboot: %s\n", msg);
}

int ab_menu(struct ab_menu_entry *e, int n, int def, const char *note)
{
	if (note && note[0])
		ab_say(note);
	ab_crumb(ST_MENU, false, "menu: no video");
	return def;
}
#else
static struct udevice *con;
static int lines_said;

static void screen_open(void)
{
	static bool tried;
	struct video_priv *vp;
	struct udevice *vid;
	int ret;

	if (tried)
		return;
	tried = true;
	if (!ab_screen_visible()) {
		ab_crumb(ST_VIDEO, true, "screen not shown");
		return;
	}
	if (!IS_ENABLED(CONFIG_VIDEO)) {
		ab_crumb(ST_VIDEO, true, "video off");
		return;
	}
	/* The console is the video device's child, made when the video device probes. */
	ab_crumb(ST_VIDPROBE, true, "video probe");
	ret = uclass_first_device_err(UCLASS_VIDEO, &vid);
	ab_crumb(ST_VIDDEV, !ret, "video dev %d", ret);
	if (ret) {
		printf("abboot: no video device (%d)\n", ret);
		ab_crumb(ST_VIDEO, false, "no video dev %d", ret);
		return;
	}
	printf("abboot: video device %s\n", vid->name);
	ab_crumb(ST_CONPROBE, true, "console probe");
	ret = device_find_first_child_by_uclass(vid, UCLASS_VIDEO_CONSOLE, &con);
	if (!ret)
		ret = device_probe(con);
	if (ret) {
		con = NULL;
		printf("abboot: no video console (%d)\n", ret);
		ab_crumb(ST_VIDEO, false, "no console %d", ret);
		return;
	}
	/*
	 * Opaque colors: a display layer that blends by pixel alpha (an a8r8g8b8 framebuffer) shows
	 * U-Boot's x8r8g8b8 colors, alpha byte 0, as nothing at all. (colour_fg and colour_bg are
	 * U-Boot's own field names.)
	 */
	vp = dev_get_uclass_priv(con->parent);
	vp->colour_fg |= 0xff000000;
	vp->colour_bg |= 0xff000000;
	video_clear(con->parent);
	ab_crumb(ST_VIDEO, true, "video %dx%d r%d", vp->xsize, vp->ysize, vp->rot);
}

static int rows(const char *font)
{
	vidconsole_select_font(con, font, 0);
	return ((struct vidconsole_priv *)dev_get_uclass_priv(con))->rows;
}

static void put_at(int col, int row, const char *str, u32 fg, u32 bg, const char *font)
{
	struct video_priv *vp = dev_get_uclass_priv(con->parent);
	u32 ofg = vp->colour_fg, obg = vp->colour_bg;

	vidconsole_select_font(con, font, 0);
	vp->colour_fg = fg;
	vp->colour_bg = bg;
	vidconsole_position_cursor(con, col, row);
	vidconsole_put_string(con, str);
	vp->colour_fg = ofg;
	vp->colour_bg = obg;
}

static void put_centered(int row, const char *str, u32 fg, u32 bg, const char *font)
{
	struct vidconsole_priv *p;
	int len = strlen(str);

	vidconsole_select_font(con, font, 0);
	p = dev_get_uclass_priv(con);
	put_at(len < p->cols ? (p->cols - len) / 2 : 0, row, str, fg, bg, font);
}

void ab_screen_clear(void)
{
	screen_open();
	if (con)
		video_clear(con->parent);
	lines_said = 0;
}

void ab_say(const char *msg)
{
	printf("abboot: %s\n", msg);
	screen_open();
	if (!con)
		return;
	if (!lines_said)
		ab_crumb(ST_DRAW, true, "draw");
	put_centered(rows(BIG) / 2 - 1 + lines_said++, msg, WHITE, BLACK, BIG);
	video_sync(con->parent, true);
	if (lines_said == 1)
		ab_crumb(ST_TEXT, true, "text drawn");
}

/* The layout, from the console's rows: for example 25 big rows and 50 small ones on 1280x800. */
struct layout {
	int first;		/* the first entry's row (big font) */
	int count;		/* "Boot in 5s." */
	int hint;		/* the hint (small font) */
	int bar;		/* bar width: the longest label plus two columns each side */
};

/* chord: both keys are down, the bar inverted (letting go starts the entry). */
static void draw_entries(const struct layout *l, struct ab_menu_entry *e, int n, int sel,
			 bool chord)
{
	char line[40];
	int i;

	for (i = 0; i < n; i++) {
		int len = strlen(e[i].label);

		if (len > l->bar)
			len = l->bar;
		memset(line, ' ', l->bar);
		line[l->bar] = '\0';
		memcpy(line + (l->bar - len) / 2, e[i].label, len);
		if (i == sel)
			put_centered(l->first + i, line, chord ? WHITE : BAR_FG, chord ? BAR_FG : WHITE,
				    BIG);
		else
			put_centered(l->first + i, line, e[i].slot >= 0 ? WHITE : GRAY, BLACK, BIG);
	}
	video_sync(con->parent, true);
}

static void draw_count(const struct layout *l, int secs)
{
	char line[33];

	memset(line, ' ', 32);
	line[32] = '\0';
	put_centered(l->count, line, WHITE, BLACK, BIG);
	if (secs >= 0) {
		snprintf(line, sizeof(line), "Boot in %ds.", secs);
		put_centered(l->count, line, WHITE, BLACK, BIG);
	}
	video_sync(con->parent, true);
}

int ab_menu(struct ab_menu_entry *e, int n, int def, const char *note)
{
	struct layout l;
	struct ab_keys keys;
	enum ab_key_event ev;
	const char *how = "timeout";
	ulong start = get_timer(0), last = start;
	int sel = def, shown = -1, i;
	bool counting = true;

	screen_open();
	if (!con) {
		/* Not shown (board_ab_menu_visible) is no failure: the pick boots at once. */
		if (note && note[0])
			printf("abboot: %s\n", note);
		ab_crumb(ST_MENU, !ab_screen_visible(), "menu: %s",
			 ab_screen_visible() ? "no screen" : "not shown");
		return def;
	}
	board_ab_screen_prepare();
	video_clear(con->parent);
	lines_said = 0;
	l.first = rows(BIG) * 7 / 25;
	l.count = l.first + n + 3;
	l.hint = rows(SMALL) - 6;
	l.bar = 0;
	for (i = 0; i < n; i++)
		l.bar = max_t(int, l.bar, strlen(e[i].label) + 4);
	l.bar = min(l.bar, 39);

	if (note && note[0])
		put_centered(l.first - 2, note, WHITE, BLACK, BIG);
	draw_entries(&l, e, n, sel, false);
	put_centered(l.hint, CONFIG_AB_MENU_HINT, GRAY, BLACK, SMALL);
	ab_keys_init(&keys, board_ab_keys());
	printf("abboot: menu: keys at start: up %d down %d\n", !!(keys.state & AB_KEY_UP),
	       !!(keys.state & AB_KEY_DOWN));

	for (;;) {
		ev = ab_keys_step(&keys, board_ab_keys());
		if (ev != AB_EV_NONE) {
			last = get_timer(0);
			if (counting) {
				counting = false;
				how = "button";
				draw_count(&l, -1);
			}
			if (ev == AB_EV_OK)
				break;
		}
		if (ev == AB_EV_CHORD)
			draw_entries(&l, e, n, sel, true);
		if (ev == AB_EV_UP || ev == AB_EV_DOWN) {
			do {
				sel = ev == AB_EV_UP ? (sel + n - 1) % n : (sel + 1) % n;
			} while (e[sel].slot < 0);
			printf("abboot: menu: %s\n", e[sel].label);
			draw_entries(&l, e, n, sel, false);
		}
		if (counting) {
			long left = 5000 - (long)get_timer(start);
			int secs = (left + 999) / 1000;

			if (left <= 0)
				break;
			if (secs != shown) {
				draw_count(&l, secs);
				shown = secs;
			}
		} else if (get_timer(last) > 60000) {
			how = "idle";
			break;
		}
		udelay(10000);	/* schedule() inside: the watchdog stays fed */
	}
	/* A note ("Slot A failed, back to slot B") stays at least CONFIG_AB_WARN_MS. */
	while (note && note[0] && get_timer(start) < CONFIG_AB_WARN_MS)
		udelay(10000);
	ab_crumb(ST_MENU, true, "menu %s %c", how, e[sel].slot >= 0 ? 'a' + e[sel].slot : '-');
	printf("abboot: menu: %s, %s\n", how, e[sel].label);
	return sel;
}
#endif
#endif /* !AB_HOST */
