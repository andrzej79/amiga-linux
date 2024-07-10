#ifndef CSWARPDEFS_H
#define CSWARPDEFS_H

#define WARP_VID                0x1400
#define WARP_PID_DDR3           60
#define WARP_PID_VRAM           100
#define WARP_PID_CTRL           101
#define WARP_PID_XROM           102
#define WARP_OFFSET_DPREG_CR    0x1000
#define WARP_OFFSET_DPRAM       0x2000
#define WARP_OFFSET_QSDMA       0x4000
#define WARP_OFFSET_SYSCFG      0x5000
#define WARP_OFFSET_ATA         0x6000
#define WARP_REGS_MCLK_OFFSET   0x0000
#define WARP_REGS_PIXC_OFFSET   0x0100
#define WARP_REGS_BCLK_OFFSET   0x0200
#define WARP_REGS_PATBUFF_OFFSET 0x0400 // 2kB pattern/template buffer 0-data, 4-addr
#define WARP_REGS_CLUT_OFFSET   0x0800
#define WARP_REGS_SPRBUFF_OFFSET 0x0C00 // hardware sprite buffer

#define PIXCLK_CR_S1  (0UL << 0)
#define PIXCLK_CR_S2  (1UL << 0)
#define PIXCLK_CR_S3  (2UL << 0)
#define PIXCLK_CR_S4  (3UL << 0)
#define PIXCLK_CR_S5  (4UL << 0)
#define PIXCLK_CR_S6  (5UL << 0)
#define PIXCLK_CR_S7  (6UL << 0)
#define PIXCLK_CR_S8  (7UL << 0)
#define PIXCLK_CR_S_MASK (7UL)
#define PIXCLK_CR_RST (1UL << 3)
#define PIXCLK_CR_RECFG (1UL << 4)

typedef enum {
  WARP_PIXCLK_31_5_MHZ,
  WARP_PIXCLK_40_MHZ,
  WARP_PIXCLK_65_MHZ,
  WARP_PIXCLK_70_219_MHZ,
  WARP_PIXCLK_108_125_MHZ,
  WARP_PIXCLK_123_75_MHZ,
} WarpPixClkType;

typedef enum
{
  // pseudo-amiga modes (derived from 1280x1024)
  SCR_320x256,
  SCR_320x512,
  SCR_640x256,
  SCR_640x512,
  SCR_1280x256,
  SCR_1280x512,
  SCR_320x240, // derived from 640x480
  SCR_640x480,
  SCR_400x300, // derived from 800x600
  SCR_800x600,
  SCR_1024x768,
  SCR_1280x720,
  SCR_1280x1024,
  SCR_1920x1080,
} CSScrMode;

// registers
typedef struct {
  volatile uint32_t   disp_addr;
  volatile uint32_t   disp_mcr;   // 31-22: scr_wpr ; 21-10: disp_lines ; 9-0: disp_wpr
  volatile uint32_t   blt_src;
  volatile uint32_t   blt_dst;
  volatile uint32_t   blt_src_xy;
  volatile uint32_t   blt_dst_xy;
  volatile uint32_t   blt_wh;
  volatile uint32_t   blt_color;
  volatile uint32_t   blt_cr;
  volatile uint32_t   blt_sr;
  volatile uint32_t   blt_src_bpr;  // 14 bits
  volatile uint32_t   blt_dst_bpr;  // 14 bits
  volatile uint32_t   div_y_cr;
  volatile uint32_t   artix_temp;
  // template/pattern blitter regs
  volatile uint32_t   patblt_cr;  // 4 bits, 0-start 1-no_bg 2-pattern_mode
  volatile uint32_t   pat_shift;  // 27-16 shift-y 3-0 shift-x
  volatile uint32_t   pat_bpr;    // 8 bits
  volatile uint32_t   pat_width;  // 12 bits
  volatile uint32_t   pat_height; // 12 bits
  volatile uint32_t   pat_bg;     // 24 bits
  volatile uint32_t   pat_fg;     // 24 bits
  volatile uint32_t   dst_base;   // 24 bits (A27 - A4)
  volatile uint32_t   dst_bpr;    // 13 bits
  volatile uint32_t   dst_bpp;    // 3 bits
  volatile uint32_t   dst_x;      // 12 bits
  volatile uint32_t   dst_y;      // 12 bits
  volatile uint32_t   patternHeight; // 12 bits
  // Hardware Line draw regs
  volatile uint32_t   hwl_cr;     // 4 bits, 0-start, 1:2-col_fmt, 3-fg only(no bg)
  volatile uint32_t   hwl_base_addr; // 24 bits
  volatile uint32_t   hwl_pitch;  // 12 bits
  volatile uint32_t   hwl_x0;     // 12 bits
  volatile uint32_t   hwl_y0;     // 12 bits
  volatile uint32_t   hwl_x1;     // 12 bits
  volatile uint32_t   hwl_y1;     // 12 bits
  volatile uint32_t   hwl_fg;     // 24 bits
  volatile uint32_t   hwl_bg;     // 24 bits
  volatile uint32_t   hwl_pat;    // 16 bits
  volatile uint32_t   hwl_pat_sh; // 4 bits
} WarpRegs_mclk;

typedef struct {
  volatile uint32_t   disp_h_act;
  volatile uint32_t   disp_h_blank;
  volatile uint32_t   disp_h_sync;
  volatile uint32_t   disp_v_act;
  volatile uint32_t   disp_v_blank;
  volatile uint32_t   disp_v_sync;
  volatile uint32_t   disp_vh_max;
  volatile uint32_t   disp_cr;    // 0:vsync_neg, 1:hsync_neg, 2:vclk_rst, 3-4:color_mode, 5:divx_sel, 6:divx_enable, 7:rtg_sd_switch
  volatile uint32_t   spr_pos_x;
  volatile uint32_t   spr_pos_y;
  volatile uint32_t   spr_ctrl;
  volatile uint32_t   spr_col0;
  volatile uint32_t   spr_col1;
  volatile uint32_t   spr_col2;
} WarpRegs_pix;

typedef struct {
  volatile uint32_t   pixclk_sr;
  volatile uint32_t   pixclk_cr;
  volatile uint32_t   irq_sr;
  volatile uint32_t   irq_cr;
} WarpRegs_bclk;

typedef struct {
  uint32_t    csr;
  uint32_t    memAddr;
  uint32_t    modulo;
  uint32_t    modInc;
  uint32_t    trNumb;
} WarpQSDMARegs;

typedef struct {
  uint32_t    res1;   // reserved, read 0x01234567
  uint32_t    cctrl;  // cache ctrl
} WarpSysCfgRegs;

#endif // CSWARPDEFS_H
