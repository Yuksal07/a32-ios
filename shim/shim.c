/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "mmc.h"

/* MediaTek TOPRGU watchdog (live FDT toprgu@10007000, VERIFIED): software reset
 * fallback when PSCI SYSTEM_RESET returns. SWRST offset 0x14 / key 0x1209 are the
 * standard mtk_wdt values [UNVERIFIED on this unit]. */
#define MTK_WDT_BASE		0x10007000UL
#define MTK_WDT_SWRST		(MTK_WDT_BASE + 0x14)
#define MTK_WDT_SWRST_KEY	0x1209U

/* GICv3 (GIC-500) [live FDT /interrupt-controller, VERIFIED]; must match tools/mkadt.py */
#define GICD_BASE         0x0c000000UL
#define GICR_BASE         0x0c040000UL
#define GICR_SIZE         0x200000UL
#define GICR_PE_SIZE      0x20000UL
#define UART_BASE         0x11002000UL	/* [iomem] serial @ 11002000 (unused by the shim) */
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
/* live FDT /timer interrupts = PPI 13,14,11,10 (secure-phys, phys, virt, hyp) */
#define VTIMER_INTID		27	/* PPI 11, VERIFIED (/proc/interrupts: GICv3 27 arch_timer) */
#define PTIMER_INTID		30	/* PPI 14, VERIFIED */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

#ifdef LOG
#define MLOG ((volatile uint64_t *)0x4d0f0000UL)
#endif

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	uint64_t step = rsr("cntfrq_el0") / 20;		/* 50 ms */
	uint64_t nk = now();
	while (now() < end) {
		if (now() >= nk) {			/* keep feeding the MediaTek watchdog (TOPRGU WDT_RESTART) */
			wr32(MTK_WDT_BASE + 0x08, 0x1971);
			nk = now() + step;
		}
	}
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(MTK_WDT_SWRST, MTK_WDT_SWRST_KEY);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

#ifdef DIAG
static volatile int dstage;
#endif

#if defined(ES) && ES == 9
void chain_android(uint64_t dtb);
#endif
#ifdef CHAT
static void __attribute__((noreturn)) chain_android(uint64_t dtb);
#endif

void exc_handler(uint64_t slot, uint64_t *regs)
{
#if defined(ES) && ES == 9
	/* Probe: wait 3 s per ESR exception-class unit (WDT fed, plain loads only), then boot Android. */
	uint64_t esr, t, e;
	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
	e = t + 13000000ULL * 3 * (esr >> 26);
	do {
		*(volatile uint32_t *)(MTK_WDT_BASE + 0x08) = 0x1971;
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
	} while (t < e);
	chain_android(regs[19]);
#endif
#ifdef LOG
	MLOG[8] = slot; MLOG[9] = rsr("esr_el1"); MLOG[10] = rsr("elr_el1"); MLOG[11] = rsr("far_el1");
	MLOG[12] = 1;			/* exception seen */
#endif
#ifdef DIAG
	/* No screen access here: just wait dstage*15 s, then reset. The reboot delay tells the stage. */
	delay_ms(dstage * 15000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(MTK_WDT_SWRST, MTK_WDT_SWRST_KEY);
	for (;;)
		__asm__ volatile("wfe");
#endif
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
#if defined(CHAT) && CHAT >= 4
	delay_ms(6000);			/* panic text is in the framebuffer: let Android's first frame show it */
	chain_android(regs[19]);
#endif
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	uint64_t f = rsr("cntfrq_el0");	/* MT67xx is 13 MHz, read it instead of assuming */
	return f ? (uint32_t)(ticks * 1000000ULL / f) : 0;
}

#ifdef CHAT
extern char _head[];
/* bring-up bisect: boot the Android kernel that sits 2 MiB behind us, exactly as ES=4..8 do in start.S */
static void __attribute__((noreturn)) chain_android(uint64_t dtb)
{
	void (*f)(uint64_t, uint64_t, uint64_t, uint64_t) = (void (*)(uint64_t, uint64_t, uint64_t, uint64_t))(_head + 0x200000);
	f(dtb, 0, 0, 0);
	for (;;)
		;
}
#endif

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;
#if defined(CHAT) && CHAT == 1
	chain_android(dtb);			/* C entry only */
#elif defined(CHAT) && CHAT == 2
	wr32(MTK_WDT_BASE + 0x00, 0x22000000);
	delay_ms(8000);			/* timer + counter + WDT feeding in C */
	chain_android(dtb);
#elif defined(CHAT) && CHAT == 7
	wr32(MTK_WDT_BASE + 0x00, 0x22000000);
	con_clear(BLUE);
	con_str("SHIM", "chat 7");
	mmc_selftest();
	for (int i = 0; i < 40; i++) {
		delay_ms(1000);
		con_kick();
	}
	chain_android(dtb);
#elif defined(CHAT) && CHAT >= 4
	wr32(MTK_WDT_BASE + 0x00, 0x22000000);
	stage = "loader";
	con_clear(RED);
	con_str("SHIM", "chat 4+: results below");
	const char *why4 = loader_boot(dtb);
	con_str("LOADER", why4);
	con_skip();
#if CHAT >= 5
	stage = "storage";
	test_mmc();
	con_skip();
#endif
	con_str("DONE", "chaining to android");
#if CHAT >= 6
	for (int i = 0; i < 40; i++) {		/* hold one stable frame ~40 s, re-kicking the panel */
		delay_ms(1000);
		con_kick();
	}
#else
	delay_ms(6000);
#endif
	chain_android(dtb);
#elif defined(CHAT) && CHAT == 3
	wr32(MTK_WDT_BASE + 0x00, 0x22000000);
	con_clear(RED);			/* first framebuffer/console use */
	con_str("SHIM", "chat 3");
	delay_ms(8000);
	chain_android(dtb);
#endif
#if defined(ES) && ES == 9
	wr32(MTK_WDT_BASE + 0x08, 0x1971);
	delay_ms(5000);			/* C code + delay_ms ran if Android follows within ~5 s */
	return;
#endif
#ifdef LOG
	MLOG[4] = 5;			/* reached shim_main */
	wr32(MTK_WDT_BASE + 0x00, 0x22000000);
	MLOG[5] = rd32(MTK_WDT_BASE + 0x00);	/* WDT_MODE read back */
	for (uint64_t i = 0;; i++) {		/* heartbeat: seconds alive survive a reset */
		MLOG[6] = i;
		MLOG[7] = now();
		delay_ms(1000);
	}
#endif

#if defined(DIAG_A)
	for (;;)
		__asm__ volatile("wfe");	/* touches nothing */
#elif defined(DIAG_B)
	wr32(MTK_WDT_BASE + 0x00, 0x22000000);
	for (;;)
		__asm__ volatile("wfe");	/* WDT disable only */
#elif defined(DIAG_C)
	for (;;) {			/* kick the watchdog every second, never disable it */
		wr32(MTK_WDT_BASE + 0x08, 0x1971);
		delay_ms(1000);
	}
#endif
	/* LK leaves the MediaTek watchdog armed: stop it first (TOPRGU WDT_MODE, key 0x22 in the top byte, EN=0). */
	wr32(MTK_WDT_BASE + 0x00, 0x22000000);

#ifdef DIAG
	/* Staged bring-up check. A fault resets the phone after dstage*15 s (see exc_handler); no fault = stays on. */
	dstage = 1;			/* the WDT write above */
	delay_ms(3000);
	dstage = 2;			/* single aligned word into the "fb_base_l" buffer 0x7e605000 */
	*(volatile uint32_t *)0x7e605000UL = 0xffff0000;
	delay_ms(3000);
	dstage = 3;			/* single word at the reserved-memory framebuffer base 0x7c720000 */
	*(volatile uint32_t *)0x7c720000UL = 0xffff0000;
	delay_ms(3000);
	dstage = 4;			/* display mutex write */
	*(volatile uint32_t *)(0x14001000UL + 0x20) = 1;
	delay_ms(3000);
	dstage = 5;			/* DSI start write */
	*(volatile uint32_t *)(0x14014000UL + 0x00) = 0;
	*(volatile uint32_t *)(0x14014000UL + 0x00) = 1;
	dstage = 9;
	for (;;)
		delay_ms(1000);
#endif
	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
	con_clear(RED);
	con_str("SHIM", "a32 milestone 4: xnu loader");
	con_skip();
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();

	/* No payload: the A32 has eMMC, there is no UFS to probe (see mmc.c). */
	stage = "storage";
	test_mmc();
	con_skip();
	/* Keep the results on screen (later tests clear it); reset after 3 min. */
	con_str("STORAGE", "probe done, reset in ~3 min");
	delay_ms(180000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(RED);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(RED);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}
