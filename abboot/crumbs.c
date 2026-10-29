// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The boot layer's diagnostics: crumbs and the console record in misc (crumbs.h,
 * SPEC.md "Diagnostics").
 *
 * misc, CONFIG_AB_CRUMBS_BLOCK (two 512-byte blocks): ST_COUNT slots of 32 bytes, "UBC1", the stage,
 * '+' or '!', the arch counter in ms (counting since the SoC came out of reset), 20 bytes of text.
 * A fresh record each boot. Written only if the blocks are empty or already hold "UBC1".
 *
 * misc, CONFIG_AB_LOG_FIRST, CONFIG_AB_LOG_BLOCKS blocks: the newest output U-Boot printed (with
 * CONFIG_CONSOLE_RECORD), behind a 16-byte header "UBLOG01\0", u32 length, u32 1 if it overflowed.
 * Written only if the area is empty or already ours.
 *
 * RAM, CONFIG_AB_CRUMBS_RAM (0: none): "UBRAM01", the counter at each stage, from before
 * relocation on; the early stage is carried from there into misc.
 */
#include <blk.h>
#include <cpu_func.h>
#include <malloc.h>
#include <membuf.h>
#include <memalign.h>
#include <part.h>
#include <stdio.h>
#include <vsprintf.h>
#include <div64.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/ptrace.h>
#include <asm/unaligned.h>
#include <linux/string.h>
#include "abboot.h"
#include "crumbs.h"

DECLARE_GLOBAL_DATA_PTR;

#define CRUMB_BLOCKS	2		/* 32 slots */
#define CRUMB_MAGIC	"UBC1"
/*
 * The generic timer's ticks per ms: CONFIG_COUNTER_FREQUENCY, or, on a board that leaves it 0 (QEMU),
 * CNTFRQ_EL0 as the firmware set it.
 */
#if CONFIG_COUNTER_FREQUENCY
#define TICKS_PER_MS	(CONFIG_COUNTER_FREQUENCY / 1000)
#else
static u64 cntfrq_per_ms(void)
{
	u64 f;

	asm volatile("mrs %0, cntfrq_el0" : "=r" (f));
	return f >= 1000 ? f / 1000 : 1;
}
#define TICKS_PER_MS	cntfrq_per_ms()
#endif

struct crumb {
	char magic[4];
	u8 stage;
	char status;	/* '+' ok, '!' failed */
	u8 pad[2];
	u32 ms;
	char text[20];
} __packed;

struct crumb_ram {
	char magic[8];		/* "UBRAM01" */
	u32 last, last_status;
	u64 ticks[ST_COUNT];
	char text[ST_COUNT][20];
};

static struct crumb misc_crumbs[CRUMB_BLOCKS * 16] __aligned(ARCH_DMA_MINALIGN);
static struct blk_desc *crumb_desc;
static lbaint_t crumb_lba, log_lba;

void ab_record_dump(const char *why)
{
	static char chunk[4096] __aligned(4096);	/* written 8 blocks at a time */
	char *data = NULL;
	int len, max = CONFIG_AB_LOG_BLOCKS * 512 - 16, off, n, ok = 1;

	if (!IS_ENABLED(CONFIG_CONSOLE_RECORD) || !log_lba)
		return;
	ab_crumb(ST_LOGW, true, "log: %s", why);
	printf("abboot: console record to misc (%s)\n", why);
#if CONFIG_IS_ENABLED(CONSOLE_RECORD)
	len = membuf_getraw((struct membuf *)&gd->console_out, -1, false, &data);
#else
	len = 0;
#endif
	if (len < 0 || !data)
		len = 0;
	if (len > max) {
		data += len - max;
		len = max;
	}
	for (off = 0; off < CONFIG_AB_LOG_BLOCKS * 512; off += sizeof(chunk)) {
		int t0 = off < 16 ? 16 : off;	/* first text byte in this chunk, as an area offset */

		memset(chunk, 0, sizeof(chunk));
		if (!off) {
			memcpy(chunk, "UBLOG01", 8);
			put_unaligned_le32(len, chunk + 8);
			put_unaligned_le32(!!(gd->flags & GD_FLG_RECORD_OVF), chunk + 12);
		}
		n = min_t(int, off + (int)sizeof(chunk), 16 + len) - t0;
		if (n > 0)
			memcpy(chunk + (t0 - off), data + (t0 - 16), n);
		if (blk_dwrite(crumb_desc, log_lba + off / 512, sizeof(chunk) / 512, chunk) !=
		    sizeof(chunk) / 512) {
			ok = 0;
			break;
		}
	}
	ab_crumb(ST_LOGD, ok, ok ? "log %d bytes" : "log write failed", len);
}

void ab_crumb(enum ab_stage st, bool ok, const char *fmt, ...)
{
	struct crumb_ram *r = (struct crumb_ram *)CONFIG_AB_CRUMBS_RAM;
	struct crumb *c;
	char text[20];
	va_list args;
	u64 t;

	asm volatile("isb; mrs %0, cntpct_el0" : "=r" (t));
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);

	/* RAM first: it works before relocation and without the eMMC. */
	if (CONFIG_AB_CRUMBS_RAM) {
		if (st == ST_EARLY || memcmp(r->magic, "UBRAM01", 8)) {
			memset(r, 0, sizeof(*r));
			memcpy(r->magic, "UBRAM01", 8);
		}
		r->ticks[st] = t;
		r->last = st;
		r->last_status = ok ? '+' : '!';
		memcpy(r->text[st], text, sizeof(text));
	}
	printf("abboot: crumb %d %c %s\n", st, ok ? '+' : '!', text);
	/* Before relocation the MMU is off (no flush needed) and .bss is not ours yet: stop here. */
	if (!(gd->flags & GD_FLG_RELOC))
		return;
	if (CONFIG_AB_CRUMBS_RAM)
		flush_dcache_range(CONFIG_AB_CRUMBS_RAM,
				   CONFIG_AB_CRUMBS_RAM + ALIGN(sizeof(*r), ARCH_DMA_MINALIGN));

	c = &misc_crumbs[st];
	memcpy(c->magic, CRUMB_MAGIC, 4);
	c->stage = st;
	c->status = ok ? '+' : '!';
	c->ms = lldiv(t, TICKS_PER_MS);
	memcpy(c->text, text, sizeof(text));
	if (crumb_desc && blk_dwrite(crumb_desc, crumb_lba, CRUMB_BLOCKS, misc_crumbs) != CRUMB_BLOCKS)
		printf("abboot: crumb write failed\n");
}

static bool area_free(const u8 *b, int n, const char *magic)
{
	int i;

	for (i = 0; i < n && !b[i]; i++)
		;
	return i == n || !memcmp(b, magic, strlen(magic));
}

void ab_crumbs_init(void)
{
	struct crumb_ram *r = (struct crumb_ram *)CONFIG_AB_CRUMBS_RAM;
	struct disk_partition info;
	struct blk_desc *desc;

	/* A fresh record each boot, with the early stage carried over from RAM. */
	memset(misc_crumbs, 0, sizeof(misc_crumbs));
	if (CONFIG_AB_CRUMBS_RAM && !memcmp(r->magic, "UBRAM01", 8) && r->ticks[ST_EARLY]) {
		memcpy(misc_crumbs[ST_EARLY].magic, CRUMB_MAGIC, 4);
		misc_crumbs[ST_EARLY].stage = ST_EARLY;
		misc_crumbs[ST_EARLY].status = '+';
		misc_crumbs[ST_EARLY].ms = lldiv(r->ticks[ST_EARLY], TICKS_PER_MS);
		memcpy(misc_crumbs[ST_EARLY].text, r->text[ST_EARLY], 20);
	}
	if (!ab_find_part(CONFIG_AB_MISC_PART, &desc, &info) &&
	    info.size >= CONFIG_AB_CRUMBS_BLOCK + CRUMB_BLOCKS) {
		ALLOC_CACHE_ALIGN_BUFFER(u8, blk, CRUMB_BLOCKS * 512);

		if (blk_dread(desc, info.start + CONFIG_AB_CRUMBS_BLOCK, CRUMB_BLOCKS, blk) == CRUMB_BLOCKS) {
			if (area_free(blk, CRUMB_BLOCKS * 512, CRUMB_MAGIC)) {
				crumb_desc = desc;
				crumb_lba = info.start + CONFIG_AB_CRUMBS_BLOCK;
			} else {
				printf("abboot: misc block %d is not empty, no crumbs there\n",
				       CONFIG_AB_CRUMBS_BLOCK);
			}
		}
	}
	if (crumb_desc && IS_ENABLED(CONFIG_CONSOLE_RECORD) &&
	    info.size >= CONFIG_AB_LOG_FIRST + CONFIG_AB_LOG_BLOCKS) {
		u8 *lb = memalign(ARCH_DMA_MINALIGN, CONFIG_AB_LOG_BLOCKS * 512);

		if (lb && blk_dread(desc, info.start + CONFIG_AB_LOG_FIRST, CONFIG_AB_LOG_BLOCKS, lb) ==
		    CONFIG_AB_LOG_BLOCKS) {
			if (area_free(lb, CONFIG_AB_LOG_BLOCKS * 512, "UBLOG01"))
				log_lba = info.start + CONFIG_AB_LOG_FIRST;
			else
				printf("abboot: misc blocks %d.. not empty, no console record there\n",
				       CONFIG_AB_LOG_FIRST);
		}
		free(lb);
	}
	ab_crumb(ST_INIT, !!crumb_desc, crumb_desc ? "init, misc ok" : "init, no misc");
}

void ab_crumbs_exception(struct pt_regs *regs)
{
	static bool done;
	ulong far, el;

	if (done)
		return;
	done = true;
	asm volatile("mrs %0, CurrentEL" : "=r" (el));
	if ((el >> 2) == 2)
		asm volatile("mrs %0, far_el2" : "=r" (far));
	else
		asm volatile("mrs %0, far_el1" : "=r" (far));
	ab_crumb(ST_EXC_ESR, false, "esr %08lx", regs->esr);
	ab_crumb(ST_EXC_ELR, false, "elr %lx", regs->elr);
	ab_crumb(ST_EXC_PC, false, "pc %lx", regs->elr - gd->reloc_off);
	ab_crumb(ST_EXC_FAR, false, "far %lx", far);
	if (gd->flags & GD_FLG_RELOC)
		ab_record_dump("exception");
}

void ab_crumbs_panic(const char *msg)
{
	static bool done;

	if (done)
		return;
	done = true;
	ab_crumb(ST_PANIC, false, "%s", msg);
	if (gd->flags & GD_FLG_RELOC)
		ab_record_dump("panic");
}
