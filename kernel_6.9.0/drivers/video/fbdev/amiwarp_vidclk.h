/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CS-Lab Warp - video pixel clock (Xilinx 7-series MMCME2 via DRP).
 *
 * Port of csWarpAmigaSoftware common/mmcm_drp.c and common/vidclk_hw.c,
 * themselves a port of Xilinx XAPP888 (mmcme2_drp.v / mmcme2_drp_func.h).
 *
 * The video MMCM: CLKIN 200 MHz, DIVCLK D, CLKFBOUT M = m8/8, CLKOUT1 = the
 * x5 (TMDS) clock divided by d, CLKOUT2 = the pixel clock divided by 5*d.
 *   VCO   = 200 MHz * m8 / (8 * D)        600..1200 MHz
 *   pixel = VCO / (5 * d) = 5 MHz * m8 / (D * d)
 */
#ifndef _AMIWARP_VIDCLK_H
#define _AMIWARP_VIDCLK_H

#include <linux/types.h>

#define VIDCLK_MIN_HZ	25000000UL
#define VIDCLK_MAX_HZ	148500000UL

struct warp_vidclk_cfg {
	u8 D;		/* 1..20 */
	u8 d;		/* 1..25 */
	u16 m8;		/* M * 8, 16..512 */
};

enum {
	VIDCLK_OK = 0,		/* programmed and locked */
	VIDCLK_UNCHANGED,	/* the MMCM already runs this program */
	VIDCLK_ERR_NO_DRP,	/* FPGA without the DRP interface */
	VIDCLK_ERR_CONFIG,	/* configuration out of range */
	VIDCLK_ERR_DRP_TIMEOUT,	/* a DRP access timed out */
	VIDCLK_ERR_LOCK,	/* programmed, but no lock */
};

/* 108.125 MHz: the scandoubler / Amiga video passthrough clock */
extern const struct warp_vidclk_cfg warp_vidclk_native;

u32 warp_vidclk_hz(const struct warp_vidclk_cfg *c);
/* closest configuration to hz (clamped); returns the achieved clock in Hz */
u32 warp_vidclk_find(u32 hz, struct warp_vidclk_cfg *c);
bool warp_vidclk_has_drp(void __iomem *regs);
/*
 * Program cfg (regs = CTRL board base).  Caller serialises.  The PIXC
 * registers must not be touched until this returns VIDCLK_OK/UNCHANGED.
 */
int warp_vidclk_apply(void __iomem *regs, const struct warp_vidclk_cfg *cfg, bool force);
const char *warp_vidclk_status_text(int status);

#endif /* _AMIWARP_VIDCLK_H */
