// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * abboot: a board's facts as configuration (SPEC.md, "Porting a board"). The hooks of
 * abboot.h, implemented once from Kconfig values and the control device tree, so that a board is a
 * defconfig, its U-Boot device tree and its packing options, with no C file of its own:
 *  - start-up (CONFIG_AB_BOARD_INIT): the crumbs, the exception and panic hooks, and the
 *    test-slot build's misc command (CONFIG_AB_TESTSLOT);
 *  - the menu's keys: two MMIO bits (CONFIG_AB_KEY_*);
 *  - the screen: the control tree's simple-framebuffer region, cleared before the menu draws
 *    (CONFIG_AB_SCREEN_CLEAR), and drawn into only if the prior stage's tree reserves it
 *    (CONFIG_AB_SCREEN_NEED_RESERVED) and its command line does not say the display stalled
 *    (CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG);
 *  - the test-slot marker and the sticky test build's misc command (CONFIG_AB_STICKY).
 * Every hook stays weak: a board that needs code may still override one.
 */
#include <blk.h>
#include <cpu_func.h>
#include <init.h>
#include <memalign.h>
#include <part.h>
#include <stdio.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/ptrace.h>
#include <dm/ofnode.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include "abboot.h"
#include "crumbs.h"

/* --- Start-up: the crumbs (CONFIG_AB_CRUMBS), exceptions and panics (uboot/patches/0002). --- */

#if IS_ENABLED(CONFIG_AB_BOARD_INIT)
int board_early_init_f(void)
{
	ab_crumb(ST_EARLY, true, "early_f");
	return 0;
}

int board_init(void)
{
	ab_crumbs_init();
	return 0;
}

void board_exception_hook(struct pt_regs *regs)
{
	ab_crumbs_exception(regs);
}

void board_panic_hook(const char *msg)
{
	ab_crumbs_panic(msg);
}

/*
 * Test-slot build: zero the 32-byte command at offset 0 of misc ("boot-recovery", which sent the
 * prior stage here) and nothing else: one block read, its first 32 bytes zeroed, written back only
 * if they were not zero already. Whatever happens next, the following boot is the boot partition's.
 */
static void clear_misc_command(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, blk, 512);
	struct blk_desc *desc;
	struct disk_partition info;
	char was[33];
	int i;

	if (ab_find_part(CONFIG_AB_MISC_PART, &desc, &info)) {
		ab_crumb(ST_MISCCMD, false, "no misc");
		return;
	}
	if (blk_dread(desc, info.start, 1, blk) != 1) {
		ab_crumb(ST_MISCCMD, false, "read failed");
		return;
	}
	for (i = 0; i < 32 && !blk[i]; i++)
		;
	if (i == 32) {
		ab_crumb(ST_MISCCMD, true, "zero");
		ab_crumb(ST_MISCCLR, true, "nothing to do");
		return;
	}
	memcpy(was, blk, 32);
	was[32] = '\0';
	ab_crumb(ST_MISCCMD, true, "set: %.14s", was);
	memset(blk, 0, 32);
	if (blk_dwrite(desc, info.start, 1, blk) != 1) {
		ab_crumb(ST_MISCCLR, false, "write failed");
		return;
	}
	printf("abboot: misc command \"%s\" cleared\n", was);
	ab_crumb(ST_MISCCLR, true, "cleared");
}

int board_late_init(void)
{
	ab_crumb(ST_LATE, true, "late_init");
	if (IS_ENABLED(CONFIG_AB_TESTSLOT))
		clear_misc_command();
	ab_record_dump("late_init");
	return 0;
}
#endif

/* --- The menu's keys. --- */

static bool key_held(ulong reg, int bit)
{
	bool set;

	if (!reg)
		return false;
	set = readl(reg) & BIT(bit);
	return IS_ENABLED(CONFIG_AB_KEYS_ACTIVE_LOW) ? !set : set;
}

/* Raw: the menu (menu.c) debounces them and makes the chord. */
__weak unsigned int board_ab_keys(void)
{
	return (key_held(CONFIG_AB_KEY_UP_REG, CONFIG_AB_KEY_UP_BIT) ? AB_KEY_UP : 0) |
	       (key_held(CONFIG_AB_KEY_DOWN_REG, CONFIG_AB_KEY_DOWN_BIT) ? AB_KEY_DOWN : 0);
}

/* --- The screen. --- */

/* The control tree's simple-framebuffer region: the page simplefb draws into. false if none. */
static bool fb_region(ulong *base, ulong *size)
{
	ofnode node = ofnode_by_compatible(ofnode_null(), "simple-framebuffer");
	fdt_size_t sz;
	fdt_addr_t a;

	if (!ofnode_valid(node))
		return false;
	a = ofnode_get_addr_size_index(node, 0, &sz);
	if (a == FDT_ADDR_T_NONE || !sz)
		return false;
	*base = a;
	*size = sz;
	return true;
}

/* Whether one of the tree's /reserved-memory regions holds [base, base + size). */
static bool prior_reserves(const void *fdt, u64 base, u64 size)
{
	int rm, node, len, ac, sc;

	rm = fdt ? fdt_path_offset(fdt, "/reserved-memory") : -1;
	if (rm < 0)
		return false;
	ac = fdt_address_cells(fdt, rm);
	sc = fdt_size_cells(fdt, rm);
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
		return false;
	fdt_for_each_subnode(node, fdt, rm) {
		const fdt32_t *reg = fdt_getprop(fdt, node, "reg", &len);
		u64 b, s;

		if (!reg || len < 4 * (ac + sc))
			continue;
		b = ac == 2 ? ((u64)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]) : fdt32_to_cpu(reg[0]);
		reg += ac;
		s = sc == 2 ? ((u64)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]) : fdt32_to_cpu(reg[0]);
		if (b <= base && base + size <= b + s)
			return true;
	}
	return false;
}

/* The prior stage's command line holds tok as a whole token. */
static bool prior_arg(const void *fdt, const char *tok)
{
	const char *args, *p;
	int node, len, n = strlen(tok);

	node = fdt ? fdt_path_offset(fdt, "/chosen") : -1;
	args = node < 0 ? NULL : fdt_getprop(fdt, node, "bootargs", &len);
	for (p = args; p && (p = strstr(p, tok)); p += n)
		if ((p == args || p[-1] == ' ') && (!p[n] || p[n] == ' '))
			return true;
	return false;
}

/*
 * Nothing drawn would be seen, so no menu (the pick boots at once):
 *  - the prior stage's tree does not reserve the framebuffer region U-Boot draws into
 *    (CONFIG_AB_SCREEN_NEED_RESERVED; the Echo boards' bootloader reserves it);
 *  - the prior stage's command line says its display stalled (CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG:
 *    on the Echo Show 5 2nd gen, fps=6000).
 */
__weak bool board_ab_menu_visible(void)
{
	const void *prior = board_ab_prior_fdt();
	ulong base, size;

	if (IS_ENABLED(CONFIG_AB_SCREEN_NEED_RESERVED) &&
	    (!fb_region(&base, &size) || !prior_reserves(prior, base, size))) {
		printf("abboot: the prior stage's tree does not reserve the framebuffer: nothing drawn\n");
		return false;
	}
	if (*CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG && prior_arg(prior, CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG)) {
		printf("abboot: %s on the prior stage's command line: no menu this boot\n",
		       CONFIG_AB_MENU_HIDE_IF_PRIOR_ARG);
		return false;
	}
	return true;
}

/*
 * Only the region simplefb draws into (stride x height), never the rest of the prior stage's
 * framebuffer block: on the Echo Show 8 2nd gen clearing all of it froze the display (the
 * region past the visible page is not pixels).
 */
__weak void board_ab_screen_prepare(void)
{
	ulong base, size, i;
	u32 *p;

	if (!IS_ENABLED(CONFIG_AB_SCREEN_CLEAR) || !fb_region(&base, &size))
		return;
	p = (u32 *)base;
	for (i = 0; i < size / 4; i++)
		p[i] = 0xff000000;
	flush_dcache_range(base, base + size);
}

/* --- The test-slot builds. --- */

__weak int board_ab_extra_args(char *out, int len, size_t size)
{
#if IS_ENABLED(CONFIG_AB_TESTSLOT)
	int n = strlen(CONFIG_AB_TEST_MARKER);

	if (len + n + 1 < size)
		len += snprintf(out + len, size - len, " %s", CONFIG_AB_TEST_MARKER);
#endif
	return len;
}

/* Sticky test slot (CONFIG_AB_STICKY): misc's command set to boot-recovery again before booti. */
__weak void board_ab_before_boot(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, blk, 512);
	struct blk_desc *desc;
	struct disk_partition info;

	if (!IS_ENABLED(CONFIG_AB_STICKY))
		return;
	if (ab_find_part(CONFIG_AB_MISC_PART, &desc, &info) || blk_dread(desc, info.start, 1, blk) != 1) {
		ab_crumb(ST_STICKY, false, "sticky: no misc");
		return;
	}
	memset(blk, 0, 32);
	strcpy((char *)blk, "boot-recovery");
	if (blk_dwrite(desc, info.start, 1, blk) != 1) {
		ab_crumb(ST_STICKY, false, "sticky: write failed");
		return;
	}
	printf("abboot: misc command set to boot-recovery: a reset comes back to this U-Boot\n");
	ab_crumb(ST_STICKY, true, "sticky: set");
}
