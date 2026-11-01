#pragma once
#include <stdint.h>

/* LK splash framebuffer [live FDT /chosen atag,videolfb: fb_base 0x7c720000,
 * vramSize 0x22e0000, panel s6e3fc3; iomem hole 0x7c720000-0x7ea40fff matches]. PROBABLE.
 * The dtbo overlay value 0x7e605000 is a placeholder. */
#define FB_BASE		0x7c720000UL
#define FB_SIZE		0x22e0000UL
#define FB_W		1080
#define FB_H		2400
#define FB_STRIDE	(1088 * 4)	/* VERIFIED on hardware: rows readable at 1088 px pitch (720 px pitch gave garbage) */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);

void con_kick(void);
