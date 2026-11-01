#pragma once
#include <stdint.h>

#define FB_PHYS	0x7c720000UL	/* keep equal to FB_BASE in console.h */
#define FB_PHYS_SIZE	0x22e0000UL

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
};

int mmu_enable(struct mmu_info *info);
uint64_t mmu_probe(uintptr_t va);
