// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * abboot: the kernel's device tree gets what the prior stage added to its own (abboot.h,
 * ab_dt_merge; SPEC.md, "The device tree").
 *
 * The prior stage (the device's bootloader) hands U-Boot the device tree packed into U-Boot's own boot image, with its
 * additions: the device's own data, memory, reserved blocks, /chosen. Those additions are found by
 * comparing the two trees, with no names listed: every property of the prior tree that the packed
 * tree does not have, or has with another value, is set at the same path in the booted image's
 * tree (its nodes made as needed). phandles are left out, as the booted tree has its own. Nothing
 * is deleted. Per-unit data that is in the packed tree itself (for example a calibration
 * blob an OS put into the device's boot image) is no addition; CONFIG_AB_DT_CARRY names such properties, and they are copied from the
 * prior tree into the booted tree's node that holds the same property with the same length.
 *
 * Only libfdt: the same file builds on the host (tests/dt-test.sh, -DAB_HOST).
 */
#ifdef AB_HOST
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <libfdt.h>
#else
#include <stdio.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#endif
#include "abboot.h"

#define PATH_MAX_LEN	256

struct merge {
	void *dst;
	const void *prior, *base;
	int props, nodes;
};

/* dst's node at path, made (with its parents) if it is not there. */
static int dst_node(void *dst, const char *path)
{
	char part[PATH_MAX_LEN];
	const char *p = path;
	int node = 0;

	while (*p == '/')
		p++;
	while (*p) {
		const char *e = strchr(p, '/');
		int n, sub;

		if (!e)
			e = p + strlen(p);
		n = e - p;

		if (n >= sizeof(part))
			return -FDT_ERR_NOSPACE;
		memcpy(part, p, n);
		part[n] = '\0';
		sub = fdt_subnode_offset(dst, node, part);
		if (sub == -FDT_ERR_NOTFOUND)
			sub = fdt_add_subnode(dst, node, part);
		if (sub < 0)
			return sub;
		node = sub;
		p = *e ? e + 1 : e;
	}
	return node;
}

static bool skip_prop(const char *name)
{
	return !strcmp(name, "phandle") || !strcmp(name, "linux,phandle");
}

/* The prior tree's node pnode (at path), against the packed tree's bnode (< 0: not there). */
static int merge_node(struct merge *m, int pnode, int bnode, char *path, int plen)
{
	int prop, child, ret, made = 0;

	fdt_for_each_property_offset(prop, m->prior, pnode) {
		const char *name;
		const void *val, *bval;
		int len, blen, dnode;

		val = fdt_getprop_by_offset(m->prior, prop, &name, &len);
		if (!val)
			return len;
		if (skip_prop(name))
			continue;
		bval = bnode < 0 ? NULL : fdt_getprop(m->base, bnode, name, &blen);
		if (bval && blen == len && !memcmp(bval, val, len))
			continue;
		dnode = dst_node(m->dst, path);
		if (dnode < 0)
			return dnode;
		ret = fdt_setprop(m->dst, dnode, name, val, len);
		if (ret)
			return ret;
		m->props++;
		made = 1;
	}
	/* A node the packed tree does not have is made even without properties. */
	if (bnode < 0 && !made) {
		ret = dst_node(m->dst, path);
		if (ret < 0)
			return ret;
	}
	if (bnode < 0)
		m->nodes++;
	fdt_for_each_subnode(child, m->prior, pnode) {
		const char *name = fdt_get_name(m->prior, child, NULL);
		int n, bchild;

		if (!name)
			return -FDT_ERR_BADSTRUCTURE;
		n = snprintf(path + plen, PATH_MAX_LEN - plen, "%s%s", plen > 1 ? "/" : "", name);
		if (n >= PATH_MAX_LEN - plen)
			return -FDT_ERR_NOSPACE;
		bchild = bnode < 0 ? -1 : fdt_subnode_offset(m->base, bnode, name);
		ret = merge_node(m, child, bchild < 0 ? -1 : bchild, path, plen + n);
		path[plen] = '\0';
		if (ret)
			return ret;
	}
	return 0;
}

/* The first node of fdt holding prop, or an error; *val and *len set. */
static int find_prop(const void *fdt, const char *prop, const void **val, int *len)
{
	int node, depth = 0;

	for (node = fdt_next_node(fdt, -1, &depth); node >= 0;
	     node = fdt_next_node(fdt, node, &depth)) {
		*val = fdt_getprop(fdt, node, prop, len);
		if (*val)
			return node;
	}
	return -FDT_ERR_NOTFOUND;
}

static int carry(void *dst, const void *prior, const char *names)
{
	char name[64];
	const char *p = names;

	while (*p) {
		const void *dv, *pv;
		int n, dl, pl, dnode, pnode, ret;

		while (*p == ' ')
			p++;
		n = strcspn(p, " ");
		if (!n)
			break;
		snprintf(name, sizeof(name), "%.*s", n, p);
		p += n;
		dnode = find_prop(dst, name, &dv, &dl);
		pnode = find_prop(prior, name, &pv, &pl);
		if (dnode < 0 || pnode < 0 || dl != pl) {
			printf("abboot: dtb: %s: booted tree %d, prior tree %d bytes: left as is\n", name,
			       dnode < 0 ? -1 : dl, pnode < 0 ? -1 : pl);
			continue;
		}
		ret = fdt_setprop_inplace(dst, dnode, name, pv, pl);
		if (ret)
			return ret;
		printf("abboot: dtb: %s carried from the prior tree (%d bytes)\n", name, pl);
	}
	return 0;
}

int ab_dt_merge(void *dst, const void *prior, const void *base, const char *carry_names)
{
	struct merge m = { dst, prior, base, 0, 0 };
	char path[PATH_MAX_LEN] = "/";
	int ret;

	ret = merge_node(&m, 0, base ? 0 : -1, path, 1);
	if (ret)
		return ret;
	printf("abboot: dtb: %d properties from the prior tree (%d nodes new to the packed tree)\n",
	       m.props, m.nodes);
	return carry_names && *carry_names ? carry(dst, prior, carry_names) : 0;
}
