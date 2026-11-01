/*
 * A32 boot shim entry: arm64 Linux Image header so the MediaTek LK treats us
 * like a kernel. LK/ATF enters at **EL2** (cmdline bootopt=64S3,32N2,64N2: 64-bit
 * normal world at EL2), MMU/D-cache off, x0 = DTB pointer. The shim and XNU both
 * need EL1, so the first thing we do is drop from EL2 to EL1 (as Linux's el2_setup
 * does). The same macro is used for PSCI-started secondaries.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

/* Drop EL2 -> EL1h and continue at \cont; falls through to \cont when already at EL1. Clobbers x1, x2. */
.macro mlog n
#ifdef LOG
	movz	x9, #0x4d0f, lsl #16		/* reserved "minirdump" RAM (live FDT mblock-18): survives a warm reset */
	mov	w10, #\n
	str	w10, [x9, #32]
	mrs	x10, cntpct_el0
	str	x10, [x9, #(64 + 8 * \n)]
#endif
.endm

.macro drop_to_el1 cont
	mrs	x1, CurrentEL
	lsr	x1, x1, #2
	cmp	x1, #2
	b.ne	\cont
	mov	x1, #(1 << 31)		/* HCR_EL2.RW: EL1 is AArch64 */
	msr	hcr_el2, x1
	msr	vttbr_el2, xzr		/* no stage 2 */
	mov	x1, #0x33ff
	msr	cptr_el2, x1		/* no FP/SIMD traps (RES1 bits kept) */
	msr	mdcr_el2, xzr
	mrs	x1, midr_el1		/* EL1 must see the real MIDR/MPIDR */
	msr	vpidr_el2, x1
	mrs	x1, mpidr_el1
	msr	vmpidr_el2, x1
	mov	x1, #3			/* CNTHCTL_EL2: EL1 may use the physical counter/timer */
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	mrs	x1, icc_sre_el2		/* GICv3 system registers for EL1 */
	mov	x2, #9
	orr	x1, x1, x2		/* SRE | Enable */
	msr	icc_sre_el2, x1
	movz	x1, #0x0800		/* SCTLR_EL1 RES1 bits, MMU/caches off, little endian */
	movk	x1, #0x30d0, lsl #16
	msr	sctlr_el1, x1
	adr	x1, \cont
	msr	elr_el2, x1
	mov	x1, #0x3c5		/* SPSR_EL2: DAIF masked, EL1h */
	msr	spsr_el2, x1
	isb
	eret
.endm

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer from LK (x19 survives the eret) */
#ifdef LOG
	movz	x9, #0x4d0f, lsl #16
	ldr	w10, [x9]
	movz	w11, #0x4158			/* magic 0x32334158 "XA32" */
	movk	w11, #0x3233, lsl #16
	cmp	w10, w11
	b.eq	8f
	str	w11, [x9]
	str	wzr, [x9, #4]			/* boot counter */
8:	ldr	w10, [x9, #4]
	add	w10, w10, #1
	str	w10, [x9, #4]
	mrs	x10, currentel
	str	x10, [x9, #8]			/* EL we were entered at (value 8 = EL2) */
	str	x19, [x9, #16]			/* x0 from LK */
	mrs	x10, sctlr_el2
	str	x10, [x9, #24]
#endif
	mlog	1
	mlog	2
#if defined(ES) && ES != 5
	/* Bring-up diagnostics: stop the MediaTek watchdog first (TOPRGU 0x10007000 WDT_MODE = key 0x22<<24, EN=0). */
	movz	x1, #0x7000
	movk	x1, #0x1000, lsl #16
	movz	w2, #0x2200, lsl #16
	str	w2, [x1]
#endif
#if defined(ES) && ES == 1
9:	wfe
	b	9b
#endif
#if defined(ES) && (ES == 4 || ES == 5)
	/* Chain test: the stock Android kernel Image sits 2 MiB behind us in the same blob (text_offset 0x80000
	 * keeps it 2 MiB-aligned + 0x80000). Boot it exactly as LK would: x0 = DTB, x1-x3 = 0, entered at LK's EL. */
	adr	x1, _head
	mov	x2, #0x200000
	add	x1, x1, x2
	mov	x0, x19
	mov	x2, xzr
	mov	x3, xzr
	br	x1
#endif
	drop_to_el1 1f
1:	mlog	3
#if defined(ES) && ES == 2
9:	wfe
	b	9b
#endif
#if defined(ES) && ES == 6
	adr	x1, _head
	mov	x2, #0x200000
	add	x1, x1, x2
	mov	x0, x19
	mov	x2, xzr
	mov	x3, xzr
	br	x1
#endif
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mlog	4
#if defined(ES) && ES == 3
9:	wfe
	b	9b
#endif
#if defined(ES) && ES == 8
	/* wait WAITSEC seconds (13 MHz counter, WDT fed), then boot Android: does anything reset us at ~30 s? */
	movz	x4, #0x5d40
	movk	x4, #0x00c6, lsl #16
	mov	x6, #WAITSEC
	mul	x6, x4, x6
	mrs	x5, cntpct_el0
	add	x6, x6, x5
	movz	x7, #0x7000
	movk	x7, #0x1000, lsl #16
	mov	w8, #0x1971
5:	str	w8, [x7, #8]
	mrs	x5, cntpct_el0
	cmp	x5, x6
	b.lo	5b
	adr	x1, _head
	mov	x2, #0x200000
	add	x1, x1, x2
	mov	x0, x19
	mov	x2, xzr
	mov	x3, xzr
	br	x1
#endif
#if defined(ES) && ES == 7
	adr	x1, _head
	mov	x2, #0x200000
	add	x1, x1, x2
	mov	x0, x19
	mov	x2, xzr
	mov	x3, xzr
	br	x1
#endif
	mov	x0, x19
	bl	shim_main
#if defined(ES) && ES == 9
	mov	x0, x19
	b	chain_android
#endif
3:	wfe
	b	3b

/* Boot the Android kernel Image that sits 2 MiB behind us (x0 = DTB). */
	.globl chain_android
chain_android:
	mov	x19, x0
	adr	x1, _head
	mov	x2, #0x200000
	add	x1, x1, x2
	mov	x0, x19
	mov	x2, xzr
	mov	x3, xzr
	br	x1

/* PSCI CPU_ON target. x0 = context id = cpu index (1..7). */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mov	x20, x0			/* context id = cpu index */
	drop_to_el1 1f
1:	mov	x0, x20
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	secondary_main
4:	wfe
	b	4b

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls
 * exc_handler(slot, regs). IRQs return; everything else ends in the
 * on-screen panic.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret
