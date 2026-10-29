// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * abboot: the boot layer's boot flow (SPEC.md). Board-independent; the board's part is
 * the hooks in abboot.h.
 *
 *  1. The A/B record from misc (CONFIG_AB_MISC_PART, byte 2048; ab.c). None there and the area
 *     empty: the default record (a priority 15, b 14, both on trial), written at step 4.
 *  2. The pick (ab_select): a slot whose tries ran out without a successful boot has failed, and
 *     the other is picked ("Slot A failed, back to slot B").
 *  3. The menu (CONFIG_AB_MENU): Slot A, Slot B, each with its word from the record and the label
 *     the OS wrote (/slots/<x>/boot/label on the store). Not when board_ab_menu_visible says the
 *     screen shows nothing this boot: then the pick at once, nothing drawn, no note waited for.
 *  5. The slot's kernel, from the slot convention: /slots/<x>/boot/boot.img on the store
 *     (CONFIG_AB_STORE_PART), an Android boot image v0/v1 (gzip'd arm64 Image, the DTB appended,
 *     the slot's initramfs; the header at 0 or at 0x400). Failing that the other slot's kernel
 *     (CONFIG_AB_BORROW_KERNEL) or the other slot whole, then the rescue kernel
 *     (CONFIG_AB_RESCUE_PART, or CONFIG_AB_RESCUE_FILE on the store), then board_ab_last_resort,
 *     then a screen that stays.
 *  6. The try (ab_take), written back unless nothing changed (CONFIG_AB_READONLY: never written).
 *  7. The kernel's tree: the image's own DTB with the board's additions (board_ab_fixup_fdt).
 *  8. The command line: the board's (board_ab_bootargs; default: the prior stage's, with the booted
 *     image's header command line in place of U-Boot's), init= (CONFIG_AB_INIT), for a slot
 *     CONFIG_AB_SLOT_ARGS (the watchdog contract), then ab.slot=<x> ab.kernel=<a|b|rescue>.
 *  9. booti, the watchdog running.
 *
 * Nothing here knows the OS: no file on the store is read but the slot convention's.
 */
#include <blk.h>
#include <command.h>
#include <env.h>
#include <fs.h>
#include <gzip.h>
#include <hexdump.h>
#include <malloc.h>
#include <mapmem.h>
#include <memalign.h>
#include <part.h>
#include <stdio.h>
#include <time.h>
#include <vsprintf.h>
#include <asm/unaligned.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <u-boot/sha1.h>
#include "ab.h"
#include "abboot.h"
#include "crumbs.h"

/* Every build carries this; an image holding it is a U-Boot image and is never booted. */
const char ab_self_marker[] __used = CONFIG_AB_SELF_MARKER;

#define SLOT_FILE(s, f)	"/slots/" s "/boot/" f
#define LABEL_MAX	32

/* --- Partitions and the store. --- */

int ab_find_part(const char *name, struct blk_desc **desc, struct disk_partition *info)
{
	char spec[40];
	int ret;

	snprintf(spec, sizeof(spec), "%d#%s", CONFIG_AB_DEV_NUM, name);
	ret = part_get_info_by_dev_and_name_or_num(CONFIG_AB_DEV_IFACE, spec, desc, info, false);
	if (ret < 0) {
		printf("abboot: no partition named %s on %s %d (%d)\n", name, CONFIG_AB_DEV_IFACE,
		       CONFIG_AB_DEV_NUM, ret);
		return ret;
	}
	if (strcmp((const char *)info->name, name) || (*desc)->blksz != 512) {
		printf("abboot: partition %s: unexpected name %s or block size %lu\n", name,
		       info->name, (*desc)->blksz);
		return -EINVAL;
	}
	return 0;
}

int ab_read_part(const char *name, ulong blocks, void *buf)
{
	struct blk_desc *desc;
	struct disk_partition info;
	int ret = ab_find_part(name, &desc, &info);

	if (ret)
		return ret;
	if (blocks > info.size || blk_dread(desc, info.start, blocks, buf) != blocks)
		return -EIO;
	return 0;
}

/* The store (an ext4 partition) set up for the next fs_* call; 0 or an error. */
static int store_open(void)
{
	struct blk_desc *desc;
	struct disk_partition info;
	char spec[40];
	int part;

	snprintf(spec, sizeof(spec), "%d#%s", CONFIG_AB_DEV_NUM, CONFIG_AB_STORE_PART);
	part = part_get_info_by_dev_and_name_or_num(CONFIG_AB_DEV_IFACE, spec, &desc, &info, false);
	if (part < 0)
		return part;
	return fs_set_blk_dev_with_part(desc, part) ? -EIO : 0;
}

/* A slot's label: the first line of /slots/<x>/boot/label, printable ASCII only; "" if none. */
static void slot_label(int slot, char *out, int size)
{
	static char buf[LABEL_MAX + 1] __aligned(ARCH_DMA_MINALIGN);
	char path[40];
	loff_t got = 0;
	int i, n = 0;

	out[0] = '\0';
	snprintf(path, sizeof(path), "/slots/%c/boot/label", 'a' + slot);
	if (store_open() || !fs_exists(path) || store_open() ||
	    fs_read(path, map_to_sysmem(buf), 0, LABEL_MAX, &got) || got <= 0)
		return;
	for (i = 0; i < got && buf[i] != '\n' && n < size - 1; i++)
		if (buf[i] >= ' ' && buf[i] < 0x7f)
			out[n++] = buf[i];
	while (n && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

/* --- The A/B record (ab.c) in misc. --- */

#define AB_BLOCK	(AB_OFFSET / 512)	/* misc block 4 */
#define AB_BLOCKS	(AB_AREA / 512)		/* 2048-4095 */

static struct {
	struct blk_desc *desc;
	lbaint_t lba;			/* misc block 4, absolute */
	u8 *blk;			/* blocks 4-7 as read */
	bool misc;			/* misc read */
	bool writable;			/* the record may be written (valid, or its area empty) */
	struct ab_rec rec;
	const char *src;		/* where the record came from, for the log */
	int pick, failed;
} ab;

static char ab_note[48];		/* "Slot A failed, back to slot B", drawn above the menu */

static const char *ab_hex(const u8 *raw)
{
	static char hex[2 * AB_SIZE + 1];

	bin2hex(hex, raw, AB_SIZE);
	hex[2 * AB_SIZE] = '\0';
	return hex;
}

static void ab_load(void)
{
	struct disk_partition info;
	bool valid = false, rest_empty = false;
	int i, ret = AB_EMAGIC;

	ab.pick = ab.failed = -1;
	ab.blk = memalign(ARCH_DMA_MINALIGN, AB_BLOCKS * 512);
	if (ab.blk && !ab_find_part(CONFIG_AB_MISC_PART, &ab.desc, &info) &&
	    info.size > AB_BLOCK + AB_BLOCKS) {
		ab.lba = info.start + AB_BLOCK;
		ab.misc = blk_dread(ab.desc, ab.lba, AB_BLOCKS, ab.blk) == AB_BLOCKS;
	}
	if (ab.misc) {
		ret = ab_decode(ab.blk, &ab.rec);
		valid = ret == AB_OK;
		for (i = AB_SIZE; i < AB_BLOCKS * 512 && !ab.blk[i]; i++)
			;
		rest_empty = i == AB_BLOCKS * 512;
		/* Ours to write: a valid record, or a record (broken or none) and nothing else. */
		ab.writable = valid || rest_empty;
		printf("abboot: misc record %s: %s%s\n", valid ? "valid" :
		       ret == AB_ECRC ? "bad crc" : ret == AB_EVERSION ? "bad version" : "none",
		       ab_hex(ab.blk), ab.writable ? "" : ", other data in 2048-4095: never written");
	} else {
		printf("abboot: misc unreadable\n");
	}
	if (valid) {
		ab.src = "misc";
	} else {
		ab_default(&ab.rec);
		ab.src = ab.writable ? "default" : "default, ro";
	}
	for (i = 0; i < 2; i++)
		printf("abboot: slot %c: priority %d tries %d successful %d verity %d (%s)\n", 'a' + i,
		       ab.rec.slot[i].priority, ab.rec.slot[i].tries, ab.rec.slot[i].successful,
		       ab.rec.slot[i].verity, ab_word(&ab.rec.slot[i]));
	ab.pick = ab_select(&ab.rec, &ab.failed);
	printf("abboot: record from %s: pick %c%s%c\n", ab.src, ab.pick < 0 ? '-' : 'a' + ab.pick,
	       ab.failed < 0 ? "" : ", failed: ", ab.failed < 0 ? ' ' : 'a' + ab.failed);
	ab_crumb(ST_AB, ab.pick >= 0, "ab %.7s %c%s%c", ab.src, ab.pick < 0 ? '-' : 'a' + ab.pick,
		 ab.failed < 0 ? "" : " f", ab.failed < 0 ? ' ' : 'a' + ab.failed);
	if (ab.pick >= 0 && ab.failed >= 0)
		snprintf(ab_note, sizeof(ab_note), "Slot %c failed, back to slot %c", 'A' + ab.failed,
			 'A' + ab.pick);
}

/* The choice made: take the try and write the record (not in the read-only build). */
static void ab_commit(int chosen)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, blk, 512);

	ab_take(&ab.rec, chosen, ab.failed);
	printf("abboot: booting %c: priority %d tries %d successful %d\n", 'a' + chosen,
	       ab.rec.slot[chosen].priority, ab.rec.slot[chosen].tries,
	       ab.rec.slot[chosen].successful);
	if (!ab.misc || !ab.writable) {
		ab_crumb(ST_ABW, false, "ab: not writable");
		return;
	}
	memcpy(blk, ab.blk, 512);
	ab_encode(&ab.rec, blk);
	if (!memcmp(blk, ab.blk, AB_SIZE)) {
		printf("abboot: record unchanged, not written\n");
		ab_crumb(ST_ABW, true, "ab: unchanged");
		return;
	}
	if (IS_ENABLED(CONFIG_AB_READONLY)) {
		printf("abboot: read-only build: would write %s\n", ab_hex(blk));
		ab_crumb(ST_ABW, true, "ab: ro, not written");
		return;
	}
	if (blk_dwrite(ab.desc, ab.lba, 1, blk) != 1) {
		printf("abboot: misc write failed\n");
		ab_crumb(ST_ABW, false, "ab: write failed");
		return;
	}
	printf("abboot: written %s\n", ab_hex(blk));
	ab_crumb(ST_ABW, true, "ab: %c p%d t%d s%d", 'a' + chosen, ab.rec.slot[chosen].priority,
		 ab.rec.slot[chosen].tries, ab.rec.slot[chosen].successful);
}

/* The menu, or the pick without one. Returns the slot to boot, -1 if none can. */
static int ab_choose(void)
{
	static char labels[2][48];
	struct ab_menu_entry e[2];
	char label[LABEL_MAX];
	int i, sel;

	ab_load();
	if (ab.pick < 0)
		return -1;
	for (i = 0; i < 2; i++) {
		slot_label(i, label, sizeof(label));
		printf("abboot: slot %c label \"%s\"\n", 'a' + i, label);
		snprintf(labels[i], sizeof(labels[i]), "Slot %c (%s)%s%s", 'A' + i,
			 ab_word(&ab.rec.slot[i]), label[0] ? " - " : "", label);
		e[i].label = labels[i];
		e[i].slot = ab_bootable(&ab.rec.slot[i]) ? i : -1;
	}
	if (!IS_ENABLED(CONFIG_AB_MENU)) {
		if (ab_note[0])
			ab_say(ab_note);
		ab_crumb(ST_MENU, true, "menu off %c", 'a' + ab.pick);
		return ab.pick;
	}
	sel = ab_menu(e, 2, ab.pick, ab_note);
	return e[sel].slot;
}

/* --- Boot images. --- */

static ulong env_addr(const char *name)
{
	return env_get_hex(name, 0);
}

static const char *id_hex(const struct ab_bootimg_hdr *h)
{
	static char hex[41];

	bin2hex(hex, (const u8 *)h->id, 20);
	hex[40] = '\0';
	return hex;
}

/*
 * The header: at the start of the image, or at 0x400 behind a 1 KB stub that some devices need in
 * front of every boot image, with the kernel and ramdisk still at page offsets from the start of the
 * image.
 */
static struct ab_bootimg_hdr *hdr_at(u8 *img)
{
	static const ulong at[] = { 0, 0x400 };
	int i;

	for (i = 0; i < ARRAY_SIZE(at); i++) {
		struct ab_bootimg_hdr *h = (struct ab_bootimg_hdr *)(img + at[i]);
		ulong ps = h->page_size;

		if (!memcmp(h->magic, AB_BOOT_MAGIC, 8) && ps >= 2048 && ps <= SZ_16K &&
		    !(ps & (ps - 1)) && at[i] + sizeof(*h) <= ps)
			return h;
	}
	return NULL;
}

static ulong page_align(const struct ab_bootimg_hdr *h, ulong n)
{
	return ALIGN(n, h->page_size);
}

/* The header id (mkbootimg's SHA-1): kernel, ramdisk, second (and, v1, recovery DTBO) with sizes. */
static bool image_id_ok(const u8 *img, const struct ab_bootimg_hdr *h)
{
	sha1_context ctx;
	u8 sum[20], le[4];

	sha1_starts(&ctx);
	sha1_update(&ctx, img + h->page_size, h->kernel_size);
	put_unaligned_le32(h->kernel_size, le);
	sha1_update(&ctx, le, 4);
	sha1_update(&ctx, img + h->page_size + page_align(h, h->kernel_size), h->ramdisk_size);
	put_unaligned_le32(h->ramdisk_size, le);
	sha1_update(&ctx, le, 4);
	put_unaligned_le32(0, le);
	sha1_update(&ctx, le, 4);		/* second: none */
	if (h->header_version == 1)
		sha1_update(&ctx, le, 4);	/* recovery DTBO: none */
	sha1_finish(&ctx, sum);
	return !memcmp(sum, h->id, 20);
}

static bool holds_marker(const u8 *p, ulong n)
{
	ulong m = strlen(ab_self_marker), i;

	for (i = 0; i + m <= n; i++)
		if (p[i] == ab_self_marker[0] && !memcmp(p + i, ab_self_marker, m))
			return true;
	return false;
}

/*
 * A boot image to $loadaddr: from the store (path) or a partition (part, by name). The partition
 * U-Boot itself lives in (CONFIG_AB_SELF_PART) is refused. *size is the bytes read.
 */
static const char *read_image(const char *path, const char *part, loff_t *size, bool *missing)
{
	ulong addr = env_addr("loadaddr");
	u8 *img = map_sysmem(addr, 0);
	struct ab_bootimg_hdr *h;
	loff_t got = 0;
	ulong t0 = get_timer(0);

	*missing = false;
	if (!addr)
		return "no loadaddr";
	if (part) {
		struct blk_desc *desc;
		struct disk_partition info;
		ulong blocks, ps;

		if (!strcmp(part, CONFIG_AB_SELF_PART))
			return "U-Boot's own partition";
		if (ab_find_part(part, &desc, &info))
			return "no partition";
		if (blk_dread(desc, info.start, 8, img) != 8)
			return "header unreadable";
		h = hdr_at(img);
		if (!h) {
			*missing = true;
			return "no boot image";
		}
		ps = h->page_size;
		*size = ps + ALIGN((loff_t)h->kernel_size, ps) + ALIGN((loff_t)h->ramdisk_size, ps);
		blocks = DIV_ROUND_UP(*size, 512);
		if (*size > CONFIG_AB_IMAGE_MAX || blocks > info.size)
			return "image size";
		if (blk_dread(desc, info.start, blocks, img) != blocks)
			return "image unreadable";
		printf("abboot: partition %s: %lld bytes read in %lu ms\n", part, *size, get_timer(t0));
		return NULL;
	}
	if (store_open())
		return "store unreadable";
	if (fs_size(path, size)) {
		*missing = true;
		return "no kernel file";
	}
	if (*size < 4096 || *size > CONFIG_AB_IMAGE_MAX)
		return "kernel file size";
	if (store_open() || fs_read(path, addr, 0, *size, &got) || got != *size)
		return "kernel read failed";
	printf("abboot: %s: %lld bytes read in %lu ms\n", path, *size, get_timer(t0));
	return NULL;
}

/*
 * A boot image's kernel and tree (a slot's file, the rescue image): the kernel gunzipped to
 * $kernel_addr_r, the image's own DTB with the board's additions at $fdt_addr_r, the ramdisk left
 * in place. NULL, or why not (short, for the crumb); *missing is set when there is simply no image.
 */
static const char *load_image(const char *what, const char *path, const char *part,
			      const void *prior, struct ab_bootimg_hdr **hp, ulong *rd, ulong *rs,
			      bool *missing)
{
	ulong kaddr = env_addr("kernel_addr_r"), faddr = env_addr("fdt_addr_r");
	u8 *img = map_sysmem(env_addr("loadaddr"), 0);
	struct ab_bootimg_hdr *h;
	void *fdt = map_sysmem(faddr, 0), *sdtb;
	loff_t size = 0;
	ulong ks, ps, len, off, dsize = 0;
	const char *why;
	int ret;

	if (!kaddr || !faddr || faddr <= kaddr)
		return "no kernel_addr_r/fdt_addr_r";
	why = read_image(path, part, &size, missing);
	if (why)
		return why;
	h = hdr_at(img);
	if (!h || h->header_version > 1 || !h->kernel_size || !h->ramdisk_size || h->second_size)
		return "bad header";
	ps = h->page_size;
	ks = h->kernel_size;
	*rs = h->ramdisk_size;
	if (ps + ALIGN(ks, ps) + ALIGN(*rs, ps) > size)
		return "image short";
	if (!image_id_ok(img, h))
		return "id mismatch";
	printf("abboot: %s boot image: kernel %lu, ramdisk %lu, header id %s\n", what, ks, *rs,
	       id_hex(h));

	/* The DTB appended to the kernel, found backwards from the end of the kernel area. */
	for (off = ks - 8; off > 0 && off + SZ_2M > ks; off--) {
		const u8 *p = img + ps + off;

		if (get_unaligned_be32(p) == FDT_MAGIC && get_unaligned_be32(p + 4) == ks - off) {
			dsize = ks - off;
			break;
		}
	}
	if (!dsize)
		return "no dtb";
	/* libfdt wants a tree 8-byte aligned; the gzip stream before it has any length. */
	sdtb = memalign(ARCH_DMA_MINALIGN, dsize);
	if (!sdtb)
		return "no memory for dtb";
	memcpy(sdtb, img + ps + off, dsize);
	if (fdt_check_header(sdtb)) {
		free(sdtb);
		return "bad dtb";
	}

	len = ks;
	if (gunzip(map_sysmem(kaddr, 0), faddr - kaddr, img + ps, &len)) {
		free(sdtb);
		return "gunzip fail";
	}
	if (memcmp(map_sysmem(kaddr + 0x38, 0), "ARM\x64", 4)) {
		free(sdtb);
		return "not arm64";
	}
	if (holds_marker(map_sysmem(kaddr, 0), len)) {
		free(sdtb);
		return "a U-Boot image";
	}
	ab_crumb(ST_KERNEL, true, "%.8s kernel ok", what);

	ret = fdt_open_into(sdtb, fdt, dsize + CONFIG_AB_FDT_GROW);
	free(sdtb);
	if (!ret)
		ret = board_ab_fixup_fdt(fdt, prior);
	if (!ret)
		ret = fdt_pack(fdt) ? : fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + SZ_64K);
	if (ret) {
		printf("abboot: dtb: %s\n", fdt_strerror(ret));
		ab_crumb(ST_DTB, false, "dtb %d", ret);
		return "dtb failed";
	}
	ab_crumb(ST_DTB, true, "dtb %u -> %u", (u32)dsize, fdt_totalsize(fdt));
	*rd = env_addr("loadaddr") + ps + ALIGN(ks, ps);
	*hp = h;
	return NULL;
}

static const char *load_slot(int slot, const void *prior, struct ab_bootimg_hdr **hp, ulong *rd,
			     ulong *rs, bool *missing)
{
	char path[40];

	snprintf(path, sizeof(path), "/slots/%c/boot/boot.img", 'a' + slot);
	return load_image(slot ? "slot b" : "slot a", path, NULL, prior, hp, rd, rs, missing);
}

static const char *load_rescue(const void *prior, struct ab_bootimg_hdr **hp, ulong *rd, ulong *rs)
{
	const char *file = CONFIG_AB_RESCUE_FILE, *why;
	bool missing;

	why = load_image("rescue", *file ? file : NULL, *file ? NULL : CONFIG_AB_RESCUE_PART, prior,
			 hp, rd, rs, &missing);
	printf("abboot: rescue kernel (%s): %s\n", *file ? file : CONFIG_AB_RESCUE_PART, why ?: "ok");
	ab_crumb(ST_RESCUE, !why, "rescue: %s", why ?: "ok");
	return why;
}

/*
 * No kernel at all: the screen stays, the watchdog fed, until someone powers the unit off. A reset
 * would make the same decision again from the same record, store and rescue partition: a loop.
 * When the screen is not shown (board_ab_menu_visible) nothing is drawn, but U-Boot stays all the
 * same.
 */
__weak void board_ab_last_resort(void)
{
}

static void __noreturn stay(const char *why)
{
	printf("abboot: no kernel can start (%s)\n", why);
	board_ab_last_resort();
	printf("abboot: staying here\n");
	ab_record_dump("no kernel");
	ab_screen_clear();
	ab_say("No slot can start");
	ab_say(CONFIG_AB_STAY_HINT);
	for (;;)
		udelay(10000);	/* schedule() inside: the watchdog stays fed */
}

/* --- The command line. --- */

/* Whether line has tok's key: a token starting "key=" if tok is key=value, else tok itself. */
static bool has_key(const char *line, const char *tok)
{
	const char *eq = strchr(tok, '='), *s = line;
	int k = eq ? eq - tok + 1 : strlen(tok);

	while (*s) {
		const char *t;
		int n;

		while (*s == ' ')
			s++;
		t = s;
		while (*s && *s != ' ')
			s++;
		n = s - t;
		if ((eq ? n >= k : n == k) && n && !strncmp(t, tok, k))
			return true;
	}
	return false;
}

static int append(char *out, int len, size_t size, const char *tok)
{
	int n = snprintf(out + len, size - len, "%s%s", len ? " " : "", tok);

	return n < 0 ? len : min_t(int, len + n, size - 1);
}

/*
 * The partition holding the image the prior stage booted, this U-Boot: CONFIG_AB_SELF_PART, or the
 * rescue partition for a test build that runs from there (CONFIG_AB_RESCUE_FILE set).
 */
static const char *booted_part(void)
{
	return *CONFIG_AB_RESCUE_FILE ? CONFIG_AB_RESCUE_PART : CONFIG_AB_SELF_PART;
}

/*
 * The prior tree's /chosen/bootargs, with the booted image's header command line in place of this
 * U-Boot image's (a bootloader that builds its line around the header command line of the image it
 * started put U-Boot's there); else the header's command line.
 */
__weak int board_ab_bootargs(const void *prior, const struct ab_bootimg_hdr *h, char *out,
			     size_t size)
{
	const char *args = NULL;
	int node, len;

	if (prior) {
		node = fdt_path_offset(prior, "/chosen");
		args = node < 0 ? NULL : fdt_getprop(prior, node, "bootargs", &len);
	}
	if (args) {
		strlcpy(out, ab_header_swap(args, h, booted_part()), size);
	} else {
		memcpy(out, h->cmdline, min(size - 1, sizeof(h->cmdline)));
		out[min(size - 1, sizeof(h->cmdline))] = '\0';
	}
	return 0;
}

__weak int board_ab_extra_args(char *out, int len, size_t size)
{
	return len;
}

/* The board's line, then init=, the slot's tokens and the markers. slot -1: the rescue system. */
static int make_bootargs(const void *prior, const struct ab_bootimg_hdr *h, int slot,
			 const char *kname, char *out, size_t size)
{
	char *tmp, *t, *save, tok[32];
	int len = 0, ret;

	tmp = malloc(size);
	if (!tmp)
		return -ENOMEM;
	ret = board_ab_bootargs(prior, h, tmp, size);
	if (ret) {
		free(tmp);
		return ret;
	}
	/* Every token as the board gave it, but init= if CONFIG_AB_INIT names one. */
	out[0] = '\0';
	save = tmp;
	while ((t = strsep(&save, " "))) {
		if (!*t || (*CONFIG_AB_INIT && !strncmp(t, "init=", 5)))
			continue;
		len = append(out, len, size, t);
	}
	free(tmp);
	if (*CONFIG_AB_INIT) {
		snprintf(tok, sizeof(tok), "init=%s", CONFIG_AB_INIT);
		len = append(out, len, size, tok);
	}
	/* The watchdog contract (SPEC.md): for a slot, not for the rescue system. */
	if (slot >= 0) {
		char sa[] = CONFIG_AB_SLOT_ARGS;

		save = sa;
		while ((t = strsep(&save, " ")))
			if (*t && !has_key(out, t))
				len = append(out, len, size, t);
		snprintf(tok, sizeof(tok), "ab.slot=%c", 'a' + slot);
		len = append(out, len, size, tok);
	}
	snprintf(tok, sizeof(tok), "ab.kernel=%s", kname);
	len = append(out, len, size, tok);
	len = board_ab_extra_args(out, len, size);
	if (len >= size - 2) {
		printf("abboot: command line too long\n");
		return -E2BIG;
	}
	return 0;
}

/* --- The rest of the weak hooks. --- */

__weak const void *board_ab_prior_fdt(void)
{
	const void *fdt = map_sysmem(env_get_hex("prevbl_fdt_addr", 0), 0);

	return fdt && !fdt_check_header(fdt) ? fdt : NULL;
}

__weak int board_ab_fixup_fdt(void *fdt, const void *prior)
{
	return 0;
}

__weak void board_ab_before_boot(void)
{
}

/* --- abboot --- */

static int fail(const char *why)
{
	printf("abboot: %s\n", why);
	ab_crumb(ST_FAIL, false, "%s", why);
	ab_record_dump("failure");
	ab_say("Could not start");
	ab_say("Restarting in 10 seconds");
	return CMD_RET_FAILURE;
}

static int do_abboot(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	ALLOC_CACHE_ALIGN_BUFFER(char, args, AB_CMDLINE_MAX);
	struct ab_bootimg_hdr *h = NULL;
	const void *pfdt;
	void *prior = NULL;
	ulong rd = 0, rs = 0;
	const char *why;
	char cmd[96], line[48], kname[8];
	int slot;

	ab_crumbs_init();	/* if the board's board_init has not already */
	ab_crumb(ST_BOOT, true, "abboot");

	/* The prior stage's tree, copied away before anything can land on it. */
	pfdt = board_ab_prior_fdt();
	if (pfdt) {
		prior = malloc(fdt_totalsize(pfdt));
		if (!prior)
			return fail("no memory for the prior tree");
		memcpy(prior, pfdt, fdt_totalsize(pfdt));
		printf("abboot: prior stage's device tree at %p, %u bytes\n", pfdt, fdt_totalsize(pfdt));
		ab_crumb(ST_PRIORFDT, true, "fdt %lx %u", (ulong)map_to_sysmem(pfdt), fdt_totalsize(pfdt));
	} else {
		printf("abboot: no device tree from the prior stage\n");
		ab_crumb(ST_PRIORFDT, false, "no prior fdt");
	}
	/* Asked once, here: whether anything is drawn, the menu shown, the notes waited for. */
	ab_screen_visible();

	slot = ab_choose();
	ab_screen_clear();
	if (slot >= 0) {
		bool missing;
		int o = !slot;

		snprintf(line, sizeof(line), "Booting slot %c", 'A' + slot);
		ab_say(line);
		/*
		 * The slot's own kernel. Failing that: with CONFIG_AB_BORROW_KERNEL (kernels that carry
		 * all they need) the other slot's kernel with this slot's root; without it (a kernel
		 * whose modules are in its own slot) the other slot whole, if it is bootable. Then the rescue kernel, then a screen that stays.
		 */
		why = load_slot(slot, prior, &h, &rd, &rs, &missing);
		if (!why) {
			snprintf(kname, sizeof(kname), "%c", 'a' + slot);
			ab_crumb(ST_SLOTK, true, "kernel of slot %c", 'a' + slot);
		} else {
			const char *why0 = why;
			int was = slot;

			printf("abboot: slot %c's kernel: %s\n", 'a' + slot, why);
			snprintf(line, sizeof(line), missing ? "Slot %c has no kernel of its own" :
				 "Slot %c's kernel cannot be used", 'A' + slot);
			ab_say(line);
			why = "not tried";
			if (IS_ENABLED(CONFIG_AB_BORROW_KERNEL) || ab_bootable(&ab.rec.slot[o]))
				why = load_slot(o, prior, &h, &rd, &rs, &missing);
			if (!why) {
				snprintf(kname, sizeof(kname), "%c", 'a' + o);
				if (IS_ENABLED(CONFIG_AB_BORROW_KERNEL)) {
					snprintf(line, sizeof(line), "Using slot %c's kernel", 'A' + o);
				} else {
					/* the other slot whole: its kernel and its root; this one's try taken */
					ab_take(&ab.rec, slot, -1);
					slot = o;
					snprintf(line, sizeof(line), "Booting slot %c instead", 'A' + o);
				}
				ab_say(line);
			} else {
				printf("abboot: slot %c's kernel: %s\n", 'a' + o, why);
				why = load_rescue(prior, &h, &rd, &rs);
				if (why)
					stay(why);
				strcpy(kname, "rescue");
				ab_say("Using the rescue kernel");
			}
			/* e.g. "a no kernel -> b": slot a's root (or slot b whole), slot b's kernel */
			ab_crumb(ST_SLOTK, false, "%c %.10s -> %s", 'a' + was, why0, kname);
			if (ab_screen_visible())
				ab_wait_ms(CONFIG_AB_WARN_MS);
		}
		ab_commit(slot);
	} else {
		/* No slot can start: the rescue kernel, no ab.slot=. */
		ab_crumb(ST_MENU, false, "menu: no slot");
		ab_say("No slot can start");
		why = load_rescue(prior, &h, &rd, &rs);
		if (why)
			stay(why);
		ab_say("Starting the rescue system");
		if (ab_screen_visible())
			ab_wait_ms(CONFIG_AB_WARN_MS);
		strcpy(kname, "rescue");
	}

	if (make_bootargs(prior, h, slot, kname, args, AB_CMDLINE_MAX))
		return fail("bootargs failed");
	printf("abboot: bootargs: %s\n", args);
	ab_crumb(ST_ARGS, true, "args %d", (int)strlen(args));

	/* booti writes $bootargs and the initrd into /chosen; the tree and ramdisk stay in place. */
	env_set("bootargs", args);
	env_set("fdt_high", "0xffffffffffffffff");
	env_set("initrd_high", "0xffffffffffffffff");
	snprintf(cmd, sizeof(cmd), "booti 0x%lx 0x%lx:0x%lx 0x%lx", env_addr("kernel_addr_r"), rd, rs,
		 env_addr("fdt_addr_r"));
	printf("abboot: %s\n", cmd);
	board_ab_before_boot();
	ab_crumb(ST_JUMP, true, "booti");
	ab_record_dump("before booti");
	run_command(cmd, 0);
	return fail("booti returned");
}

U_BOOT_CMD(abboot, 1, 0, do_abboot,
	   "boot the A/B slot the record picks (SPEC.md)", "");
