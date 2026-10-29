/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * abboot: the boot layer's U-Boot part (SPEC.md). Board-independent: the A/B record in
 * misc (ab.c), the slot convention on the store, the menu (menu.c), the rescue kernel, the stay
 * screen, the command line markers and booti. Every hook below has a default made from Kconfig
 * values and the control device tree (board.c, boot.c, menu.c), so a board is configuration only;
 * each stays weak, for a board that needs code.
 *
 * How a board plugs in: SPEC.md, "Porting a board".
 */
#ifndef __ABBOOT_H
#define __ABBOOT_H

#ifdef AB_HOST
/* The host tests (tests/menu-test.c, tests/args-test.c) build menu.c's keys and cmdline.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long ulong;
#define __packed __attribute__((packed))
#else
#include <linux/types.h>
#endif

struct blk_desc;
struct disk_partition;

/* The Android boot image header, v0/v1, the fields abboot uses (include/android_image.h has all). */
struct ab_bootimg_hdr {
	char magic[8];
	u32 kernel_size, kernel_addr;
	u32 ramdisk_size, ramdisk_addr;
	u32 second_size, second_addr;
	u32 tags_addr, page_size, header_version, os_version;
	char name[16];
	char cmdline[512];
	u32 id[8];
} __packed;

#define AB_BOOT_MAGIC	"ANDROID!"
#define AB_CMDLINE_MAX	4096

/* The menu's keys, as bits of board_ab_keys(): for example Volume Up and Volume Down. */
#define AB_KEY_UP	(1u << 0)
#define AB_KEY_DOWN	(1u << 1)
#define AB_KEY_ALL	(AB_KEY_UP | AB_KEY_DOWN)

/* --- Hooks a board may provide (weak defaults in boot.c and menu.c). --- */

/*
 * The device tree the stage before U-Boot handed over (on the Echo boards the device bootloader's), or NULL. Default: the one
 * at $prevbl_fdt_addr (CONFIG_SAVE_PREV_BL_FDT_ADDR), if it is a valid tree.
 */
const void *board_ab_prior_fdt(void);

/*
 * The kernel's tree, the slot's own DTB opened with CONFIG_AB_FDT_GROW to grow: add what the
 * prior stage adds for this board (memory, reserved blocks, the device's own data). prior may be NULL.
 * 0, or a libfdt error. Default (CONFIG_AB_DT_MERGE): ab_dt_merge against the tree packed into
 * U-Boot's own image, with CONFIG_AB_DT_CARRY.
 */
int board_ab_fixup_fdt(void *fdt, const void *prior);

/*
 * The kernel's command line, before abboot adds its own tokens: into out (size bytes). h is the
 * image being booted (its header command line). 0, or an error. Default: the prior tree's
 * /chosen/bootargs through ab_header_line (CONFIG_AB_HEADER_KEYS) if there is one, else the
 * header's command line.
 */
int board_ab_bootargs(const void *prior, const struct ab_bootimg_hdr *h, char *out, size_t size);

/* More tokens at the end of the command line; returns the new length. Default: CONFIG_AB_TEST_MARKER
 * in a test-slot build (CONFIG_AB_TESTSLOT). */
int board_ab_extra_args(char *out, int len, size_t size);

/*
 * The menu's keys as they are now, raw (no debouncing, no chord logic: menu.c does both): the
 * AB_KEY_UP and AB_KEY_DOWN bits of the keys held down. Up moves the bar up, Down moves it down,
 * both together start the selected entry. Default: CONFIG_AB_KEY_UP_REG/_BIT and
 * CONFIG_AB_KEY_DOWN_REG/_BIT (board.c); a register of 0 is no key.
 */
unsigned int board_ab_keys(void);

/*
 * Whether the screen can show anything this boot. false: no menu (the pick boots at once), no
 * notes and no waits for them, nothing drawn (the console still gets every line), and the stay
 * screen still stays, undrawn. Asked once, early in abboot (after board_ab_prior_fdt). Default
 * (board.c): the prior tree reserves the framebuffer (CONFIG_AB_SCREEN_NEED_RESERVED) and its
 * command line lacks CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG.
 */
bool board_ab_menu_visible(void);

/* Just before the menu draws. Default: the simple-framebuffer region cleared (CONFIG_AB_SCREEN_CLEAR). */
void board_ab_screen_prepare(void);

/* Right before booti. Default: the sticky test build's misc command (CONFIG_AB_STICKY). */
void board_ab_before_boot(void);

/* --- What abboot offers a board. --- */

/* A partition on CONFIG_AB_DEV_IFACE CONFIG_AB_DEV_NUM, by name, with 512-byte blocks. 0 or error. */
int ab_find_part(const char *name, struct blk_desc **desc, struct disk_partition *info);

/* The first blocks (512 bytes) of partition name into buf (cache-aligned). 0 or an error. */
int ab_read_part(const char *name, ulong blocks, void *buf);

/* The command line (cmdline.c). */

/* h's header command line into out (sizeof(h->cmdline) + 1 bytes): NUL-terminated, no trailing spaces. */
void ab_hdr_cmdline(const struct ab_bootimg_hdr *h, char *out);

/*
 * The kernel's command line from the prior stage's (line): the header command line of the image it
 * booted (this U-Boot's, was) replaced by the booted image's (now), and every other token of a
 * parameter group in keys (space-separated; "key=" or a flag, a|b one group) that now sets
 * dropped, so the booted image decides it (its root). Into out; tokens joined by one space. 1
 * replaced, 0 nothing to replace (was empty or the same as now), -1 was not in line (out = line).
 */
int ab_cmdline_build(const char *line, const char *was, const char *now, const char *keys, char *out,
		     size_t size);

/* The same, with was read from partition part's boot image and now from h. */
int ab_header_line(const char *line, const struct ab_bootimg_hdr *h, const char *part, const char *keys,
		   char *out, size_t size);

/*
 * The kernel's tree (dst, opened with room): every property of the prior stage's tree (prior)
 * that base (the tree packed into U-Boot's own image) does not have or has with another value,
 * at the same path (dt.c); then the properties named in carry (space-separated) copied from
 * prior into the dst node holding the same property with the same length. 0 or a libfdt error.
 */
int ab_dt_merge(void *dst, const void *prior, const void *base, const char *carry);

/* The screen is shown this boot (board_ab_menu_visible, asked once). */
bool ab_screen_visible(void);

/* The string every build carries (CONFIG_AB_SELF_MARKER); abboot never boots an image holding it. */
extern const char ab_self_marker[];

/* The screen (menu.c): one centered line below the last; also on the console. */
void ab_say(const char *msg);
/* Clear the screen and start the lines again from the middle. */
void ab_screen_clear(void);
/* Wait, the watchdog fed. */
void ab_wait_ms(ulong ms);

/*
 * The keys' logic (menu.c), host-testable: one call per 10 ms sample of board_ab_keys().
 * Debounced (a key's change counts after three samples in a row); a key already down when the
 * menu starts counts only once every key has been let go. A key acts only when let go: a press
 * of one key alone is a move when it is released, however long it was held; a press during which
 * both keys were down at once, in either order and at any delay, is AB_EV_OK when the last of
 * them is released, with no move. A press itself is AB_EV_TOUCH (the countdown stops), and both
 * down is AB_EV_CHORD (the screen shows it), once per press.
 */
enum ab_key_event { AB_EV_NONE, AB_EV_TOUCH, AB_EV_CHORD, AB_EV_UP, AB_EV_DOWN, AB_EV_OK };
struct ab_keys {
	unsigned int state;	/* debounced: the keys down */
	int count[2];		/* consecutive samples of each key that differ from state */
	bool armed;		/* every key seen up since the menu started */
	unsigned int first;	/* the key whose press is pending, 0 when none */
	bool chord;		/* both keys have been down together in this press */
};
void ab_keys_init(struct ab_keys *k, unsigned int raw);
enum ab_key_event ab_keys_step(struct ab_keys *k, unsigned int raw);

/* The menu (menu.c). */
struct ab_menu_entry {
	const char *label;
	int slot;		/* 0 or 1; -1: shown, not selectable */
};
/*
 * Show the entries, the bar on def, "Boot in 5s."; note (may be "") above them, held at least
 * CONFIG_AB_WARN_MS. Returns the entry chosen. Without a screen, def at once.
 */
int ab_menu(struct ab_menu_entry *e, int n, int def, const char *note);

#endif
