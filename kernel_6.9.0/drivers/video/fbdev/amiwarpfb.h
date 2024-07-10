#ifndef AMIWARPFB_H
#define AMIWARPFB_H

#include <asm/cswarpdefs.h>

typedef struct {
  uint32_t      pseudo_col[16];
  void          *regs_base;
  uint32_t      regs_size;
  void          *vram_base;
  uint32_t      vram_size;
  WarpRegs_pix  *pregs;
  WarpRegs_mclk *mregs;
  WarpRegs_bclk *bregs;
  uint32_t      *clut;
} WarpFBPrivData;


static inline CSScrMode getCSGfxMode(struct fb_var_screeninfo *var)
{
  CSScrMode mode = SCR_640x480;

  if (var->xres == 640 && var->yres == 480)
  {
    mode = SCR_640x480;
  }
  if (var->xres == 800 && var->yres == 600)
  {
    mode = SCR_800x600;
  }
  if (var->xres == 1024 && var->yres == 768)
  {
    mode = SCR_1024x768;
  }
  if (var->xres == 1280 && var->yres == 720)
  {
    mode = SCR_1280x720;
  }
  if (var->xres == 1280 && var->yres == 1024)
  {
    mode = SCR_1280x1024;
  }
  return mode;
}

#endif // AMIWARPFB_H
