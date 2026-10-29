/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boot layer's A/B record: AOSP's struct bootloader_control, 32 bytes at misc byte 2048
 * (SPEC.md, "The A/B record"). This small code reads and writes it instead of U-Boot's
 * android_ab.c. No U-Boot dependency but crc32(), so the same file builds on the host
 * (tests/ab-test.c, with -DAB_HOST).
 *
 * Slot index 0 is slot a ("_a"), 1 is slot b ("_b").
 */
#ifndef __ABBOOT_AB_H
#define __ABBOOT_AB_H

#ifdef AB_HOST
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#else
#include <linux/types.h>
#endif

#define AB_OFFSET	2048		/* misc byte offset: offsetof(bootloader_message_ab, slot_suffix) */
#define AB_SIZE		32
#define AB_AREA		2048		/* 2048-4095: the record, and nothing else ever */
#define AB_MAGIC	0x42414342	/* "BCAB" little-endian, BOOT_CTRL_MAGIC */
#define AB_VERSION	1
#define AB_PRIO_MAX	15
#define AB_TRIES_MAX	7		/* 3 bits */
#define AB_TRIES_NEW	3		/* a slot set active, and the default record */

struct ab_slot {
	u8 priority;		/* 0-15, 0: not bootable */
	u8 tries;		/* 0-7 */
	bool successful;
	bool verity;		/* verity_corrupted: never set by the boot layer, honored */
};

struct ab_rec {
	int last;		/* slot_suffix: the slot booted last, 0 or 1 (-1 if unset) */
	struct ab_slot slot[2];
};

enum ab_err { AB_OK = 0, AB_EMAGIC = -1, AB_ECRC = -2, AB_EVERSION = -3 };

/* Decode raw (32 bytes); returns AB_OK or why not. */
int ab_decode(const u8 *raw, struct ab_rec *r);
/*
 * Encode r into raw (32 bytes), in place: slot_suffix, magic, version, nb_slot, slot_info[0..1]
 * priority/tries/successful and crc32_le are written, reserved1 (bytes 20-27) is zeroed as AOSP
 * leaves it, and everything else (recovery tries, merge status, verity bits, slot_info[2..3]) is kept as it is.
 * Pass zeros for a fresh record.
 */
void ab_encode(const struct ab_rec *r, u8 *raw);

/* The default record, written where the area is empty: a priority 15, b 14, both AB_TRIES_NEW. */
void ab_default(struct ab_rec *r);

bool ab_bootable(const struct ab_slot *s);
/* "good" (successful), "trial" (tries left), "bad" (neither, or priority 0) */
const char *ab_word(const struct ab_slot *s);

/*
 * The pick: the slot of highest priority (a tie goes to a) if it is bootable; if it is not (its
 * tries ran out without a successful boot, or it is verity-corrupted), it has failed: *failed is
 * set to it and the other slot is picked if bootable. Returns the slot, or -1 if none can boot.
 * r is not changed.
 */
int ab_select(const struct ab_rec *r, int *failed);

/*
 * After the choice (the pick, or a slot chosen in the menu, which must be bootable): a failed slot
 * goes to priority 0 (so the fallback is announced once), the chosen slot loses one try unless it
 * has booted successfully, and becomes slot_suffix. Priorities are otherwise untouched: a manual
 * choice is for this boot only.
 */
void ab_take(struct ab_rec *r, int chosen, int failed);

#endif
