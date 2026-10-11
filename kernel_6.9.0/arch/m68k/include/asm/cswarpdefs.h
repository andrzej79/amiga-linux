/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CS-Lab Warp turbo board - FPGA register map (68k side).
 *
 * Mirrors csWarpAmigaSoftware common/WarpDefs.h and common/WarpRegisters.h
 * (firmware 3.000).  Register offsets are relative to the base of the Warp
 * CTRL Zorro board (ZORRO_PROD_CSLAB_WARP_CTRL).  Every register is a 32-bit
 * big-endian word: access it with z_readl()/z_writel() (readl()/writel() are
 * little-endian on m68k).
 */
#ifndef _ASM_M68K_CSWARPDEFS_H
#define _ASM_M68K_CSWARPDEFS_H

#include <linux/bits.h>

/* Zorro IDs (see also ZORRO_PROD_CSLAB_* in zorro_ids.h) */
#define WARP_VID			0x1400
#define WARP_PID_DDR3			60
#define WARP_PID_VRAM			100
#define WARP_PID_CTRL			101
#define WARP_PID_XROM			102

/* VRAM board: autoconfigured as 32 MB, the first 24 MB are video memory */
#define WARP_VRAM_SIZE			(24 * 1024 * 1024)

/* Blocks inside the CTRL board */
#define WARP_REGS_MCLK_OFFSET		0x0000	/* display/blitter, memory clock */
#define WARP_REGS_PIXC_OFFSET		0x0100	/* timing generator, pixel clock */
#define WARP_REGS_BCLK_OFFSET		0x0200	/* pixel clock MMCM, vblank irq */
#define WARP_REGS_PATBUFF_OFFSET	0x0400	/* pattern buffer: +0 data, +4 address */
#define WARP_REGS_CLUT_OFFSET		0x0800	/* 256 x 0x00RRGGBB */
#define WARP_REGS_SPRBUFF_OFFSET	0x0C00	/* hardware sprite buffer */
#define WARP_OFFSET_DPREG_CR		0x1000	/* DPREG register file, see cswarpamicommdata.h */
#define WARP_OFFSET_DPRAM		0x2000	/* 4 KB command/reply frame */
#define WARP_OFFSET_DPRAM_ASYNC		0x3000	/* AHI window (not used by Linux) */
#define WARP_OFFSET_QSDMA		0x4000
#define WARP_OFFSET_SYSCFG		0x5000
#define WARP_OFFSET_ATA			0x6000	/* native IDE, 4-byte register stride */
#define WARP_ATA_CTL_OFFSET		0x1018	/* alt-status / device control (CS1, reg 6) */
#define WARP_ATA_WINDOW_SIZE		0x2000

/* ---- MCLK block (WarpRegs_mclk) ---------------------------------------- */
#define WARP_MCLK(n)			(WARP_REGS_MCLK_OFFSET + (n))
#define WARP_DISP_ADDR			WARP_MCLK(0x00)	/* 24 bit, 16-byte units */
#define WARP_DISP_MCR			WARP_MCLK(0x04)	/* 31-22 scr_wpr, 21-10 lines, 9-0 disp_wpr */
#define WARP_BLT_SRC			WARP_MCLK(0x08)
#define WARP_BLT_DST			WARP_MCLK(0x0C)
#define WARP_BLT_SRC_XY			WARP_MCLK(0x10)
#define WARP_BLT_DST_XY			WARP_MCLK(0x14)
#define WARP_BLT_WH			WARP_MCLK(0x18)
#define WARP_BLT_COLOR			WARP_MCLK(0x1C)
#define WARP_BLT_CR			WARP_MCLK(0x20)
#define WARP_BLT_SR			WARP_MCLK(0x24)
#define WARP_BLT_SRC_BPR		WARP_MCLK(0x28)	/* 14 bit */
#define WARP_BLT_DST_BPR		WARP_MCLK(0x2C)	/* 14 bit */
#define WARP_DIV_Y_CR			WARP_MCLK(0x30)
#define WARP_ARTIX_TEMP			WARP_MCLK(0x34)
#define WARP_GENLOCK_SR			WARP_MCLK(0x98)	/* read only */

#define WARP_DISP_MCR_VAL(scr_wpr, lines, disp_wpr) \
	((((u32)(scr_wpr) & 0x3ff) << 22) | (((u32)(lines) & 0xfff) << 10) | ((u32)(disp_wpr) & 0x3ff))

#define WARP_BLT_CR_COPY		BIT(0)
#define WARP_BLT_CR_FILL		BIT(1)
#define WARP_BLT_CR_FMT(f)		((u32)(f) << 2)	/* 0 = 8 bit, 1 = 16 bit, 2 = 32 bit */
#define WARP_BLT_SR_BUSY_MASK		0x19

/* ---- PIXC block (WarpRegs_pix), pixel clock domain ---------------------- */
/*
 * A PIXC access completes through a pixel clock handshake: while the video
 * MMCM is in reset or unlocked the 68k bus cycle never ends.  Only touch these
 * registers with PIXCLK_SR_LOCKED set.
 */
#define WARP_PIX(n)			(WARP_REGS_PIXC_OFFSET + (n))
#define WARP_DISP_H_ACT			WARP_PIX(0x00)
#define WARP_DISP_H_BLANK		WARP_PIX(0x04)	/* start << 12 | end (= htotal) */
#define WARP_DISP_H_SYNC		WARP_PIX(0x08)	/* start << 12 | end */
#define WARP_DISP_V_ACT			WARP_PIX(0x0C)
#define WARP_DISP_V_BLANK		WARP_PIX(0x10)
#define WARP_DISP_V_SYNC		WARP_PIX(0x14)
#define WARP_DISP_VH_MAX		WARP_PIX(0x18)	/* vtotal << 12 | htotal */
#define WARP_DISP_CR			WARP_PIX(0x1C)
#define WARP_SPR_POS_X			WARP_PIX(0x20)
#define WARP_SPR_POS_Y			WARP_PIX(0x24)
#define WARP_SPR_CTRL			WARP_PIX(0x28)
#define WARP_SPR_COL0			WARP_PIX(0x2C)
#define WARP_SPR_COL1			WARP_PIX(0x30)
#define WARP_SPR_COL2			WARP_PIX(0x34)

#define WARP_DISP_CR_VSYNC_NEG		BIT(0)
#define WARP_DISP_CR_HSYNC_NEG		BIT(1)
#define WARP_DISP_CR_TG_RESET		BIT(2)
#define WARP_DISP_CR_COLOR(c)		((u32)(c) << 3)	/* 0 = 8 bit CLUT, 1 = RGB565, 2 = 32 bit */
#define WARP_DISP_CR_DIVX_SEL		BIT(5)
#define WARP_DISP_CR_DIVX_EN		BIT(6)
#define WARP_DISP_CR_RTG		BIT(7)	/* 1 = RTG framebuffer, 0 = Amiga video (scandoubler) */

#define WARP_SPR_CTRL_EN		BIT(0)
#define WARP_SPR_CTRL_SCALE_MASK	(0x0f << 2)

#define WARP_TIMING(start, end)		((((u32)(start) & 0xfff) << 12) | ((u32)(end) & 0xfff))

/* ---- BCLK block (WarpRegs_bclk) ----------------------------------------- */
#define WARP_BCLK(n)			(WARP_REGS_BCLK_OFFSET + (n))
#define WARP_PIXCLK_SR			WARP_BCLK(0x00)
#define WARP_PIXCLK_CR			WARP_BCLK(0x04)
#define WARP_IRQ_SR			WARP_BCLK(0x08)	/* bit0 vblank latched, bit1 vblank live */
#define WARP_IRQ_CR			WARP_BCLK(0x0C)	/* bit0 vblank irq enable, write bit1 = clear bit0 of SR */
#define WARP_PIXCLK_DRP_ADDR		WARP_BCLK(0x10)	/* 6-0 */
#define WARP_PIXCLK_DRP_DATA		WARP_BCLK(0x14)	/* one access = one whole DRP cycle */

#define PIXCLK_SR_DRP_TIMEOUT		BIT(0)	/* last DRP access timed out */
#define PIXCLK_SR_LOCKED		BIT(1)	/* video MMCM locked */
#define PIXCLK_SR_DRP_IF		BIT(2)	/* FPGA has the DRP interface */
#define PIXCLK_CR_MMCM_RST		BIT(3)	/* hold the video MMCM in reset */

#define WARP_IRQ_SR_VBLANK		BIT(0)
#define WARP_IRQ_SR_VBLANK_LIVE		BIT(1)
#define WARP_IRQ_CR_VBLANK_EN		BIT(0)
#define WARP_IRQ_CR_VBLANK_CLR		BIT(1)

/* ---- SysCfg block -------------------------------------------------------- */
/*
 * Registers above +0x04 exist only from SYSCFG_ID_L2CFG on; on older images a
 * write there is never acknowledged and hangs the bus.  Linux only reads +0x00.
 */
#define WARP_SYSCFG_ID			(WARP_OFFSET_SYSCFG + 0x00)
#define WARP_SYSCFG_CCTRL		(WARP_OFFSET_SYSCFG + 0x04)
#define SYSCFG_ID_ORIGINAL		0x01234567
#define SYSCFG_ID_L2CFG			0x01234568
#define SYSCFG_ID_L2STATS		0x01234569	/* oldest image with the video changes we rely on */

/* Board types, as reported in the diagnostic frame (WarpBoardType.h) */
enum warp_board_type {
	WARP_BOARD_UNDEFINED = 0,
	WARP_BOARD_530,
	WARP_BOARD_540,
	WARP_BOARD_560,
	WARP_BOARD_1230,
	WARP_BOARD_1240,
	WARP_BOARD_1260,
	WARP_BOARD_7030,
	WARP_BOARD_7040,
	WARP_BOARD_7060,
	WARP_BOARD_CDTV30,
	WARP_BOARD_CDTV40,
	WARP_BOARD_CDTV60,
	WARP_BOARD_1260X,
};

#endif /* _ASM_M68K_CSWARPDEFS_H */
