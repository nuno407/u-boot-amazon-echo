/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boot layer's diagnostics (CONFIG_AB_CRUMBS): one 32-byte "crumb" per boot stage in misc
 * (CONFIG_AB_CRUMBS_BLOCK, two blocks), and the console record (CONFIG_CONSOLE_RECORD) in
 * CONFIG_AB_LOG_BLOCKS blocks from CONFIG_AB_LOG_FIRST, readable from Linux after a reset
 * (SPEC.md, "Diagnostics"). Each area is written only while it is empty or already
 * ours. Without CONFIG_AB_CRUMBS all of it compiles to nothing but the printf.
 *
 * The stage numbers are part of the on-disk format (SPEC.md lists them); some are unused now.
 */
#ifndef __ABBOOT_CRUMBS_H
#define __ABBOOT_CRUMBS_H

#include <linux/types.h>

struct pt_regs;

enum ab_stage {
	ST_EARLY,	/* board_early_init_f: before relocation, MMU off (RAM record only) */
	ST_INIT,	/* board_init: relocated, MMU on; misc reachable if this slot exists */
	ST_MISCCMD,	/* what the misc command held when U-Boot looked (test slot) */
	ST_MISCCLR,	/* what U-Boot did about it */
	ST_LATE,	/* board_late_init */
	ST_VIDPROBE,	/* about to probe the video device */
	ST_VIDDEV,	/* ... its result */
	ST_CONPROBE,	/* about to probe its console */
	ST_VIDEO,	/* ... the result: size and rotation, or the error */
	ST_DRAW,	/* about to draw the first text */
	ST_TEXT,	/* text drawn */
	ST_LOGW,	/* about to write the console record to misc (why) */
	ST_LOGD,	/* ... written */
	ST_BOOT,	/* abboot started */
	ST_PRIORFDT,	/* the prior stage's device tree found and copied */
	ST_HDR,		/* (unused) */
	ST_KERNEL,	/* kernel read and decompressed */
	ST_ARGS,	/* command line made */
	ST_JUMP,	/* about to run booti */
	ST_FAIL,	/* abboot failed: why */
	ST_EXC_ESR,	/* an exception: ESR (board_exception_hook) */
	ST_EXC_ELR,	/* ... ELR, the faulting PC */
	ST_EXC_PC,	/* ... ELR minus the relocation offset: the address in u-boot.map */
	ST_EXC_FAR,	/* ... FAR, the faulting address */
	ST_PANIC,	/* panic(): its message (board_panic_hook) */
	ST_MENU,	/* the boot menu's result: how (timeout, button, idle) and which entry */
	ST_AB,		/* A/B: where the record came from and the pick */
	ST_ABW,		/* A/B: the record written to misc, or why not */
	ST_SLOTK,	/* whose kernel the slot boots, or why another's */
	ST_DTB,		/* the slot's device tree with the board's additions, or the error */
	ST_STICKY,	/* test slot: boot-recovery set again before booti */
	ST_RESCUE,	/* the rescue kernel loaded, or why not */
	ST_COUNT
};

#if IS_ENABLED(CONFIG_AB_CRUMBS)
void ab_crumb(enum ab_stage st, bool ok, const char *fmt, ...)
	__attribute__ ((format (__printf__, 3, 4)));
/* Find misc and its areas, once relocated: from board_init (CONFIG_AB_BOARD_INIT), else abboot. */
void ab_crumbs_init(void);
/* The console record to misc now, with why in a crumb. */
void ab_record_dump(const char *why);
/* For board_exception_hook / board_panic_hook. */
void ab_crumbs_exception(struct pt_regs *regs);
void ab_crumbs_panic(const char *msg);
#else
static inline void ab_crumb(enum ab_stage st, bool ok, const char *fmt, ...) {}
static inline void ab_crumbs_init(void) {}
static inline void ab_record_dump(const char *why) {}
static inline void ab_crumbs_exception(struct pt_regs *regs) {}
static inline void ab_crumbs_panic(const char *msg) {}
#endif

#endif
