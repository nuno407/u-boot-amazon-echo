// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of the kernel's device tree (abboot/dt.c, ab_dt_merge; tests/dt-test.sh):
 *
 *   dt-test BASE.dtb SLOT.dtb PRIOR.dtb OUT.dtb [CARRY]
 *
 * writes SLOT.dtb with what PRIOR.dtb (the prior stage's tree) adds to BASE.dtb (the tree packed
 * into U-Boot's image), as abboot gives it to a slot's kernel (before booti's /chosen changes);
 * CARRY is CONFIG_AB_DT_CARRY.
 */
#include <stdio.h>
#include <stdlib.h>
#include <libfdt.h>
#include "abboot.h"

static void *slurp(const char *name, size_t *n)
{
	FILE *f = fopen(name, "rb");
	char *b;

	if (!f || fseek(f, 0, SEEK_END))
		return NULL;
	*n = ftell(f);
	rewind(f);
	b = calloc(1, *n);
	if (!b || fread(b, 1, *n, f) != *n)
		return NULL;
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	size_t nb, ns, np;
	void *base, *slot, *prior, *out;
	FILE *f;
	int ret;

	if (argc != 5 && argc != 6)
		return 2;
	base = slurp(argv[1], &nb);
	slot = slurp(argv[2], &ns);
	prior = slurp(argv[3], &np);
	out = calloc(1, ns + 0x40000);
	if (!base || !slot || !prior || !out)
		return 1;
	ret = fdt_open_into(slot, out, ns + 0x40000);
	if (!ret)
		ret = ab_dt_merge(out, prior, base, argc == 6 ? argv[5] : "");
	if (!ret)
		ret = fdt_pack(out);
	if (ret) {
		fprintf(stderr, "dt-test: %s\n", fdt_strerror(ret));
		return 1;
	}
	f = fopen(argv[4], "wb");
	fwrite(out, 1, fdt_totalsize(out), f);
	fclose(f);
	return 0;
}
