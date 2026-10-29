// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * abboot's example board: QEMU's arm64 "virt" machine. It needs no hardware and no vendor facts, so
 * it doubles as the skeleton to copy when porting abboot to a real board. Every hook abboot offers
 * is listed below; the ones this board does not need are left as comments, with what a real board
 * would put there.
 *
 * On QEMU, U-Boot runs on QEMU's own device tree (gd->fdt_blob), and the disk is virtio
 * (CONFIG_AB_DEV_IFACE "virtio"). tests/qemu/ builds a disk with the partitions abboot expects
 * and boots it.
 */

#include <asm/global_data.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <stdio.h>
#include "../../../boot/abboot/abboot.h"
#include "../../../boot/abboot/crumbs.h"

DECLARE_GLOBAL_DATA_PTR;

/*
 * The device tree the previous stage handed over. On a phone-style device this is the tree the
 * vendor bootloader patched for the kernel (with memory, reserved regions and per-device data),
 * kept by CONFIG_SAVE_PREV_BL_FDT_ADDR; the default hook reads it from $prevbl_fdt_addr. On QEMU
 * it is QEMU's tree, which U-Boot also runs on.
 */
const void *board_ab_prior_fdt(void)
{
	return gd->fdt_blob;
}

/* src's node snode, its properties and subnodes, into dst under dparent (merged if there). */
static int copy_node(void *dst, int dparent, const void *src, int snode)
{
	const char *name = fdt_get_name(src, snode, NULL);
	int dnode, prop, child, ret;

	if (!name)
		return -FDT_ERR_BADSTRUCTURE;
	dnode = fdt_subnode_offset(dst, dparent, name);
	if (dnode == -FDT_ERR_NOTFOUND)
		dnode = fdt_add_subnode(dst, dparent, name);
	if (dnode < 0)
		return dnode;
	fdt_for_each_property_offset(prop, src, snode) {
		const char *pname;
		const void *val;
		int len;

		val = fdt_getprop_by_offset(src, prop, &pname, &len);
		if (!val)
			return len;
		if (!strcmp(pname, "phandle"))
			continue;
		ret = fdt_setprop(dst, dnode, pname, val, len);
		if (ret)
			return ret;
	}
	fdt_for_each_subnode(child, src, snode) {
		ret = copy_node(dst, dnode, src, child);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * The slot's own device tree (appended to its kernel), opened with CONFIG_AB_FDT_GROW of room: add
 * what only the previous stage knows. Here: QEMU's /memory and /psci, so a slot's tree need not
 * know how much RAM the machine was started with. A real board copies what its bootloader adds
 * (memory size, reserved regions, calibration data), and nothing else.
 */
int board_ab_fixup_fdt(void *fdt, const void *prior)
{
	int node, ret, n = 0;

	if (!prior)
		return 0;
	fdt_for_each_subnode(node, prior, 0) {
		const char *name = fdt_get_name(prior, node, NULL);

		if (!name || (strncmp(name, "memory", 6) && strcmp(name, "psci")))
			continue;
		ret = copy_node(fdt, 0, prior, node);
		if (ret)
			return ret;
		n++;
	}
	printf("abboot-qemu: %d nodes from QEMU's tree added to the slot's\n", n);
	return 0;
}

/*
 * The menu's keys, raw: AB_KEY_UP and AB_KEY_DOWN for the keys held down now. abboot debounces
 * them and makes the chord (both together: start). QEMU has no keys, so the default (none: the
 * menu counts down and boots the pick) is kept. A real board reads two GPIO inputs, e.g.
 *
 *	unsigned int board_ab_keys(void)
 *	{
 *		return (gpio_low(VOLUME_UP) ? AB_KEY_UP : 0) |
 *		       (gpio_low(VOLUME_DOWN) ? AB_KEY_DOWN : 0);
 *	}
 */

/*
 * Other hooks, all optional (abboot.h has the details):
 *
 *	bool board_ab_menu_visible(void);	false: no menu, nothing drawn this boot
 *	void board_ab_screen_prepare(void);	before the menu draws (clear a framebuffer)
 *	int board_ab_bootargs(const void *prior, const struct ab_bootimg_hdr *h, char *out,
 *			      size_t size);	the base command line
 *	int board_ab_extra_args(char *out, int len, size_t size);	tokens at the end
 *	void board_ab_last_resort(void);	a last kernel to try before the stay screen
 *	void board_ab_before_boot(void);	right before booti
 */

#if IS_ENABLED(CONFIG_AB_CRUMBS)
/* With uboot/patches/0002: an exception or a panic leaves its crumb and the console record. */
void board_exception_hook(struct pt_regs *regs)
{
	ab_crumbs_exception(regs);
}

void board_panic_hook(const char *msg)
{
	ab_crumbs_panic(msg);
}
#endif
