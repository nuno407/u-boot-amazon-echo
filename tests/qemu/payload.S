/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A stand-in "kernel" for the QEMU tests: an arm64 Image header (so booti takes it), then a line
 * on QEMU virt's PL011 UART at 0x09000000, "PAYLOAD <tag>", and PSCI SYSTEM_OFF, so QEMU exits.
 * mkbootimg.py writes the tag (16 bytes) into the binary, which tells the test whose kernel ran.
 */
	.text
	.globl	_start
_start:
	b	start				/* code0 */
	.long	0				/* code1 */
	.quad	0				/* text_offset */
	.quad	image_end - _start		/* image_size */
	.quad	0xa				/* flags: little-endian, 4 KB pages, anywhere */
	.quad	0, 0, 0				/* res2-res4 */
	.ascii	"ARM\x64"			/* magic */
	.long	0				/* res5 */

start:
	ldr	x1, =0x09000000
	adr	x2, msg
1:	ldrb	w3, [x2], #1
	cbz	w3, 3f
2:	ldr	w4, [x1, #0x18]			/* UARTFR */
	tbnz	w4, #5, 2b			/* TXFF */
	strb	w3, [x1]			/* UARTDR */
	b	1b
3:	ldr	x0, =0x84000008			/* PSCI SYSTEM_OFF */
	hvc	#0
	b	.
	.ltorg

msg:	.ascii	"\r\nPAYLOAD "
	.ascii	"@@@@@@@@@@@@@@@@"		/* the tag */
	.asciz	"\r\n"
	.balign	8
image_end:
