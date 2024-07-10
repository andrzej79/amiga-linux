/*
 *  linux/drivers/video/amiwarpfb.c -- Amiga / csWarp frame buffer device
 *
 *      Copyright (C) 2024 Andrzej Rogozynski
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/zorro.h>

#include <linux/fb.h>
#include <linux/init.h>

#include "amiwarpfb.h"

#define VIDEOMEMSIZE	(24*1024*1024)	/* 24 MB */
#define DEF_MODE 	1
#define DEF_DEPTH 	16

//#define IMAGE_BLIT_SUPPORT

static u_long videomemorysize = VIDEOMEMSIZE;

static u_long depth_option = DEF_DEPTH;
module_param(depth_option, ulong, 0);
MODULE_PARM_DESC(depth_option, "Preferred video bpp (8, 16, 32)");

static char *mode_option = NULL;
module_param(mode_option, charp, 0);
MODULE_PARM_DESC(mode_option, "Preferred video mode (e.g. 640x480p@75)");

static ulong stat_hw_fill_calls = 0;
static ulong stat_hw_copy_calls = 0;
static ulong stat_hw_pan_calls = 0;

static ssize_t statHwFillShow(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%lu\n", stat_hw_fill_calls);
}
static ssize_t statHwCopyShow(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%lu\n", stat_hw_copy_calls);
}
static ssize_t statHwPanShow(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%lu\n", stat_hw_pan_calls);
}

static DEVICE_ATTR(stat_hw_fill_calls, 0444, statHwFillShow, NULL);
static DEVICE_ATTR(stat_hw_copy_calls, 0444, statHwCopyShow, NULL);
static DEVICE_ATTR(stat_hw_pan_calls, 0444, statHwPanShow, NULL);

/**
 * @brief predefined video modes
*/
static struct fb_videomode vid_modedb[] = {
	{
		/* 640x480, 37.5 kHz, 75 Hz  */
		"640x480p@75", 75, 640, 480, 31700, 16, 120, 1, 16, 64, 3,
		0, FB_VMODE_NONINTERLACED
	}, {
		/* 800x600, 37.9 kHz, 60 Hz  */
		"800x600p@60", 60, 800, 600, 25000, 40, 88, 1, 23, 128, 4,
		0, FB_VMODE_NONINTERLACED
	}, {
		/* 1024x768, 48.4 kHz, 60 Hz  */
		"1024x768p@60", 60, 1024, 768, 15400, 24, 160, 3, 29, 136, 6,
		0, FB_VMODE_NONINTERLACED
	}, {
		/* 1280x720, 45.0 kHz, 60 Hz  */
		"1280x720p@60", 60, 1280, 720, 13500, 110, 220, 5, 20, 40, 5,
		0, FB_VMODE_NONINTERLACED
	}, {
		/* 1280x1024, 64.0 kHz, 60 Hz  */
		"1280x1024p@60", 60, 1280, 1024, 9300, 48, 248, 1, 38, 112, 3,
		0, FB_VMODE_NONINTERLACED
	},
};
#define NUM_TOTAL_MODES  ARRAY_SIZE(vid_modedb)

static struct fb_fix_screeninfo warpfb_fix = {
	.id 		= "csWarp-fb",
	.type 		= FB_TYPE_PACKED_PIXELS,
	.visual 	= FB_VISUAL_TRUECOLOR,
	.xpanstep 	= 1,
	.ypanstep 	= 1,
	.ywrapstep 	= 1,
	.accel 		= FB_ACCEL_NONE,
};

static int warpfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info);
static int warpfb_set_par(struct fb_info *info);
static int warpfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue, 
							u_int transp, struct fb_info *info);
static int warpfb_pan_display(struct fb_var_screeninfo *var,
			   				  struct fb_info *info);
static void warpfb_fillrect(struct fb_info *info, const struct fb_fillrect *rect);
static void warpfb_copyarea(struct fb_info *info, const struct fb_copyarea *region);

#ifdef IMAGE_BLIT_SUPPORT
static void warpfb_imageblt(struct fb_info *info, const struct fb_image *img);
#endif

static const struct fb_ops warpfb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_check_var	= warpfb_check_var,
	.fb_set_par		= warpfb_set_par,
	.fb_setcolreg	= warpfb_setcolreg,
	.fb_pan_display	= warpfb_pan_display,
	.fb_fillrect	= warpfb_fillrect,
	.fb_copyarea	= warpfb_copyarea,
#ifdef IMAGE_BLIT_SUPPORT
	.fb_imageblit	= warpfb_imageblt,
#else
	.fb_imageblit	= cfb_imageblit,
#endif
};

    /*
     *  Internal routines
     */

static u_long get_line_length(int xres_virtual, int bpp)
{
	u_long length;

	length = xres_virtual * bpp;
	length = (length + 31) & ~31;
	length >>= 3;
	return (length);
}

static bool configurePixelClock(struct fb_info *info, WarpPixClkType pixClkValue) 
{
	WarpFBPrivData *par = (WarpFBPrivData*)info->par;
	uint32_t newClkSel = 0;

	switch(pixClkValue) {
	case WARP_PIXCLK_31_5_MHZ:
		newClkSel |= (PIXCLK_CR_S1);
		break;
	case WARP_PIXCLK_40_MHZ:
		newClkSel |= (PIXCLK_CR_S2);
		break;
	case WARP_PIXCLK_65_MHZ:
		newClkSel |= (PIXCLK_CR_S3);
		break;
	case WARP_PIXCLK_70_219_MHZ:
		newClkSel |= (PIXCLK_CR_S4);
		break;
	case WARP_PIXCLK_108_125_MHZ:
		newClkSel |= (PIXCLK_CR_S5);
		break;
	case WARP_PIXCLK_123_75_MHZ:
		newClkSel |= (PIXCLK_CR_S6);
		break;
	default:
		fb_warn(info, "Unsupported pixel clock value: %lu \n", (u_long)pixClkValue);
		return false;
	}

	uint32_t currentClkSel = (par->bregs->pixclk_cr & PIXCLK_CR_S_MASK);

	if(newClkSel != currentClkSel) {
		par->bregs->pixclk_cr = (newClkSel | PIXCLK_CR_RECFG);
		fb_dbg(info, "waiting for pll lock...\n");
		// wait for pixel clock PLL lock
		while(!(par->bregs->pixclk_sr & 0x02)) {};
		fb_dbg(info, "pixel clock pll ready\n");
		return true;
	} else {
		fb_dbg(info, "pixel clock reconfiguration not necessary\n");
		return false;
	}
}

/**
 * @brief CS-WARP video mode screen mode initialization
 */
static void csInitDisplay(struct fb_info *info, CSScrMode scrMode, int bpp) 
{
	bool clkReconfigured = false;

	WarpFBPrivData *par = (WarpFBPrivData*)info->par;

	uint32_t col = 0;
	uint16_t shift = 0;
	if(bpp == 16) {
		shift = 1;
		col = (1 << 3);
	}
	if(bpp == 32) {
		shift = 2;
		col = (2 << 3);
	}
	uint32_t divx = 0;

	#define MCR_VAL(x_res, lines, col_shift)  ((((x_res/16)<<col_shift) << 22) | (lines << 10) | ((x_res/16)<<col_shift))

	switch(scrMode) {
	case SCR_640x256:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_108_125_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->pregs->spr_ctrl |= (0x01 << 2 | 0x02 << 4);
		par->mregs->div_y_cr  = (1UL << 13) | (1UL << 12);
		par->mregs->disp_mcr  = MCR_VAL(640, 256, shift);
		divx             = (1UL << 6);
		par->pregs->disp_h_act   = 1280UL;
		par->pregs->disp_h_blank = 1280UL << 12 | 1688UL;
		par->pregs->disp_h_sync  = 1328UL << 12 | 1440UL;
		par->pregs->disp_v_act   = 1024UL;
		par->pregs->disp_v_blank = 1024UL << 12 | 1066UL;
		par->pregs->disp_v_sync  = 1025UL << 12 | 1028UL;
		par->pregs->disp_vh_max  = 1066UL << 12 | 1688UL;
		break;

	case SCR_640x512:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_108_125_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->pregs->spr_ctrl |= (0x01 << 2 | 0x01 << 4);
		par->mregs->div_y_cr  = (1UL << 13);
		par->mregs->disp_mcr  = MCR_VAL(640, 512, shift);
		divx             = (1UL << 6);
		par->pregs->disp_h_act   = 1280UL;
		par->pregs->disp_h_blank = 1280UL << 12 | 1688UL;
		par->pregs->disp_h_sync  = 1328UL << 12 | 1440UL;
		par->pregs->disp_v_act   = 1024UL;
		par->pregs->disp_v_blank = 1024UL << 12 | 1066UL;
		par->pregs->disp_v_sync  = 1025UL << 12 | 1028UL;
		par->pregs->disp_vh_max  = 1066UL << 12 | 1688UL;
		break;

	case SCR_320x240:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_31_5_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->pregs->spr_ctrl |= (0x01 << 2 | 0x01 << 4);
		par->mregs->div_y_cr  = (1UL << 13);
		par->mregs->disp_mcr  = MCR_VAL(320, 240, shift);
		divx             = (1UL << 6);
		par->pregs->disp_h_act   = 640UL;
		par->pregs->disp_h_blank = 640UL << 12 | 840UL;
		par->pregs->disp_h_sync  = 656UL << 12 | 720UL;
		par->pregs->disp_v_act   = 480UL;
		par->pregs->disp_v_blank = 480UL << 12 | 500UL;
		par->pregs->disp_v_sync  = 481UL << 12 | 484UL;
		par->pregs->disp_vh_max  = 500UL << 12 | 840UL;
		break;
	
	case SCR_640x480:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_31_5_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->mregs->div_y_cr  =  0;
		par->mregs->disp_mcr  = MCR_VAL(640, 480, shift);
		par->pregs->disp_h_act   = 640UL;
		par->pregs->disp_h_blank = 640UL << 12 | 840UL;
		par->pregs->disp_h_sync  = 656UL << 12 | 720UL;
		par->pregs->disp_v_act   = 480UL;
		par->pregs->disp_v_blank = 480UL << 12 | 500UL;
		par->pregs->disp_v_sync  = 481UL << 12 | 484UL;
		par->pregs->disp_vh_max  = 500UL << 12 | 840UL;
		break;

	case SCR_800x600:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_40_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->mregs->div_y_cr  = 0;
		par->mregs->disp_mcr  = MCR_VAL(800, 600, shift);
		par->pregs->disp_h_act   = 800UL;
		par->pregs->disp_h_blank = 800UL << 12 | 1056UL;
		par->pregs->disp_h_sync  = 840UL << 12 | 968UL;
		par->pregs->disp_v_act   = 600UL;
		par->pregs->disp_v_blank = 600UL << 12 | 628UL;
		par->pregs->disp_v_sync  = 601UL << 12 | 605UL;
		par->pregs->disp_vh_max  = 628UL << 12 | 1056UL;
		break;

	case SCR_400x300:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_40_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->pregs->spr_ctrl |= (0x01 << 2 | 0x01 << 4);
		par->mregs->div_y_cr  = (1UL << 13);
		par->mregs->disp_mcr  = MCR_VAL(400, 300, shift);
		divx                = (1UL << 6);
		par->pregs->disp_h_act   = 800UL;
		par->pregs->disp_h_blank = 800UL << 12 | 1056UL;
		par->pregs->disp_h_sync  = 840UL << 12 | 968UL;
		par->pregs->disp_v_act   = 600UL;
		par->pregs->disp_v_blank = 600UL << 12 | 628UL;
		par->pregs->disp_v_sync  = 601UL << 12 | 605UL;
		par->pregs->disp_vh_max  = 628UL << 12 | 1056UL;
		break;

	case SCR_1024x768:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_65_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->mregs->div_y_cr  =  0;
		par->mregs->disp_mcr  = MCR_VAL(1024, 768, shift);
		par->pregs->disp_h_act   = 1024UL;
		par->pregs->disp_h_blank = 1024UL << 12 | 1344UL;
		par->pregs->disp_h_sync  = 1048UL << 12 | 1184UL;
		par->pregs->disp_v_act   = 768UL;
		par->pregs->disp_v_blank = 768UL << 12 | 806UL;
		par->pregs->disp_v_sync  = 771UL << 12 | 777UL;
		par->pregs->disp_vh_max  = 806UL << 12 | 1344UL;
		break;

	case SCR_1280x720:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_70_219_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->mregs->div_y_cr  = 0;
		par->mregs->disp_mcr  = MCR_VAL(1280, 720, shift);
		par->pregs->disp_h_act   = 1280UL;
		par->pregs->disp_h_blank = 1280UL << 12 | 1650UL;
		par->pregs->disp_h_sync  = 1390UL << 12 | 1430UL;
		par->pregs->disp_v_act   = 720UL;
		par->pregs->disp_v_blank = 720UL << 12 | 750UL;
		par->pregs->disp_v_sync  = 725UL << 12 | 730UL;
		par->pregs->disp_vh_max  = 750UL << 12 | 1650UL;
		break;

	case SCR_1280x1024:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_108_125_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->mregs->div_y_cr  = 0;
		par->mregs->disp_mcr  = MCR_VAL(1280, 1024, shift);
		par->pregs->disp_h_act   = 1280UL;
		par->pregs->disp_h_blank = 1280UL << 12 | 1688UL;
		par->pregs->disp_h_sync  = 1328UL << 12 | 1440UL;
		par->pregs->disp_v_act   = 1024UL;
		par->pregs->disp_v_blank = 1024UL << 12 | 1066UL;
		par->pregs->disp_v_sync  = 1025UL << 12 | 1028UL;
		par->pregs->disp_vh_max  = 1066UL << 12 | 1688UL;
		break;

	case SCR_1920x1080:
		clkReconfigured = configurePixelClock(info, WARP_PIXCLK_123_75_MHZ);
		// set sprite size multiplier (according to resolution divs)
		par->pregs->spr_ctrl &= ~(0x0f << 2);
		par->mregs->div_y_cr  = 0;
		par->mregs->disp_mcr  = MCR_VAL(1920, 1080, shift);
		par->pregs->disp_h_act   = 1920UL;
		par->pregs->disp_h_blank = 1920UL << 12 | 2200UL;
		par->pregs->disp_h_sync  = 2008UL << 12 | 2052UL;
		par->pregs->disp_v_act   = 1080UL;
		par->pregs->disp_v_blank = 1080UL << 12 | 1125UL;
		par->pregs->disp_v_sync  = 1084UL << 12 | 1089UL;
		par->pregs->disp_vh_max  = 1125UL << 12 | 2200UL;
		break;

	default:
		fb_err(info, "unsupported display mode\n");
		break;
	}
	// set color mode and reset timing-gen if required
	uint32_t rtgEnable = (1U << 7);
	if(clkReconfigured) {
		// reset timing-gen
		par->pregs->disp_cr = rtgEnable | col | divx | 0x04;
	}
	par->pregs->disp_cr = rtgEnable | col | divx;

	#undef MCR_VAL
}



    /*
     *  Setting the video mode has been split into two parts.
     *  First part, xxxfb_check_var, must not write anything
     *  to hardware, it should only verify and adjust var.
     *  This means it doesn't alter par but it does use hardware
     *  data from it to check this var.
     */

static int warpfb_check_var(struct fb_var_screeninfo *var,
			 struct fb_info *info)
{
	u_long line_length;

	// set actual resolution, according to what modes are available
	// if requested resolution is not found, there will be fallback to
	// 640x480
	CSScrMode mode = getCSGfxMode(var);
	switch(mode) {
	case SCR_640x480:
		var->xres = 640;
		var->yres = 480;
		break;
	case SCR_800x600:
		var->xres = 800;
		var->yres = 600;
		break;
	case SCR_1024x768:
		var->xres = 1024;
		var->yres = 768;
		break;
	case SCR_1280x720:
		var->xres = 1280;
		var->yres = 720;
		break;
	case SCR_1280x1024:
		var->xres = 1280;
		var->yres = 1024;
		break;
	default:
		var->xres = 640;
		var->yres = 480;
		break;
	}

	/*
	 *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
	 *  as FB_VMODE_SMOOTH_XPAN is only used internally
	 */
	if (var->vmode & FB_VMODE_CONUPDATE) {
		var->xoffset = info->var.xoffset;
		var->yoffset = info->var.yoffset;
	}

	/*
	 *  Some checks
	 */
	if (var->xres > var->xres_virtual)
		var->xres_virtual = var->xres;
	if (var->yres > var->yres_virtual)
		var->yres_virtual = var->yres;

	if (var->bits_per_pixel <= 8)
		var->bits_per_pixel = 8;
	else if (var->bits_per_pixel <= 16)
		var->bits_per_pixel = 16;
	else if (var->bits_per_pixel <= 32)
		var->bits_per_pixel = 32;
	else
		return -EINVAL;

	if (var->xres_virtual < var->xoffset + var->xres)
		var->xres_virtual = var->xoffset + var->xres;
	if (var->yres_virtual < var->yoffset + var->yres)
		var->yres_virtual = var->yoffset + var->yres;

	/*
	 *  Memory limit
	 */
	line_length = get_line_length(var->xres_virtual, var->bits_per_pixel);
	if (line_length * var->yres_virtual > videomemorysize)
		return -ENOMEM;

	/*
	 * Now that we checked it we alter var. The reason being is that the video
	 * mode passed in might not work but slight changes to it might make it
	 * work. This way we let the user know what is acceptable.
	 */
	switch (var->bits_per_pixel) {
	case 1:
	case 8:
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 0;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 16:
		/* RGB 565 */
		var->red.offset = 11;
		var->red.length = 5;
		var->green.offset = 5;
		var->green.length = 6;
		var->blue.offset = 0;
		var->blue.length = 5;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 24:		/* RGB 888 */
		var->red.offset = 8;
		var->green.offset = 16;
		var->blue.offset = 24;
		var->transp.offset = 0;
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->transp.length = 0;
		break;
	case 32:		/* RGBA 8888 */
		var->red.offset = 8;
		var->green.offset = 16;
		var->blue.offset = 24;
		var->transp.offset = 0;
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->transp.length = 0;
		break;
	}
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;

	return 0;
}

/* This routine actually sets the video mode. It's in here where we
 * the hardware state info->par and fix which can be affected by the
 * change in par. For this driver it doesn't do much.
 */
static int warpfb_set_par(struct fb_info *info)
{
	switch (info->var.bits_per_pixel) {
	case 1:
		info->fix.visual = FB_VISUAL_MONO01;
		break;
	case 8:
		info->fix.visual = FB_VISUAL_PSEUDOCOLOR;
		break;
	case 16:
	case 24:
	case 32:
		info->fix.visual = FB_VISUAL_TRUECOLOR;
		break;
	}

	info->fix.line_length = get_line_length(info->var.xres_virtual,
						info->var.bits_per_pixel);

	WarpFBPrivData *par = (WarpFBPrivData*)info->par;
	// set rtg video source
    par->pregs->disp_cr |= (1U << 7);
	// disable sprite
	par->pregs->spr_ctrl &= ~(0x01);

	csInitDisplay(info, getCSGfxMode(&info->var), info->var.bits_per_pixel);

	return 0;
}

    /*
     *  Set a single color register. The values supplied are already
     *  rounded down to the hardware's capabilities (according to the
     *  entries in the var structure). Return != 0 for invalid regno.
     */

static int warpfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info)
{
	if (regno >= 256)	/* no. of hw registers */
		return 1;
	/*
	 * Program hardware... do anything you want with transp
	 */
	
	WarpFBPrivData *par = (WarpFBPrivData*)info->par;

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
		    (red * 77 + green * 151 + blue * 28) >> 8;
	}

	if(info->fix.visual == FB_VISUAL_PSEUDOCOLOR) {
		u32 *clut = (u32*)(par->regs_base + WARP_REGS_CLUT_OFFSET);
		red >>= 8;
		green >>= 8;
		blue >>= 8;
		clut[regno] = (red << 16) | (green << 8) | blue;
	}

	/* Directcolor:
	 *   var->{color}.offset contains start of bitfield
	 *   var->{color}.length contains length of bitfield
	 *   {hardwarespecific} contains width of RAMDAC
	 *   cmap[X] is programmed to (X << red.offset) | (X << green.offset) | (X << blue.offset)
	 *   RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Pseudocolor:
	 *    var->{color}.offset is 0 unless the palette index takes less than
	 *                        bits_per_pixel bits and is stored in the upper
	 *                        bits of the pixel value
	 *    var->{color}.length is set so that 1 << length is the number of available
	 *                        palette entries
	 *    cmap is not used
	 *    RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Truecolor:
	 *    does not use DAC. Usually 3 are present.
	 *    var->{color}.offset contains start of bitfield
	 *    var->{color}.length contains length of bitfield
	 *    cmap is programmed to (red << red.offset) | (green << green.offset) |
	 *                      (blue << blue.offset) | (transp << transp.offset)
	 *    RAMDAC does not exist
	 */
#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		red = CNVT_TOHW(red, 8);	/* expect 8 bit DAC */
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		u32 v;

		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
		switch (info->var.bits_per_pixel) {
		case 8:
			break;
		case 16:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		case 24:
		case 32:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		}
		return 0;
	}
	return 0;
}

    /*
     *  Pan the Display
     *
     *  This call looks only at xoffset, yoffset
     */

static int warpfb_pan_display(struct fb_var_screeninfo *var,
			   struct fb_info *info)
{
	stat_hw_pan_calls++;

	if (var->xoffset + info->var.xres > info->var.xres_virtual ||
		var->yoffset + info->var.yres > info->var.yres_virtual)
	{
		return -EINVAL;
	}
	
	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;

	// YWARP is not supported
	info->var.vmode &= ~FB_VMODE_YWRAP;

	// set panning in hardware
	WarpFBPrivData *par = (WarpFBPrivData*)info->par;

	u32 bppix = info->var.bits_per_pixel >> 3;
	u32 bpr = info->fix.line_length;
	u32 xWordOffsetRounded = (((var->xoffset * bppix) + 8) >> 4);

	// add xy offsets
	u32 dispWordOffset = (((u32)info->var.xoffset * bpr) >> 4) + xWordOffsetRounded;

	// set screen display start offset
	par->mregs->disp_addr = dispWordOffset;

	// set screen words-per-row
	u32 mcr = par->mregs->disp_mcr;
	mcr &= ~(0x3ffUL << 22);
	mcr |= ((bpr >> 4) << 22);
	par->mregs->disp_mcr = mcr;

	return 0;
}

/**
 * @brief draw filled rectangle
 * @param info framebuffer info
 * @param rect rectangle
 * @return none
*/
static void warpfb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	stat_hw_fill_calls++;

	if(rect->width == 0 || rect->height == 0) {
		return;
	}

	WarpFBPrivData *par = (WarpFBPrivData*)info->par;

	void *scrMem = info->screen_base;
	u32 bpr = info->fix.line_length;
	u32 x = rect->dx;
	u32 y = rect->dy;
	u32 w = rect->width;
	u32 h = rect->height;
	u32 color = rect->color;

	// wait if blitter busy
	while((par->mregs->blt_sr & 0x19) != 0);
	
	if(info->var.bits_per_pixel == 8) {
		par->mregs->blt_dst = (uint32_t)scrMem + bpr * y + x;
		par->mregs->blt_dst_xy = (((uint32_t)y)<<16) | (uint32_t)x;
		par->mregs->blt_src_xy = par->mregs->blt_dst_xy;
		par->mregs->blt_wh = (((uint32_t)h)<<16) | (uint32_t)w;
		par->mregs->blt_color = color & 0xff;
		par->mregs->blt_src_bpr = bpr;
		par->mregs->blt_dst_bpr = bpr;
		par->mregs->blt_cr = (1 << 1) | (0 << 2); // start-fill | color-format
	} else if(info->var.bits_per_pixel == 16) {
		par->mregs->blt_dst = (uint32_t)scrMem + bpr * y + (x<<1);
		par->mregs->blt_dst_xy = (((uint32_t)y)<<16) | (uint32_t)x;
		par->mregs->blt_src_xy = par->mregs->blt_dst_xy;
		par->mregs->blt_wh = (((uint32_t)h)<<16) | (uint32_t)w;
		par->mregs->blt_color = color & 0xffff;
		par->mregs->blt_src_bpr = bpr;
		par->mregs->blt_dst_bpr = bpr;
		par->mregs->blt_cr = (1 << 1) | (1 << 2); // start-fill | color-format
	} else {
		par->mregs->blt_dst = (uint32_t)scrMem + bpr * y + (x<<2);
		par->mregs->blt_dst_xy = (((uint32_t)y)<<16) | (uint32_t)x;
		par->mregs->blt_src_xy = par->mregs->blt_dst_xy;
		par->mregs->blt_wh = (((uint32_t)h)<<16) | (uint32_t)w;
		par->mregs->blt_color = color;
		par->mregs->blt_src_bpr = bpr;
		par->mregs->blt_dst_bpr = bpr;
		par->mregs->blt_cr = (1 << 1) | (2 << 2); // start-fill | color-format
	}
}

/**
 * @brief draw filled rectangle
 * @param info framebuffer info
 * @param region copy area region info
 * @return none
*/
static void warpfb_copyarea(struct fb_info *info, const struct fb_copyarea *region)
{
	stat_hw_copy_calls++;

	if(region->width == 0 || region->height == 0) {
		return;
	}
	WarpFBPrivData *par = (WarpFBPrivData*)info->par;

	void *scrMem = info->screen_base;
	u32 bpr = info->fix.line_length;
	u32 sx = region->sx;
	u32 sy = region->sy;
	u32 dx = region->dx;
	u32 dy = region->dy;
	u32 w = region->width;
	u32 h = region->height;

	// wait if blitter busy
	while((par->mregs->blt_sr & 0x19) != 0);

	if(info->var.bits_per_pixel == 8) {
		par->mregs->blt_src = (uint32_t)scrMem + sy * bpr + sx;
		par->mregs->blt_dst = (uint32_t)scrMem + dy * bpr + dx;
		par->mregs->blt_src_xy = (((uint32_t)sy)<<16) | (uint32_t)sx;
		par->mregs->blt_dst_xy = (((uint32_t)dy)<<16) | (uint32_t)dx;
		par->mregs->blt_wh = (((uint32_t)h)<<16) | (uint32_t)w;
		par->mregs->blt_src_bpr = bpr;
		par->mregs->blt_dst_bpr = bpr;
		par->mregs->blt_cr = (1) | (0 << 2); // start-copy | color-format
	} else if(info->var.bits_per_pixel == 16) {
		par->mregs->blt_src = (uint32_t)scrMem + sy * bpr + (sx<<1);
		par->mregs->blt_dst = (uint32_t)scrMem + dy * bpr + (dx<<1);
		par->mregs->blt_src_xy = (((uint32_t)sy)<<16) | (uint32_t)sx;
		par->mregs->blt_dst_xy = (((uint32_t)dy)<<16) | (uint32_t)dx;
		par->mregs->blt_wh = (((uint32_t)h)<<16) | (uint32_t)w;
		par->mregs->blt_src_bpr = bpr;
		par->mregs->blt_dst_bpr = bpr;
		par->mregs->blt_cr = (1) | (1 << 2); // start-copy | color-format
	} else {
		par->mregs->blt_src = (uint32_t)scrMem + sy * bpr + (sx<<2);
		par->mregs->blt_dst = (uint32_t)scrMem + dy * bpr + (dx<<2);
		par->mregs->blt_src_xy = (((uint32_t)sy)<<16) | (uint32_t)sx;
		par->mregs->blt_dst_xy = (((uint32_t)dy)<<16) | (uint32_t)dx;
		par->mregs->blt_wh = (((uint32_t)h)<<16) | (uint32_t)w;
		par->mregs->blt_src_bpr = bpr;
		par->mregs->blt_dst_bpr = bpr;
		par->mregs->blt_cr = (1) | (2 << 2); // start-copy | color-format
	}
}

#ifdef IMAGE_BLIT_SUPPORT
/**
 * @brief draw filled rectangle
 * @param info framebuffer info
 * @param region copy area region info
 * @return none
*/
static void warpfb_imageblt(struct fb_info *info, const struct fb_image *img)
{
	printk("amiwarpfb: warpfb_imageblt called, img: 0x%08lx\n", (ulong)img->data);
}
#endif

static int __init warpfb_setup(char *options)
{
	char *this_opt;

	if (!*options) {
		return 1;
	}

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;

		/* Test disable for backwards compatibility */
		if(!strcmp(this_opt, "disable"))
			printk("amiwarpfb: 'disable' option unhandled\n");
		else if(!strncmp(this_opt, "depth:", 6))
			depth_option = simple_strtoul(this_opt + 6, NULL, 0);
		else {
			mode_option = this_opt;
		}
	}
	return 1;
}

    /*
     *  Initialisation
     */

static int warpfb_probe(struct zorro_dev *z, const struct zorro_device_id *id)
{
	struct fb_info *info;
	unsigned int vramSizeAligned = PAGE_ALIGN(videomemorysize);
	int retval = -ENOMEM;

	info = framebuffer_alloc(sizeof(WarpFBPrivData), &z->dev);
	if (!info)
		goto err;

	retval = device_create_file(&z->dev, &dev_attr_stat_hw_fill_calls);
	if (retval)
		goto err1;

	retval = device_create_file(&z->dev, &dev_attr_stat_hw_copy_calls);
    if (retval) {
        device_remove_file(&z->dev, &dev_attr_stat_hw_fill_calls);
    	goto err1;
    }
	retval = device_create_file(&z->dev, &dev_attr_stat_hw_pan_calls);
    if (retval) {
        device_remove_file(&z->dev, &dev_attr_stat_hw_fill_calls);
        device_remove_file(&z->dev, &dev_attr_stat_hw_copy_calls);
    	goto err1;
    }

	info->fbops = &warpfb_ops;
	info->flags |= FBINFO_HWACCEL_COPYAREA |
              	   FBINFO_HWACCEL_FILLRECT;
#ifdef IMAGE_BLIT_SUPPORT
	info->flags |= FBINFO_HWACCEL_IMAGEBLIT;
#endif

	// Find Warp-CTRL Zorro device (card control registers)
	struct zorro_dev *zWarpCtrl = NULL;
	zWarpCtrl = zorro_find_device(ZORRO_PROD_CSLAB_WARP_CTRL, zWarpCtrl);
	if(zWarpCtrl == NULL) {
		dev_err(&z->dev, "amiwarpfb: Can't find Warp-CTRL device! \n");
		retval = -ENODEV;
		goto err1;
	}
	// Find Warp-VRAM Zorro device (video memory)
	struct zorro_dev *zWarpVRAM = NULL;
	zWarpVRAM = zorro_find_device(ZORRO_PROD_CSLAB_WARP_VRAM, zWarpVRAM);
	if(zWarpVRAM == NULL) {
		dev_err(&z->dev, "amiwarpfb: Can't find Warp-VRAM device! \n");
		retval = -ENODEV;
		goto err1;
	}
	// set driver private data
	WarpFBPrivData *par = (WarpFBPrivData*)info->par;
	par->regs_base 	= (void*)zorro_resource_start(zWarpCtrl);
	par->regs_size 	= zorro_resource_len(zWarpCtrl);
	par->vram_base 	= (void*)zorro_resource_start(zWarpVRAM);
	par->vram_size 	= vramSizeAligned;
	par->pregs		= (WarpRegs_pix*)((u32)par->regs_base + WARP_REGS_PIXC_OFFSET);
	par->bregs		= (WarpRegs_bclk*)((u32)par->regs_base + WARP_REGS_BCLK_OFFSET);
	par->mregs		= (WarpRegs_mclk*)((u32)par->regs_base + WARP_REGS_MCLK_OFFSET);
	par->clut		= (uint32_t*)((u32)par->regs_base + WARP_REGS_CLUT_OFFSET);

	if(mode_option) {
		fb_info(info, "searching mode for option: %s, depth: %lu\n", 
				mode_option, depth_option);
	}

	if (!fb_find_mode(&info->var, info, mode_option,
			  vid_modedb, NUM_TOTAL_MODES, &vid_modedb[DEF_MODE], depth_option)){
		fb_err(info, "Unable to find usable video mode.\n");
		retval = -EINVAL;
		goto err1;
	}
	fb_dbg(info, "amiwarpfb: warpfb_probe fb_find_mode done, xres: %lu, yres: %lu\n",
		   (ulong)info->var.xres, (ulong)info->var.yres);

	// set list of video modes
	fb_videomode_to_modelist(vid_modedb, NUM_TOTAL_MODES, &info->modelist);

	warpfb_fix.smem_start = (ulong)par->vram_base;
	warpfb_fix.smem_len = par->vram_size;
	warpfb_fix.mmio_start = (ulong)par->regs_base;
	warpfb_fix.mmio_len = par->regs_size;

	info->fix = warpfb_fix;
	info->pseudo_palette = par->pseudo_col;
	info->screen_base = ioremap_wt((ulong)par->vram_base, par->vram_size);

	retval = fb_alloc_cmap(&info->cmap, 256, 0);
	if (retval < 0)
		goto err1;

	retval = register_framebuffer(info);
	if (retval < 0)
		goto err2;

	warpfb_set_par(info);

	fb_info(info, "csWarp frame buffer device, %ldK of video memory at vram_phys_addr: 0x%08lx\n",
		videomemorysize >> 10, (ulong)par->vram_base);

	return 0;
err2:
	fb_dealloc_cmap(&info->cmap);
err1:
	framebuffer_release(info);
err:
	return retval;
}

static const struct zorro_device_id warpvid_devices[] = {
	{ ZORRO_PROD_CSLAB_WARP_VRAM },
	{ 0 }
};
MODULE_DEVICE_TABLE(zorro, warpvid_devices);

static struct zorro_driver warpfb_driver = {
	.name		= "amiwarpfb",
	.id_table	= warpvid_devices,
	.probe		= warpfb_probe,
};

static int __init warpfb_init(void)
{
	char *option = NULL;

	if (fb_get_options("amiwarpfb", &option))
		return -ENODEV;
	warpfb_setup(option);
	return zorro_register_driver(&warpfb_driver);
}

module_init(warpfb_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("frame buffer driver for CSWarp Video Hardware");

