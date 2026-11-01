/*
 * Storage probe for the Samsung Galaxy A32 4G (MT6769V/CT): the internal eMMC on
 * MSDC0 (live FDT /msdc@11230000: reg 0x11230000, SPI 100, 8-bit, HS400).
 * Replaces the Qualcomm UFS probe of the original port.
 *
 * Runs with the MMU off, in PIO mode, and is STRICTLY READ-ONLY: the only data
 * command ever issued is CMD17 (READ_SINGLE_BLOCK); there is no write path here.
 * LK (the bootloader that booted from this very eMMC) leaves MSDC0 clocked and the
 * card in transfer state, so we reuse that state instead of re-initialising.
 *
 * Register layout is the MediaTek MSDC block as used by Linux drivers/mmc/host/mtk-sd.c.
 * !! It was written from that layout without access to the hardware: every line of
 * !! the on-screen output is meant to be photographed so the assumptions can be
 * !! confirmed or corrected. UNTESTED.
 */
#include <stdint.h>
#include "console.h"
#include "mmc.h"

#define MSDC0_BASE	0x11230000UL	/* live FDT msdc@11230000, VERIFIED */

#define MSDC_CFG	0x00
#define MSDC_IOCON	0x04
#define MSDC_PS		0x08
#define MSDC_INT	0x0c
#define MSDC_FIFOCS	0x14
#define MSDC_RXDATA	0x1c
#define SDC_CFG		0x30
#define SDC_CMD		0x34
#define SDC_ARG		0x38
#define SDC_STS		0x3c
#define SDC_RESP0	0x40
#define SDC_BLK_NUM	0x50

#define CFG_PIO		(1u << 3)
#define CFG_CKPDN	(1u << 1)
#define FIFOCS_CLR	(1u << 31)
#define STS_SDCBUSY	(1u << 0)
#define STS_CMDBUSY	(1u << 1)
#define INT_CMDRDY	(1u << 8)
#define INT_CMDTMO	(1u << 9)
#define INT_RSPCRCERR	(1u << 10)
#define INT_XFER_COMPL	(1u << 12)
#define INT_DATTMO	(1u << 14)
#define INT_DATCRCERR	(1u << 15)

#define RSP_R1		1u
#define DTYPE_SINGLE	1u

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(MSDC0_BASE + off); }
static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(MSDC0_BASE + off) = v; }

static inline uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static uint64_t freq(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f ? f : 13000000;
}

static uint32_t last_int;

/* Wait until (MSDC_INT & mask) != 0 or ~1 s; returns the bits seen (0 = timeout). Clears them. */
static uint32_t wait_int(uint32_t mask)
{
	uint64_t end = ticks() + freq();
	while (ticks() < end) {
		uint32_t v = rd(MSDC_INT);
		if (v & mask) {
			wr(MSDC_INT, v & mask);	/* write-1-to-clear, only what we waited for (XFER_COMPL may arrive with CMDRDY) */
			last_int = v;
			return v & mask;
		}
	}
	last_int = rd(MSDC_INT);
	return 0;
}

static int wait_idle(void)
{
	uint64_t end = ticks() + freq();
	while (ticks() < end)
		if (!(rd(SDC_STS) & (STS_SDCBUSY | STS_CMDBUSY)))
			return 0;
	return 1;
}

/* Returns 0 on success, 1 busy timeout, 2 cmd timeout, 3 crc error, 4 data error. */
static int send_cmd(uint32_t op, uint32_t arg, uint32_t rsp, int data_read)
{
	if (wait_idle())
		return 1;
	wr(MSDC_INT, rd(MSDC_INT));
	uint32_t raw = op | (rsp << 7);
	if (data_read) {
		wr(SDC_BLK_NUM, 1);
		raw |= (DTYPE_SINGLE << 11) | (512u << 16);	/* read: RW bit stays 0 */
	}
	wr(SDC_ARG, arg);
	wr(SDC_CMD, raw);
	uint32_t m = wait_int(INT_CMDRDY | INT_CMDTMO | INT_RSPCRCERR);
	if (!m || (m & INT_CMDTMO))
		return 2;
	if (m & INT_RSPCRCERR)
		return 3;
	return 0;
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static int same_n(const uint8_t *a, const char *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i] != (uint8_t)b[i])
			return 0;
	return 1;
}

static uint8_t sector[512] __attribute__((aligned(16)));

/* Read-only: CMD17 with PIO. LBA addressing (eMMC > 2 GB). 0 = ok. */
static int read_sector(uint32_t lba, uint8_t *out)
{
	uint32_t cfg = rd(MSDC_CFG);
	wr(MSDC_CFG, cfg | CFG_PIO);
	wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | FIFOCS_CLR);
	for (int i = 0; i < 100000 && (rd(MSDC_FIFOCS) & FIFOCS_CLR); i++)
		;
	int rc = send_cmd(17, lba, RSP_R1, 1);
	if (rc)
		return rc;
	uint32_t *w = (uint32_t *)out;
	uint64_t end = ticks() + freq();
	for (int n = 0; n < 128;) {
		if ((rd(MSDC_FIFOCS) & 0xff) >= 4) {
			w[n++] = rd(MSDC_RXDATA);
			continue;
		}
		if (rd(MSDC_INT) & (INT_DATTMO | INT_DATCRCERR))
			return 4;
		if (ticks() > end)
			return 4;
	}
	/* The data is already in our buffer: give XFER_COMPL ~5 ms to show, never the 1 s wait_int timeout. */
	uint64_t e2 = ticks() + freq() / 200;
	while (ticks() < e2 && !(rd(MSDC_INT) & INT_XFER_COMPL))
		;
	last_int = rd(MSDC_INT);
	wr(MSDC_INT, last_int & INT_XFER_COMPL);
	return 0;
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static const char *fs_type(uint32_t lba)
{
	/* superblock at byte 1024 = sector lba+2 for both f2fs and ext4 */
	if (read_sector(lba + 2, sector))
		return "read error";
	if (sector[0] == 0x10 && sector[1] == 0x20 && sector[2] == 0xf5 && sector[3] == 0xf2)
		return "f2fs";
	/* ext4 magic is at sb+56, i.e. sector offset 56 of the same sector */
	if (sector[56] == 0x53 && sector[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (512-byte LBAs). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ent[512] __attribute__((aligned(16)));
	for (uint32_t s = 0; s * 4 < nent; s++) {
		if (read_sector(entry_lba + s, ent)) {
			con_str("PART", "read error");
			return;
		}
		for (uint32_t k = 0; k < 4 && s * 4 + k < nent; k++) {
			const uint8_t *e = ent + k * 128;
			uint64_t first = le64(e + 32), last = le64(e + 40);
			if (!first)
				continue;
			char name[17];
			for (int c = 0; c < 16; c++) {
				uint8_t ch = e[56 + 2 * c];
				name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
			}
			name[16] = 0;
			if (((last - first + 1) >> 11) < 64)
				continue;
			con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 11));
			if (same(name, "userdata") || same(name, "cache"))
				con_str(" fs", fs_type((uint32_t)first));
		}
	}
}

void test_mmc(void)
{
	con_str("EMMC", "MSDC0 probe (ro)");
	uint32_t cfg = rd(MSDC_CFG);
	con_hex2("MSDC", "cfg=", cfg, "iocon=", rd(MSDC_IOCON));
	con_hex2("MSDC", "ps=", rd(MSDC_PS), "int=", rd(MSDC_INT));
	con_hex2("SDC", "cfg=", rd(SDC_CFG), "sts=", rd(SDC_STS));
	if (cfg == 0xffffffffu || cfg == 0) {
		con_str("EMMC", "registers read 0/ffffffff: block off or wrong base");
		return;
	}
	if (cfg & CFG_CKPDN) {
		con_str("EMMC", "clock gated by LK (CKPDN=1): stop");
		return;
	}

	/* CMD13 SEND_STATUS with RCA=1 (what Linux/LK assign to the eMMC): expect 'tran' (state 4). */
	int rc = send_cmd(13, 1u << 16, RSP_R1, 0);
	con_hex2("CMD13", "rc=", rc, "resp=", rc ? last_int : rd(SDC_RESP0));
	if (!rc)
		con_hex2("CARD", "state=", (rd(SDC_RESP0) >> 9) & 0xf, "(4=tran)", 0);

	rc = read_sector(1, sector);
	con_hex2("GPT hdr", "rc=", rc, "int=", last_int);
	if (rc)
		return;
	con_hex2("GPT", "sig lo=", sector[0] | sector[1] << 8 | sector[2] << 16 | (uint32_t)sector[3] << 24,
		 "hi=", sector[4] | sector[5] << 8 | sector[6] << 16 | (uint32_t)sector[7] << 24);
	if (!same_n(sector, "EFI PART", 8)) {
		con_str("GPT", "no EFI PART signature at LBA 1");
		return;
	}
	uint32_t nent = sector[80] | sector[81] << 8 | sector[82] << 16 | (uint32_t)sector[83] << 24;
	uint32_t elba = (uint32_t)le64(sector + 72);
	con_hex2("GPT", "entries=", nent, "at lba=", elba);
	if (nent > 128)
		nent = 128;
	list_partitions(elba, nent);
}

/* Self test: read the first 8 MiB of GPT entry 36 (mmcblk0p36 = boot) with CMD17 PIO, CRC32 it, report in 3 lines. */
void mmc_selftest(void)
{
	static uint32_t tab[256];
	static uint8_t ent[512] __attribute__((aligned(16)));
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		tab[i] = c;
	}
	con_str("EMMC", "selftest p36 8MiB crc32");
	if (read_sector(1, sector) || !same_n(sector, "EFI PART", 8)) {
		con_str("EMMC", "GPT read failed");
		return;
	}
	uint32_t elba = (uint32_t)le64(sector + 72);
	if (read_sector(elba + 35 / 4, ent)) {
		con_str("EMMC", "entry read failed");
		return;
	}
	const uint8_t *e = ent + (35 % 4) * 128;
	uint32_t first = (uint32_t)le64(e + 32), mib = (uint32_t)((le64(e + 40) - le64(e + 32) + 1) >> 11);
	con_hex2("P36", "lba=", first, "MiB=", mib);
	uint32_t crc = 0xffffffffu, rc = 0, n = 2048;	/* 1 MiB; report after 1, 8, 64, 512, 2048 sectors with the elapsed ms */
	uint64_t t0 = ticks();
	for (uint32_t s = 0; s < n; s++) {
		rc = read_sector(first + s, sector);
		if (rc) {
			con_hex2("ERR", "s=", s, "rc=", rc);
			return;
		}
		for (int i = 0; i < 512; i++)
			crc = tab[(crc ^ sector[i]) & 0xff] ^ (crc >> 8);
		if (s == 0 || s == 7 || s == 63 || s == 511 || s == 2047) {
			uint64_t ms = (ticks() - t0) * 1000 / freq();
			con_hex2(s == 0 ? "CRC S1" : s == 7 ? "CRC S8" : s == 63 ? "CRC S64" : s == 511 ? "CRC S512" : "CRC S2048",
				 "=", ~crc, "ms=", (uint32_t)ms);
		}
	}
	con_str("EMMC", "selftest done");
}
