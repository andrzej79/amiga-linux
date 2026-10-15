// SPDX-License-Identifier: GPL-2.0
/*
 * CS-Lab Warp turbo board - RTG frame buffer driver
 *
 * Copyright (C) 2024-2026 Andrzej Rogozynski, CS-Lab s.c.
 *
 * 24 MB of video memory on the VRAM Zorro board, display/blitter registers on
 * the CTRL board (shared through cswarp-core).  Any timing within the FPGA's
 * limits is accepted: the pixel clock is synthesised by reprogramming the
 * video MMCM through DRP (amiwarp_vidclk.c), 25 .. 148.5 MHz.
 *
 * Console acceleration: fill and copy on the blitter, text (mono images) on
 * the template blitter, the cursor on the hardware sprite.
 *
 * Pixel formats (big-endian in memory):
 *   8 bit  - CLUT, 256 x 0x00RRGGBB
 *   16 bit - RGB565
 *   32 bit - bytes B,G,R,X (red.offset 8, green 16, blue 24)
 *
 * Mode set order matters: program the pixel clock first and only touch the
 * PIXC (pixel clock domain) registers once the MMCM is locked - an access
 * while it is stopped never completes and hangs the machine.
 *
 * Kernel options: video=amiwarpfb:<mode>[,depth:<bpp>]
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/cswarp.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/zorro.h>

#include "amiwarp_vidclk.h"

#define DRV_NAME	"amiwarpfb"
#define DEF_MODE	4	/* 1280x1024@60: 108 MHz, no harmonic in the 2.4 GHz WiFi band */
#define DEF_DEPTH	16

#define WARP_MAX_XRES		1920
#define WARP_MAX_TIMING		4095	/* 12-bit timing counters */
#define WARP_MAX_WPR		1023	/* 10-bit words-per-row fields */
#define WARP_MAX_BLT_Y		4095	/* 12-bit blitter y coordinates */
#define WARP_FAST_CLK_HZ	110000000UL	/* above this only the 7060 meets timing */
#define BLT_IDLE_POLLS		2000000UL

static unsigned long depth_option = DEF_DEPTH;
module_param(depth_option, ulong, 0444);
MODULE_PARM_DESC(depth_option, "Preferred bpp (8, 16, 32)");

static char *mode_option;
module_param(mode_option, charp, 0444);
MODULE_PARM_DESC(mode_option, "Preferred video mode (e.g. 1024x768@60)");

static bool vram_cached = true;
module_param(vram_cached, bool, 0444);
MODULE_PARM_DESC(vram_cached, "Map VRAM write-through cached (0: uncached)");

static bool noaccel;
module_param(noaccel, bool, 0444);
MODULE_PARM_DESC(noaccel, "Disable the blitter (software fill/copy/text)");

static bool hwcursor = true;
module_param(hwcursor, bool, 0444);
MODULE_PARM_DESC(hwcursor, "Console cursor on the hardware sprite (0: drawn in software)");

struct warpfb_par {
	struct cswarp_core *wc;
	struct device *dev;
	void __iomem *regs;		/* CTRL board */
	unsigned long vram_phys;
	u32 pseudo_palette[16];
	struct mutex mode_lock;
	bool clk_ok;			/* MMCM locked: PIXC accessible */
	u32 cur_pixclk_hz;
	bool blt_timeout_logged;
	u32 tmpl[WARP_PATBUFF_BYTES / 4];	/* template being built for the pattern buffer */
	/* console cursor on the sprite */
	u32 spr_ctrl;
	bool spr_valid;			/* spr_img/spr_col are in the hardware */
	u32 spr_img[WARP_SPR_H * 2];
	u32 spr_col[2];
	/* statistics (sysfs) */
	unsigned long n_fill, n_copy, n_text, n_pan, n_cursor, n_blt_wait_timeouts;
};

/*
 * Mode database.  Standard VESA/CEA timings: fb_videomode is
 * { name, refresh, xres, yres, pixclock (ps), left (back porch), right (front
 *   porch), upper, lower, hsync_len, vsync_len, sync, vmode }.
 */
static const struct fb_videomode warpfb_modedb[] = {
	{ "640x480@60", 60, 640, 480, 39722, 48, 16, 33, 10, 96, 2,
	  0, FB_VMODE_NONINTERLACED },
	/* 40.4167 MHz, not 40.000: 60 x 40 MHz = 2400 MHz sits on WiFi channel 1 (as csgfx) */
	{ "800x600@60", 60, 800, 600, 24742, 88, 40, 23, 1, 128, 4,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED },
	/* 64.375 MHz, not 65.000: 37 x 65 MHz = 2405 MHz sits on WiFi channel 1 (as csgfx) */
	{ "1024x768@60", 60, 1024, 768, 15534, 160, 24, 29, 3, 136, 6,
	  0, FB_VMODE_NONINTERLACED },
	{ "1280x720@60", 60, 1280, 720, 13468, 220, 110, 20, 5, 40, 5,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED },
	{ "1280x1024@60", 60, 1280, 1024, 9259, 248, 48, 38, 1, 112, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED },
	{ "1360x768@60", 60, 1360, 768, 11696, 256, 64, 18, 3, 112, 6,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED },
	{ "1680x1050@60", 60, 1680, 1050, 8403, 80, 48, 21, 3, 32, 6,
	  FB_SYNC_HOR_HIGH_ACT, FB_VMODE_NONINTERLACED },	/* CVT reduced blanking */
	{ "1920x1080@60", 60, 1920, 1080, 6734, 148, 88, 36, 4, 44, 5,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED },
};

static const struct fb_fix_screeninfo warpfb_fix_default = {
	.id		= "Warp RTG",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.ypanstep	= 1,
	.accel		= FB_ACCEL_NONE,
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static inline u32 rd(struct warpfb_par *par, unsigned long off)
{
	return z_readl(par->regs + off);
}

static inline void wr(struct warpfb_par *par, u32 val, unsigned long off)
{
	z_writel(val, par->regs + off);
}

static u32 bytes_per_pixel(u32 bpp)
{
	return bpp >> 3;
}

static u32 color_fmt(u32 bpp)
{
	return bpp == 8 ? 0 : bpp == 16 ? 1 : 2;
}

/* rows must be whole 16-byte words: the display fetch and blitter work in them */
static u32 warpfb_line_length(u32 xres_virtual, u32 bpp)
{
	return ALIGN(xres_virtual * bytes_per_pixel(bpp), 16);
}

static u32 var_pixclk_hz(const struct fb_var_screeninfo *var)
{
	if (!var->pixclock)
		return 0;
	return PICOS2KHZ(var->pixclock) * 1000;
}

static int warpfb_wait_blitter(struct warpfb_par *par)
{
	unsigned long i;

	for (i = 0; i < BLT_IDLE_POLLS; i++)
		if (!(rd(par, WARP_BLT_SR) & WARP_BLT_SR_BUSY_MASK))
			return 0;

	par->n_blt_wait_timeouts++;
	if (!par->blt_timeout_logged) {
		par->blt_timeout_logged = true;
		dev_err(par->dev, "blitter stuck busy (BLT_SR 0x%08x)\n", rd(par, WARP_BLT_SR));
	}
	return -ETIMEDOUT;
}

/* ------------------------------------------------------------------------ */
/* WiFi interference check                                                  */
/* ------------------------------------------------------------------------ */

/*
 * The video clocks radiate: a pixel-clock harmonic inside the WiFi channel the
 * board's ESP32 uses costs frames on the air, and the module's block-ack
 * reorder buffer then holds everything behind a lost frame - pings of
 * hundreds of ms in bursts.  Seen with 1024x768@60 (65 MHz * 37 = 2405 MHz) on
 * channel 1.  So, like the AmigaOS driver's Picasso96 settings (1024x768 at
 * 64.375 MHz), every mode set moves its clock off the current channel: the
 * smallest shift that clears it, downwards first, at most +-2.5% (monitors
 * take that as a slightly different refresh).  The channel is the one the
 * board reported at boot (diag).  wifi_dodge=0 turns this off.
 */
#define WIFI_MARGIN_KHZ		2000	/* beyond the 22 MHz channel */
#define WIFI_DODGE_STEPS	10	/* x 0.25% */

static bool wifi_dodge = true;
module_param(wifi_dodge, bool, 0644);
MODULE_PARM_DESC(wifi_dodge, "Shift pixel clocks whose harmonics hit the board's WiFi channel");

/* the channel's band in kHz, margin included; false when unknown */
static bool wifi_band(u8 ch, u32 *lo, u32 *hi)
{
	u32 c;

	if (ch >= 1 && ch <= 13)
		c = (2407 + 5 * ch) * 1000;
	else if (ch == 14)
		c = 2484000;
	else
		return false;
	*lo = c - 11000 - WIFI_MARGIN_KHZ;
	*hi = c + 11000 + WIFI_MARGIN_KHZ;
	return true;
}

/* the first harmonic of khz at or above lo, if it is not above hi */
static bool harmonic_in(u32 khz, u32 lo, u32 hi, u32 *h)
{
	if (!khz)
		return false;
	*h = DIV_ROUND_UP(lo, khz) * khz;
	return *h <= hi;
}

/*
 * The clock to use for a request of hz: hz itself, or the nearest clock
 * within +-2.5% whose achievable PLL setting keeps every harmonic out of the
 * WiFi channel.  Fills cfg; returns the achieved clock.
 */
static u32 warpfb_pick_clock(struct warpfb_par *par, u32 hz, struct warp_vidclk_cfg *cfg)
{
	u32 achieved = warp_vidclk_find(hz, cfg), lo, hi, h;
	int k;

	if (!wifi_dodge || !wifi_band(par->wc->wifi_channel, &lo, &hi) ||
	    !harmonic_in(achieved / 1000, lo, hi, &h))
		return achieved;

	for (k = 1; k <= WIFI_DODGE_STEPS; k++) {
		u32 step = hz / 400 * k;	/* 0.25% per step */
		u32 cand[2] = { hz - step, hz + step };
		int i;

		for (i = 0; i < 2; i++) {
			struct warp_vidclk_cfg c;
			u32 a;

			if (cand[i] < VIDCLK_MIN_HZ || cand[i] > VIDCLK_MAX_HZ)
				continue;
			a = warp_vidclk_find(cand[i], &c);
			if (harmonic_in(a / 1000, lo, hi, &h))
				continue;
			cswarp_dbg(CSWARP_DBG_FB, par->dev,
				   "pixel clock %u Hz -> %u Hz (%s%u.%02u%%) to keep harmonics off WiFi channel %u\n",
				   achieved, a, i ? "+" : "-", k / 4, (k % 4) * 25,
				   par->wc->wifi_channel);
			*cfg = c;
			return a;
		}
	}
	return achieved;	/* no clean clock nearby: set_par warns */
}

static void warpfb_check_wifi(struct warpfb_par *par, u32 pixclk_hz)
{
	u8 ch = par->wc->wifi_channel;
	u32 f = pixclk_hz / 1000, lo, hi, h;	/* kHz */

	if (!wifi_band(ch, &lo, &hi) || !harmonic_in(f, lo, hi, &h))
		return;
	dev_warn(par->dev,
		 "pixel clock %u.%03u MHz: harmonic %u x = %u MHz falls into WiFi channel %u%s - expect WiFi packet loss and latency\n",
		 f / 1000, f % 1000, h / f, h / 1000, ch,
		 wifi_dodge ? " (no clean clock within 2.5%)" : " (wifi_dodge off)");
}

/* ------------------------------------------------------------------------ */
/* Mode setting                                                             */
/* ------------------------------------------------------------------------ */

static int warpfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct warpfb_par *par = info->par;
	struct warp_vidclk_cfg cfg;
	u32 bpp, step, line_length, htotal, vtotal, hz, achieved;

	if (var->bits_per_pixel <= 8)
		bpp = 8;
	else if (var->bits_per_pixel <= 16)
		bpp = 16;
	else if (var->bits_per_pixel <= 32)
		bpp = 32;
	else
		return -EINVAL;
	var->bits_per_pixel = bpp;

	/* the visible line must be whole 16-byte words */
	step = 16 / bytes_per_pixel(bpp);
	var->xres = ALIGN(var->xres, step);
	if (var->xres == 0 || var->xres > WARP_MAX_XRES || var->yres == 0)
		return -EINVAL;
	if (var->vmode & FB_VMODE_INTERLACED || var->vmode & FB_VMODE_DOUBLE)
		return -EINVAL;

	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;
	var->xres_virtual = ALIGN(var->xres_virtual, step);
	if (var->xoffset + var->xres > var->xres_virtual)
		var->xoffset = var->xres_virtual - var->xres;
	if (var->yoffset + var->yres > var->yres_virtual)
		var->yoffset = var->yres_virtual - var->yres;

	line_length = warpfb_line_length(var->xres_virtual, bpp);
	if (line_length / 16 > WARP_MAX_WPR ||
	    var->xres * bytes_per_pixel(bpp) / 16 > WARP_MAX_WPR)
		return -EINVAL;
	if ((u64)line_length * var->yres_virtual > info->fix.smem_len)
		return -ENOMEM;

	htotal = var->xres + var->right_margin + var->hsync_len + var->left_margin;
	vtotal = var->yres + var->lower_margin + var->vsync_len + var->upper_margin;
	if (htotal > WARP_MAX_TIMING || vtotal > WARP_MAX_TIMING || !var->hsync_len || !var->vsync_len)
		return -EINVAL;

	hz = var_pixclk_hz(var);
	if (hz < VIDCLK_MIN_HZ - VIDCLK_MIN_HZ / 100 || hz > VIDCLK_MAX_HZ + VIDCLK_MAX_HZ / 100) {
		cswarp_dbg(CSWARP_DBG_FB, par->dev, "check_var: pixel clock %u Hz out of range\n", hz);
		return -EINVAL;
	}
	achieved = warpfb_pick_clock(par, hz, &cfg);
	var->pixclock = KHZ2PICOS(achieved / 1000);

	switch (bpp) {
	case 8:
		var->red = (struct fb_bitfield){ 0, 8, 0 };
		var->green = (struct fb_bitfield){ 0, 8, 0 };
		var->blue = (struct fb_bitfield){ 0, 8, 0 };
		break;
	case 16:	/* RGB565 */
		var->red = (struct fb_bitfield){ 11, 5, 0 };
		var->green = (struct fb_bitfield){ 5, 6, 0 };
		var->blue = (struct fb_bitfield){ 0, 5, 0 };
		break;
	case 32:	/* memory B,G,R,X */
		var->red = (struct fb_bitfield){ 8, 8, 0 };
		var->green = (struct fb_bitfield){ 16, 8, 0 };
		var->blue = (struct fb_bitfield){ 24, 8, 0 };
		break;
	}
	var->transp = (struct fb_bitfield){ 0, 0, 0 };
	var->nonstd = 0;
	return 0;
}

static void warpfb_set_start(struct warpfb_par *par, struct fb_info *info,
			     u32 xoffset, u32 yoffset)
{
	u32 bpp = bytes_per_pixel(info->var.bits_per_pixel);
	u32 offset = yoffset * info->fix.line_length + xoffset * bpp;

	/* 16-byte units, relative to the start of video memory */
	wr(par, offset >> 4, WARP_DISP_ADDR);
}

static int warpfb_set_par(struct fb_info *info)
{
	struct warpfb_par *par = info->par;
	struct fb_var_screeninfo *var = &info->var;
	struct warp_vidclk_cfg cfg;
	u32 bpp = var->bits_per_pixel, Bpp = bytes_per_pixel(bpp);
	u32 hfp, hsync_end, htotal, vfp, vsync_end, vtotal;
	u32 hz, achieved, disp_cr;
	int status;

	mutex_lock(&par->mode_lock);

	info->fix.visual = bpp == 8 ? FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_TRUECOLOR;
	info->fix.line_length = warpfb_line_length(var->xres_virtual, bpp);
	info->fix.xpanstep = 16 / Bpp;
	info->fix.ypanstep = 1;

	hfp = var->xres + var->right_margin;
	hsync_end = hfp + var->hsync_len;
	htotal = hsync_end + var->left_margin;
	vfp = var->yres + var->lower_margin;
	vsync_end = vfp + var->vsync_len;
	vtotal = vsync_end + var->upper_margin;

	hz = var_pixclk_hz(var);
	achieved = warpfb_pick_clock(par, hz, &cfg);

	cswarp_dbg(CSWARP_DBG_FB, par->dev,
		   "set_par %ux%u (virt %ux%u) %u bpp, line %u B, htotal %u vtotal %u, sync %s/%s, clock %u Hz -> %u Hz (D=%u d=%u m8=%u), %u.%02u Hz refresh\n",
		   var->xres, var->yres, var->xres_virtual, var->yres_virtual, bpp,
		   info->fix.line_length, htotal, vtotal,
		   var->sync & FB_SYNC_HOR_HIGH_ACT ? "+h" : "-h",
		   var->sync & FB_SYNC_VERT_HIGH_ACT ? "+v" : "-v",
		   hz, achieved, cfg.D, cfg.d, cfg.m8,
		   achieved / (htotal * vtotal),
		   (achieved % (htotal * vtotal)) * 100 / (htotal * vtotal));

	if (achieved > WARP_FAST_CLK_HZ && par->wc->board_type != WARP_BOARD_7060)
		dev_warn(par->dev, "%u Hz pixel clock is marginal on %s (only the 7060 meets timing)\n",
			 achieved, cswarp_board_type_name(par->wc->board_type));

	/* 1. pixel clock - nothing in PIXC may be touched until it is locked */
	status = warp_vidclk_apply(par->regs, &cfg, false);
	cswarp_dbg(CSWARP_DBG_FB, par->dev, "pixel clock: %s, PIXCLK_SR 0x%08x\n",
		   warp_vidclk_status_text(status), rd(par, WARP_PIXCLK_SR));
	if (status != VIDCLK_OK && status != VIDCLK_UNCHANGED) {
		par->clk_ok = false;
		dev_err(par->dev, "pixel clock %u Hz: %s\n", achieved,
			warp_vidclk_status_text(status));
		mutex_unlock(&par->mode_lock);
		return -EIO;
	}
	par->clk_ok = true;
	par->cur_pixclk_hz = achieved;
	warpfb_check_wifi(par, achieved);

	/* 2. fetch (MCLK domain): no divided modes */
	wr(par, 0, WARP_DIV_Y_CR);
	wr(par, WARP_DISP_MCR_VAL(info->fix.line_length >> 4, var->yres,
				  (var->xres * Bpp) >> 4), WARP_DISP_MCR);
	warpfb_set_start(par, info, var->xoffset, var->yoffset);

	/* 3. timing (PIXC domain) */
	wr(par, var->xres, WARP_DISP_H_ACT);
	wr(par, WARP_TIMING(var->xres, htotal), WARP_DISP_H_BLANK);
	wr(par, WARP_TIMING(hfp, hsync_end), WARP_DISP_H_SYNC);
	wr(par, var->yres, WARP_DISP_V_ACT);
	wr(par, WARP_TIMING(var->yres, vtotal), WARP_DISP_V_BLANK);
	wr(par, WARP_TIMING(vfp, vsync_end), WARP_DISP_V_SYNC);
	wr(par, WARP_TIMING(vtotal, htotal), WARP_DISP_VH_MAX);
	/* sprite off, no scaling, 1:1 pixels; fbcon loads its cursor again */
	par->spr_ctrl = (rd(par, WARP_SPR_CTRL) & ~(WARP_SPR_CTRL_EN | WARP_SPR_CTRL_SCALE_MASK)) |
			WARP_SPR_CTRL_HIRES;
	par->spr_valid = false;
	wr(par, par->spr_ctrl, WARP_SPR_CTRL);

	/* 4. colour mode, polarity, RTG source; pulse the timing generator reset */
	disp_cr = WARP_DISP_CR_RTG | WARP_DISP_CR_COLOR(color_fmt(bpp));
	if (!(var->sync & FB_SYNC_HOR_HIGH_ACT))
		disp_cr |= WARP_DISP_CR_HSYNC_NEG;
	if (!(var->sync & FB_SYNC_VERT_HIGH_ACT))
		disp_cr |= WARP_DISP_CR_VSYNC_NEG;
	wr(par, disp_cr | WARP_DISP_CR_TG_RESET, WARP_DISP_CR);
	wr(par, disp_cr, WARP_DISP_CR);

	cswarp_dbg(CSWARP_DBG_FB, par->dev,
		   "regs: DISP_MCR 0x%08x DISP_ADDR 0x%08x DISP_CR 0x%08x H_BLANK 0x%08x H_SYNC 0x%08x V_BLANK 0x%08x V_SYNC 0x%08x VH_MAX 0x%08x\n",
		   rd(par, WARP_DISP_MCR), rd(par, WARP_DISP_ADDR), rd(par, WARP_DISP_CR),
		   rd(par, WARP_DISP_H_BLANK), rd(par, WARP_DISP_H_SYNC), rd(par, WARP_DISP_V_BLANK),
		   rd(par, WARP_DISP_V_SYNC), rd(par, WARP_DISP_VH_MAX));

	mutex_unlock(&par->mode_lock);
	return 0;
}

/* Back to the Amiga's own video through the scandoubler (as csgfx setSwitch) */
static void warpfb_restore_native(struct warpfb_par *par)
{
	int status;

	mutex_lock(&par->mode_lock);
	status = warp_vidclk_apply(par->regs, &warp_vidclk_native, false);
	cswarp_dbg(CSWARP_DBG_FB, par->dev, "native clock: %s\n", warp_vidclk_status_text(status));
	if (status == VIDCLK_OK || status == VIDCLK_UNCHANGED) {
		/* 1280x1024 32 bit scandoubler timing, then the Amiga video source */
		wr(par, 0, WARP_DIV_Y_CR);
		wr(par, WARP_DISP_MCR_VAL(320, 1024, 320), WARP_DISP_MCR);
		wr(par, 0, WARP_DISP_ADDR);
		wr(par, 1280, WARP_DISP_H_ACT);
		wr(par, WARP_TIMING(1280, 1688), WARP_DISP_H_BLANK);
		wr(par, WARP_TIMING(1328, 1440), WARP_DISP_H_SYNC);
		wr(par, 1024, WARP_DISP_V_ACT);
		wr(par, WARP_TIMING(1024, 1066), WARP_DISP_V_BLANK);
		wr(par, WARP_TIMING(1025, 1028), WARP_DISP_V_SYNC);
		wr(par, WARP_TIMING(1066, 1688), WARP_DISP_VH_MAX);
		wr(par, rd(par, WARP_SPR_CTRL) & ~WARP_SPR_CTRL_EN, WARP_SPR_CTRL);
		wr(par, WARP_DISP_CR_COLOR(2) | WARP_DISP_CR_TG_RESET, WARP_DISP_CR);
		wr(par, WARP_DISP_CR_COLOR(2), WARP_DISP_CR);
	} else {
		dev_err(par->dev, "cannot restore the native video clock: %s\n",
			warp_vidclk_status_text(status));
	}
	mutex_unlock(&par->mode_lock);
}

static int warpfb_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
			    unsigned int blue, unsigned int transp, struct fb_info *info)
{
	struct warpfb_par *par = info->par;

	if (regno >= 256)
		return 1;

	if (info->var.grayscale)
		red = green = blue = (red * 77 + green * 151 + blue * 28) >> 8;

	if (info->fix.visual == FB_VISUAL_PSEUDOCOLOR) {
		if (!par->clk_ok)
			return 0;
		wr(par, ((red >> 8) << 16) | ((green >> 8) << 8) | (blue >> 8),
		   WARP_REGS_CLUT_OFFSET + regno * 4);
		return 0;
	}

	if (regno >= 16)
		return 1;
#define CNVT_TOHW(val, width) ((((val) << (width)) + 0x7FFF - (val)) >> 16)
	par->pseudo_palette[regno] =
		(CNVT_TOHW(red, info->var.red.length) << info->var.red.offset) |
		(CNVT_TOHW(green, info->var.green.length) << info->var.green.offset) |
		(CNVT_TOHW(blue, info->var.blue.length) << info->var.blue.offset);
#undef CNVT_TOHW
	return 0;
}

static int warpfb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct warpfb_par *par = info->par;
	u32 step = info->fix.xpanstep ? info->fix.xpanstep : 1;

	if (var->xoffset + info->var.xres > info->var.xres_virtual ||
	    var->yoffset + info->var.yres > info->var.yres_virtual ||
	    var->xoffset % step)
		return -EINVAL;
	if (var->vmode & FB_VMODE_YWRAP)
		return -EINVAL;

	par->n_pan++;
	warpfb_set_start(par, info, var->xoffset, var->yoffset);
	cswarp_dbg(CSWARP_DBG_FBACC, par->dev, "pan %u,%u\n", var->xoffset, var->yoffset);
	return 0;
}

static int warpfb_blank(int blank, struct fb_info *info)
{
	/* no DPMS control in the FPGA; let fbcon draw black */
	return blank == FB_BLANK_UNBLANK ? 0 : 1;
}

/* ------------------------------------------------------------------------ */
/* Blitter                                                                  */
/* ------------------------------------------------------------------------ */

static int warpfb_sync(struct fb_info *info)
{
	struct warpfb_par *par = info->par;

	if (!noaccel)
		warpfb_wait_blitter(par);
	return 0;
}

static u32 warpfb_pen(struct fb_info *info, u32 color)
{
	struct warpfb_par *par = info->par;

	if (info->fix.visual == FB_VISUAL_TRUECOLOR || info->fix.visual == FB_VISUAL_DIRECTCOLOR)
		return color < 16 ? par->pseudo_palette[color] : color;
	return color & 0xff;
}

static void warpfb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	struct warpfb_par *par = info->par;
	u32 bpp = info->var.bits_per_pixel, Bpp = bytes_per_pixel(bpp);
	u32 bpr = info->fix.line_length;
	u32 pen;

	if (noaccel || rect->rop != ROP_COPY || !par->clk_ok) {
		warpfb_sync(info);
		cfb_fillrect(info, rect);
		return;
	}
	if (!rect->width || !rect->height)
		return;

	pen = warpfb_pen(info, rect->color);
	par->n_fill++;
	cswarp_dbg(CSWARP_DBG_FBACC, par->dev, "fill %u,%u %ux%u pen 0x%08x\n",
		   rect->dx, rect->dy, rect->width, rect->height, pen);

	if (warpfb_wait_blitter(par)) {
		cfb_fillrect(info, rect);
		return;
	}
	wr(par, par->vram_phys + rect->dy * bpr + rect->dx * Bpp, WARP_BLT_DST);
	wr(par, (rect->dy << 16) | rect->dx, WARP_BLT_DST_XY);
	wr(par, (rect->dy << 16) | rect->dx, WARP_BLT_SRC_XY);
	wr(par, (rect->height << 16) | rect->width, WARP_BLT_WH);
	wr(par, pen, WARP_BLT_COLOR);
	wr(par, bpr, WARP_BLT_SRC_BPR);
	wr(par, bpr, WARP_BLT_DST_BPR);
	wr(par, WARP_BLT_CR_FILL | WARP_BLT_CR_FMT(color_fmt(bpp)), WARP_BLT_CR);
}

static void warpfb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	struct warpfb_par *par = info->par;
	u32 bpp = info->var.bits_per_pixel, Bpp = bytes_per_pixel(bpp);
	u32 bpr = info->fix.line_length;

	if (noaccel || !par->clk_ok) {
		warpfb_sync(info);
		cfb_copyarea(info, area);
		return;
	}
	if (!area->width || !area->height)
		return;

	par->n_copy++;
	cswarp_dbg(CSWARP_DBG_FBACC, par->dev, "copy %u,%u -> %u,%u %ux%u\n",
		   area->sx, area->sy, area->dx, area->dy, area->width, area->height);

	if (warpfb_wait_blitter(par)) {
		cfb_copyarea(info, area);
		return;
	}
	wr(par, par->vram_phys + area->sy * bpr + area->sx * Bpp, WARP_BLT_SRC);
	wr(par, par->vram_phys + area->dy * bpr + area->dx * Bpp, WARP_BLT_DST);
	wr(par, (area->sy << 16) | area->sx, WARP_BLT_SRC_XY);
	wr(par, (area->dy << 16) | area->dx, WARP_BLT_DST_XY);
	wr(par, (area->height << 16) | area->width, WARP_BLT_WH);
	wr(par, bpr, WARP_BLT_SRC_BPR);
	wr(par, bpr, WARP_BLT_DST_BPR);
	wr(par, WARP_BLT_CR_COPY | WARP_BLT_CR_FMT(color_fmt(bpp)), WARP_BLT_CR);
}

/*
 * Mono images - fbcon text, a run of glyphs per call - go through the template
 * blitter: the 1-bit rows are loaded into the FPGA's 2 KB pattern buffer and
 * expanded to FG/BG pixels in VRAM, so the CPU sends a few hundred bytes over
 * the bus instead of writing every pixel.  Runs too wide for the buffer are
 * cut into slices.  Other images (the logo) are drawn by the CPU.
 */
static void warpfb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	struct warpfb_par *par = info->par;
	u32 bpp = info->var.bits_per_pixel, Bpp = bytes_per_pixel(bpp);
	u32 bpr = info->fix.line_length;
	u32 pitch = DIV_ROUND_UP(image->width, 8);
	u32 h = image->height, slice, x0, fg, bg, base;

	if (noaccel || !par->clk_ok || image->depth != 1 || !image->width || !h ||
	    h > WARP_PATBUFF_BYTES / 2) {
		/* the CPU must not race the blitter on the same memory */
		warpfb_sync(info);
		cfb_imageblit(info, image);
		return;
	}

	fg = warpfb_pen(info, image->fg_color);
	bg = warpfb_pen(info, image->bg_color);
	if (bpp == 32) {
		/* B,G,R,X in memory: the blitter takes the pen as 0xBBGGRR */
		fg >>= 8;
		bg >>= 8;
	}
	/* rows of 16-bit words; the slice must fit the buffer and PAT_BPR's 8 bits */
	slice = min(127U, WARP_PATBUFF_BYTES / 2 / h) * 16;
	/* the image's first row as the base: PAT_DST_Y has only 12 bits */
	base = (((par->vram_phys + image->dy * bpr) & 0x0fffffff) + WARP_VRAM_BOARD_BASE) >> 4;
	par->n_text++;

	for (x0 = 0; x0 < image->width; x0 += slice) {
		u32 w = min(slice, image->width - x0);
		u32 tbpr = DIV_ROUND_UP(w, 16) * 2;
		u32 n = min(tbpr, pitch - x0 / 8);
		u32 longs = DIV_ROUND_UP(tbpr * h, 4);
		const u8 *src = image->data + x0 / 8;
		u8 *t = (u8 *)par->tmpl;
		u32 r, i;

		for (r = 0; r < h; r++, src += pitch, t += tbpr) {
			memcpy(t, src, n);
			memset(t + n, 0, tbpr - n);
		}
		memset(t, 0, longs * 4 - tbpr * h);

		/* one pattern buffer: the previous template must be done with */
		if (warpfb_wait_blitter(par)) {
			cfb_imageblit(info, image);
			return;
		}
		wr(par, 0, WARP_REGS_PATBUFF_OFFSET + 4);
		for (i = 0; i < longs; i++)
			wr(par, par->tmpl[i], WARP_REGS_PATBUFF_OFFSET);
		wr(par, 0, WARP_PAT_SHIFT);
		wr(par, tbpr, WARP_PAT_BPR);
		wr(par, w, WARP_PAT_WIDTH);
		wr(par, h, WARP_PAT_HEIGHT);
		wr(par, 0, WARP_PAT_PATTERN_H);
		wr(par, base, WARP_PAT_DST_BASE);
		wr(par, bpr, WARP_PAT_DST_BPR);
		wr(par, Bpp, WARP_PAT_DST_BPP);
		wr(par, bg, WARP_PAT_BG);
		wr(par, fg, WARP_PAT_FG);
		wr(par, image->dx + x0, WARP_PAT_DST_X);
		wr(par, 0, WARP_PAT_DST_Y);
		wr(par, WARP_PATBLT_CR_START, WARP_PATBLT_CR);
	}
}

/* ------------------------------------------------------------------------ */
/* Console cursor on the hardware sprite                                    */
/* ------------------------------------------------------------------------ */

static void warpfb_sprite_enable(struct warpfb_par *par, bool on)
{
	u32 ctrl = on ? par->spr_ctrl | WARP_SPR_CTRL_EN : par->spr_ctrl & ~WARP_SPR_CTRL_EN;

	if (ctrl != par->spr_ctrl || !par->spr_valid) {
		par->spr_ctrl = ctrl;
		wr(par, ctrl, WARP_SPR_CTRL);
	}
}

static u32 warpfb_cmap_rgb(struct fb_info *info, u32 idx)
{
	const struct fb_cmap *c = &info->cmap;

	if (idx >= c->len)
		return 0xffffff;
	return (c->red[idx] >> 8) << 16 | (c->green[idx] >> 8) << 8 | (c->blue[idx] >> 8);
}

/*
 * fbcon's cursor on the FPGA sprite.  soft_cursor() redraws the character
 * cell with the glyph XORed by the cursor shape; the sprite shows the same,
 * but only over the shape - the inverted glyph in FG/BG - and is transparent
 * elsewhere, so the character below is left as it is.  Blinking only toggles
 * the enable bit, and the image is reloaded only when it changes.
 */
static int warpfb_cursor(struct fb_info *info, struct fb_cursor *cursor)
{
	struct warpfb_par *par = info->par;
	const struct fb_image *img = &cursor->image;
	u32 pitch = DIV_ROUND_UP(img->width, 8);
	u32 spr[WARP_SPR_H * 2], col[2];
	u32 x, y, i;

	if (!hwcursor || !par->clk_ok)
		return -EINVAL;
	if (img->width > WARP_SPR_W || img->height > WARP_SPR_H || img->depth != 1 ||
	    cursor->rop != ROP_XOR) {
		/* drawn in software from now on: the sprite must not stay up */
		warpfb_sprite_enable(par, false);
		return -EINVAL;
	}
	if (!cursor->enable) {
		warpfb_sprite_enable(par, false);
		return 0;
	}

	memset(spr, 0, sizeof(spr));
	for (y = 0; y < img->height; y++) {
		const u8 *g = (const u8 *)img->data + y * pitch;
		const u8 *m = (const u8 *)cursor->mask + y * pitch;

		for (x = 0; x < img->width; x++) {
			u8 bit = 0x80 >> (x & 7);

			if (m[x >> 3] & bit)	/* glyph ^ mask: FG (COL0) where the glyph is clear */
				spr[y * 2 + (x >> 4)] |= (g[x >> 3] & bit ? 2u : 1u) << ((x & 15) * 2);
		}
	}
	col[0] = warpfb_cmap_rgb(info, img->fg_color);
	col[1] = warpfb_cmap_rgb(info, img->bg_color);

	par->n_cursor++;
	if (!par->spr_valid || memcmp(spr, par->spr_img, sizeof(spr))) {
		/* hide while the buffer is rewritten, so no half-new frame shows */
		warpfb_sprite_enable(par, false);
		wr(par, 0, WARP_REGS_SPRBUFF_OFFSET + 4);
		for (i = 0; i < WARP_SPR_H * 2; i++)
			wr(par, spr[i], WARP_REGS_SPRBUFF_OFFSET);
		memcpy(par->spr_img, spr, sizeof(spr));
	}
	if (!par->spr_valid || col[0] != par->spr_col[0] || col[1] != par->spr_col[1]) {
		wr(par, col[0], WARP_SPR_COL0);
		wr(par, col[1], WARP_SPR_COL1);
		par->spr_col[0] = col[0];
		par->spr_col[1] = col[1];
	}
	/* 12-bit signed screen coordinates */
	wr(par, (img->dx - info->var.xoffset) & 0xfff, WARP_SPR_POS_X);
	wr(par, (img->dy - info->var.yoffset) & 0xfff, WARP_SPR_POS_Y);
	par->spr_valid = true;
	warpfb_sprite_enable(par, true);
	return 0;
}

/*
 * A user space opener (the X driver) may program the sprite through the
 * mmapped registers: reload the console cursor from scratch afterwards.
 */
static int warpfb_open(struct fb_info *info, int user)
{
	struct warpfb_par *par = info->par;

	if (user)
		par->spr_valid = false;
	return 0;
}

static int warpfb_release(struct fb_info *info, int user)
{
	struct warpfb_par *par = info->par;

	if (user && par->clk_ok) {
		par->spr_ctrl = (rd(par, WARP_SPR_CTRL) &
				 ~(WARP_SPR_CTRL_EN | WARP_SPR_CTRL_SCALE_MASK)) | WARP_SPR_CTRL_HIRES;
		par->spr_valid = false;
	}
	return 0;
}

static ssize_t warpfb_read(struct fb_info *info, char __user *buf, size_t count, loff_t *ppos)
{
	warpfb_sync(info);
	return fb_io_read(info, buf, count, ppos);
}

static ssize_t warpfb_write(struct fb_info *info, const char __user *buf, size_t count,
			    loff_t *ppos)
{
	warpfb_sync(info);
	return fb_io_write(info, buf, count, ppos);
}

static const struct fb_ops warpfb_ops = {
	.owner		= THIS_MODULE,
	.fb_open	= warpfb_open,
	.fb_release	= warpfb_release,
	.fb_read	= warpfb_read,
	.fb_write	= warpfb_write,
	.fb_mmap	= fb_io_mmap,
	.fb_check_var	= warpfb_check_var,
	.fb_set_par	= warpfb_set_par,
	.fb_setcolreg	= warpfb_setcolreg,
	.fb_pan_display	= warpfb_pan_display,
	.fb_blank	= warpfb_blank,
	.fb_fillrect	= warpfb_fillrect,
	.fb_copyarea	= warpfb_copyarea,
	.fb_imageblit	= warpfb_imageblit,
	.fb_cursor	= warpfb_cursor,
	.fb_sync	= warpfb_sync,
};

/* ------------------------------------------------------------------------ */
/* sysfs                                                                    */
/* ------------------------------------------------------------------------ */

static ssize_t accel_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct warpfb_par *par = info->par;

	return sysfs_emit(buf, "fill %lu\ncopy %lu\ntext %lu\npan %lu\ncursor %lu\nblt_wait_timeouts %lu\npixclk_hz %u\n",
			  par->n_fill, par->n_copy, par->n_text, par->n_pan, par->n_cursor,
			  par->n_blt_wait_timeouts, par->cur_pixclk_hz);
}
static DEVICE_ATTR_RO(accel_stats);

static struct attribute *warpfb_attrs[] = {
	&dev_attr_accel_stats.attr,
	NULL
};
ATTRIBUTE_GROUPS(warpfb);

/* ------------------------------------------------------------------------ */
/* Probe                                                                    */
/* ------------------------------------------------------------------------ */

static int warpfb_probe(struct platform_device *pdev)
{
	struct cswarp_core *wc = cswarp_core_get(pdev);
	struct zorro_dev *zvram;
	struct warpfb_par *par;
	struct fb_info *info;
	u32 sr;
	int err;

	if (!wc)
		return -ENODEV;

	/* same gate as csgfx.card FindCard() */
	if (wc->syscfg_id < SYSCFG_ID_L2STATS) {
		dev_err(&pdev->dev, "FPGA image too old for RTG (SysCfg id 0x%08x < 0x%08x): update the firmware package\n",
			wc->syscfg_id, SYSCFG_ID_L2STATS);
		return -ENODEV;
	}
	sr = z_readl(wc->base + WARP_PIXCLK_SR);
	if (!warp_vidclk_has_drp(wc->base)) {
		dev_err(&pdev->dev, "FPGA image has no video PLL DRP interface (PIXCLK_SR 0x%08x): update the firmware package\n",
			sr);
		return -ENODEV;
	}

	zvram = zorro_find_device(ZORRO_PROD_CSLAB_WARP_VRAM, NULL);
	if (!zvram) {
		dev_err(&pdev->dev, "Warp VRAM board not found\n");
		return -ENODEV;
	}

	info = framebuffer_alloc(sizeof(*par), &pdev->dev);
	if (!info)
		return -ENOMEM;
	par = info->par;
	par->wc = wc;
	par->dev = &pdev->dev;
	par->regs = wc->base;
	par->vram_phys = zorro_resource_start(zvram);
	mutex_init(&par->mode_lock);
	par->clk_ok = !!(sr & PIXCLK_SR_LOCKED);

	dev_info(&pdev->dev, "VRAM at 0x%08lx (board 0x%lx bytes, using %u MB), PIXCLK_SR 0x%08x, DISP_CR 0x%08x\n",
		 par->vram_phys, (unsigned long)zorro_resource_len(zvram), WARP_VRAM_SIZE >> 20, sr,
		 par->clk_ok ? rd(par, WARP_DISP_CR) : 0xffffffff);

	if (!devm_request_mem_region(&pdev->dev, par->vram_phys, WARP_VRAM_SIZE, DRV_NAME))
		dev_warn(&pdev->dev, "could not claim the VRAM region, continuing\n");

	info->fix = warpfb_fix_default;
	info->fix.smem_start = par->vram_phys;
	info->fix.smem_len = WARP_VRAM_SIZE;
	/*
	 * What user space (the X driver) may mmap: only the first page of the
	 * CTRL board - blitter, display, CLUT and sprite registers.  The DPRAM
	 * mailbox (+0x1000) and the IDE port (+0x6000) belong to the kernel.
	 */
	info->fix.mmio_start = wc->phys;
	info->fix.mmio_len = WARP_OFFSET_DPREG_CR;
	info->screen_base = vram_cached ? ioremap_wt(par->vram_phys, WARP_VRAM_SIZE)
					: z_ioremap(par->vram_phys, WARP_VRAM_SIZE);
	if (!info->screen_base) {
		dev_err(&pdev->dev, "cannot map VRAM\n");
		err = -ENOMEM;
		goto err_release;
	}
	info->screen_size = WARP_VRAM_SIZE;
	info->fbops = &warpfb_ops;
	info->pseudo_palette = par->pseudo_palette;
	info->flags = FBINFO_HWACCEL_YPAN | FBINFO_HWACCEL_XPAN;
	if (!noaccel)
		/*
		 * Not FBINFO_HWACCEL_IMAGEBLIT, although imageblit is the template
		 * blitter: fbcon would then scroll by redrawing every line of text
		 * (SCROLL_REDRAW) instead of one blitter copy, twice as slow.
		 */
		info->flags |= FBINFO_HWACCEL_COPYAREA | FBINFO_HWACCEL_FILLRECT;
	cswarp_dbg(CSWARP_DBG_FB, &pdev->dev, "VRAM mapped %s at %p, accel %s\n",
		   vram_cached ? "write-through" : "uncached", info->screen_base,
		   noaccel ? "off" : "on");

	/* the blitter may be mid-operation from AmigaOS */
	if (!noaccel)
		warpfb_wait_blitter(par);

	if (mode_option)
		dev_info(&pdev->dev, "mode option \"%s\", depth %lu\n", mode_option, depth_option);
	if (!fb_find_mode(&info->var, info, mode_option, warpfb_modedb,
			  ARRAY_SIZE(warpfb_modedb), &warpfb_modedb[DEF_MODE], depth_option)) {
		dev_err(&pdev->dev, "no usable video mode\n");
		err = -EINVAL;
		goto err_unmap;
	}
	fb_videomode_to_modelist(warpfb_modedb, ARRAY_SIZE(warpfb_modedb), &info->modelist);

	/*
	 * A virtual screen taller than the visible one lets fbcon scroll by
	 * panning (SCROLL_PAN_MOVE: one register write per line, one blitter
	 * copy when the bottom is reached) instead of copying runs of changed
	 * characters on every line.  At most 4095 lines: the blitter's y
	 * coordinates have 12 bits.
	 */
	info->var.yres_virtual = max(info->var.yres,
				     min_t(u32, WARP_MAX_BLT_Y,
					   WARP_VRAM_SIZE / warpfb_line_length(info->var.xres_virtual,
									       info->var.bits_per_pixel)));

	err = warpfb_check_var(&info->var, info);
	if (err) {
		dev_err(&pdev->dev, "initial mode rejected (%d)\n", err);
		goto err_modelist;
	}

	err = fb_alloc_cmap(&info->cmap, 256, 0);
	if (err)
		goto err_modelist;

	/* set the mode before registering, so fbcon starts on a working display */
	err = warpfb_set_par(info);
	if (err)
		goto err_cmap;

	/* clear the visible screen */
	if (!noaccel) {
		struct fb_fillrect r = {
			.width = info->var.xres_virtual, .height = info->var.yres,
			.color = 0, .rop = ROP_COPY,
		};

		warpfb_fillrect(info, &r);
		warpfb_wait_blitter(par);
	}

	platform_set_drvdata(pdev, info);
	err = register_framebuffer(info);
	if (err) {
		dev_err(&pdev->dev, "register_framebuffer failed (%d)\n", err);
		goto err_native;
	}
	if (devm_device_add_groups(&pdev->dev, warpfb_groups))
		dev_warn(&pdev->dev, "sysfs statistics not created\n");

	fb_info(info, "Warp RTG %ux%u %u bpp, %u kHz pixel clock, %u MB VRAM at 0x%08lx\n",
		info->var.xres, info->var.yres, info->var.bits_per_pixel,
		par->cur_pixclk_hz / 1000, WARP_VRAM_SIZE >> 20, par->vram_phys);
	return 0;

err_native:
	warpfb_restore_native(par);
err_cmap:
	fb_dealloc_cmap(&info->cmap);
err_modelist:
	fb_destroy_modelist(&info->modelist);
err_unmap:
	iounmap(info->screen_base);
err_release:
	framebuffer_release(info);
	return err;
}

static void warpfb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);
	struct warpfb_par *par = info->par;

	unregister_framebuffer(info);
	warpfb_sync(info);
	warpfb_restore_native(par);
	fb_dealloc_cmap(&info->cmap);
	fb_destroy_modelist(&info->modelist);
	iounmap(info->screen_base);
	framebuffer_release(info);
}

static void warpfb_shutdown(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);

	if (info) {
		warpfb_sync(info);
		warpfb_restore_native(info->par);
	}
}

static struct platform_driver warpfb_driver = {
	.probe		= warpfb_probe,
	.remove_new	= warpfb_remove,
	.shutdown	= warpfb_shutdown,
	.driver		= {
		.name	= "cswarp-fb",
	},
};

#ifndef MODULE
static int __init warpfb_setup(char *options)
{
	char *this_opt;

	if (!options || !*options)
		return 0;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;
		if (!strncmp(this_opt, "depth:", 6))
			depth_option = simple_strtoul(this_opt + 6, NULL, 0);
		else
			mode_option = this_opt;
	}
	return 0;
}
#endif

static int __init warpfb_init(void)
{
#ifndef MODULE
	char *option = NULL;

	if (fb_get_options(DRV_NAME, &option))
		return -ENODEV;
	warpfb_setup(option);
#endif
	return platform_driver_register(&warpfb_driver);
}
module_init(warpfb_init);

static void __exit warpfb_exit(void)
{
	platform_driver_unregister(&warpfb_driver);
}
module_exit(warpfb_exit);

MODULE_ALIAS("platform:cswarp-fb");
MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("frame buffer driver for the CS-Lab Warp RTG video");
MODULE_LICENSE("GPL");
