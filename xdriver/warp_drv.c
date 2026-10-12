// SPDX-License-Identifier: MIT
/*
 * Xorg video driver for the CS-Lab Warp RTG frame buffer (Linux fbdev).
 *
 * Copyright (C) 2026 Andrzej Rogozynski, CS-Lab s.c.
 * Based on xf86-video-fbdev (Alan Hourihane, Michel Dänzer).
 *
 * The Warp's VRAM is fast to write in bursts but slow to read, and mixed
 * reads and writes are worst of all (measured on a Warp560: move16 writes
 * ~52 MB/s, memcpy RAM->VRAM ~30 MB/s, CPU read-modify-write ~10 MB/s).  So:
 *
 *  - Everything is rendered by fb into a shadow buffer in system RAM; the CPU
 *    never reads VRAM.
 *  - The shadow is pushed to VRAM with move16, 16-byte aligned (the screen
 *    pitch is a multiple of 16, so every row is).
 *  - Copies between on-screen windows (scrolling, CopyArea, window moves) are
 *    done by the blitter in VRAM only, after pushing what is pending.  The
 *    shadow there is then stale; before software draws into or reads from a
 *    stale area it is refreshed from VRAM - except the 16-byte columns an
 *    opaque operation (ImageText, fills, PutImage, CopyArea from a pixmap)
 *    overwrites entirely.
 *  - Solid fills are queued and done by the blitter at the next push, just
 *    before the shadow goes out, so a clear and the text drawn over it reach
 *    the screen together.  Software drawing over a queued fill first gets
 *    that part filled into the shadow by the CPU (not at all where it
 *    overwrites whole columns).
 *  - The FPGA sprite (32x48, 3 colours) is the hardware cursor.
 *
 * Registers come from the kernel driver's MMIO mapping (fix.mmio_*): the
 * first page of the Warp CTRL board.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "xorg-server.h"
#include "xf86.h"
#include "xf86_OSproc.h"
#include "mipointer.h"
#include "micmap.h"
#include "colormapst.h"
#include "xf86cmap.h"
#include "shadow.h"
#include "fb.h"
#include "fbdevhw.h"
#include "xf86Cursor.h"
#include "damage.h"
#include "mi.h"
#include "gcstruct.h"
#include "windowstr.h"
#include "cursorstr.h"
#include "servermd.h"
#include "privates.h"
#include "dixfontstr.h"
#include "dixfont.h"

#define WARP_VERSION		1000
#define WARP_NAME		"WARP"
#define WARP_DRIVER_NAME	"warp"
#define WARP_MAJOR		0
#define WARP_MINOR		1
#define WARP_PATCH		0

/* ---- FPGA registers (offsets from the CTRL board, see asm/cswarpdefs.h) ---- */
#define REG_BLT_SRC		0x08
#define REG_BLT_DST		0x0C
#define REG_BLT_SRC_XY		0x10
#define REG_BLT_DST_XY		0x14
#define REG_BLT_WH		0x18
#define REG_BLT_COLOR		0x1C
#define REG_BLT_CR		0x20
#define REG_BLT_SR		0x24
#define REG_BLT_SRC_BPR		0x28
#define REG_BLT_DST_BPR		0x2C
#define REG_SPR_POS_X		0x120
#define REG_SPR_POS_Y		0x124
#define REG_SPR_CTRL		0x128
#define REG_SPR_COL0		0x12C
#define REG_SPR_COL1		0x130
#define REG_SPR_COL2		0x134
#define REG_SPR_BUFF		0xC00	/* +0 data (auto-increment), +4 address */
/* template blitter: 1-bit data expanded to fg/bg (blitter_pt.vhd) */
#define REG_PB_CR		0x38
#define REG_PB_SHIFT		0x3C	/* 3-0: x shift into the first word */
#define REG_PB_BPR		0x40	/* template bytes per row (even) */
#define REG_PB_WIDTH		0x44
#define REG_PB_HEIGHT		0x48
#define REG_PB_BG		0x4C
#define REG_PB_FG		0x50
#define REG_PB_DST_BASE		0x54	/* memory address >> 4 */
#define REG_PB_DST_BPR		0x58
#define REG_PB_DST_BPP		0x5C	/* bytes per pixel */
#define REG_PB_DST_X		0x60
#define REG_PB_DST_Y		0x64
#define REG_PB_PAT_H		0x68	/* pattern mode only */
#define REG_PATBUFF		0x400	/* +0 data (auto-increment), +4 long address */
#define PB_CR_START		(1u << 0)
#define PB_CR_NO_BG		(1u << 1)	/* JAM1: 0 bits leave VRAM alone */
#define PATBUFF_WORDS		1024	/* 2 KB, rows start on 16-bit words */
#define VRAM_MEM_BASE		0x0E000000u	/* VRAM in the board's memory map */

#define BLT_CR_COPY		(1u << 0)
#define BLT_CR_FILL		(1u << 1)
#define BLT_CR_FMT(f)		((uint32_t)(f) << 2)
#define BLT_SR_BUSY		0x19
#define SPR_CTRL_EN		(1u << 0)
#define SPR_CTRL_HIRES		(1u << 1)
#define SPR_W			32
#define SPR_H			48

/* -------------------------------------------------------------------- */

typedef struct {
	const GCFuncs *wrapFuncs;
	const GCOps *wrapOps;
} WarpGCPrivRec, *WarpGCPrivPtr;

typedef enum {
	OPTION_FBDEV,
	OPTION_ACCEL,
	OPTION_HWCURSOR,
	OPTION_MOVE16,
	OPTION_MINPIXELS,
	OPTION_DEFERFILLS,
	OPTION_VERIFY,
	OPTION_ACCELTEXT,
	OPTION_DEBUG
} WarpOpts;

static const OptionInfoRec WarpOptions[] = {
	{ OPTION_FBDEV,		"fbdev",	OPTV_STRING,	{0},	FALSE },
	{ OPTION_ACCEL,		"Accel",	OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_HWCURSOR,	"HWCursor",	OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_MOVE16,	"Move16",	OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_MINPIXELS,	"AccelMinPixels", OPTV_INTEGER,	{0},	FALSE },
	{ OPTION_DEFERFILLS,	"DeferFills",	OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_VERIFY,	"Verify",	OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_ACCELTEXT,	"AccelText",	OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_DEBUG,		"Debug",	OPTV_BOOLEAN,	{0},	FALSE },
	{ -1,			NULL,		OPTV_NONE,	{0},	FALSE }
};

/* a fill for the blitter to do at the next push */
typedef struct {
	BoxRec b;
	uint32_t pixel;
} WarpFill;

#define WARP_FILLQ	512

typedef struct {
	unsigned char *fbmem;		/* VRAM mapping */
	int fboff;
	unsigned char *fbstart;
	volatile uint8_t *mmio;		/* CTRL board registers, NULL = none */
	unsigned char *shadow;		/* system RAM copy of the screen */
	int pitch;			/* bytes per row, VRAM and shadow alike */
	int Bpp;
	uint32_t bltFmt;
	shadowBufPtr pBuf;		/* learnt from the first shadow update */

	Bool accel, hwcursor, move16, deferFills, verify, debug;
	int width, height;		/* visible screen, for Verify */
	int verifyRow;			/* next band Verify checks */
	Bool textAccel;
	uint32_t pbDstBase;		/* screen start for the template blitter */
	uint16_t tmpl[PATBUFF_WORDS + 8];	/* template being built */
	int minPixels;

	OptionInfoPtr Options;
	EntityInfoPtr pEnt;
	xf86CursorInfoPtr cursorInfo;
	uint32_t sprCtrl;

	/*
	 * VRAM-authoritative area: the blitter changed it in VRAM only, the
	 * shadow there is stale until refreshed.  Kept in 16-byte columns.
	 */
	RegionRec stale;
	DamagePtr pDamagePre;		/* reports drawing before it happens */
	int inHw;			/* our own damage/source calls: no refresh */

	/*
	 * The drawing in progress overwrites every pixel of the whole 16-byte
	 * columns in opaqueIn: those need no refresh, and are right in the
	 * shadow afterwards.  opaqueDrawn: all it can touch (NULL: what damage
	 * reports).
	 */
	Bool inOpaque;
	RegionRec opaqueIn;
	RegionPtr opaqueDrawn;

	/*
	 * Deferred fills, done by the blitter at the next push, just before the
	 * shadow goes out, so they reach the screen together with what was
	 * drawn over them.  Until then pend (exact pixels) is where the shadow
	 * lacks them: software drawing or reading there first gets them filled
	 * in by the CPU, in queue order.  After the push what is left of pend
	 * becomes stale (VRAM-authoritative).
	 */
	WarpFill *fillq;
	int nFillq;
	RegionRec pend;

	ScreenBlockHandlerProcPtr BlockHandler;

	CloseScreenProcPtr CloseScreen;
	CreateScreenResourcesProcPtr CreateScreenResources;
	CreateGCProcPtr CreateGC;
	CopyWindowProcPtr CopyWindow;
	SourceValidateProcPtr SourceValidate;

	/* statistics, logged at exit */
	unsigned long nCopy, nFill, nCopyBoxes, nFillBoxes, nUpdates, nFlushes, nDebug;
	unsigned long nCreateGC, nValidateGC, nCopyAreaCalls, nFillCalls;
	unsigned long nRefresh, nStaleSkip, nOpaque, nMaterialize, nFillFlush;
	unsigned long long materializeBytes;
	unsigned long nVerify, nVerifyBad;
	unsigned long nText, nTextBlits, nTextSoft, nTextChecked, nTextBad;
	unsigned long long verifyBadPixels;
	unsigned long long copyPixels, fillPixels, updateBytes, refreshBytes;
	unsigned long long usWait, usRefresh, usPush;	/* time spent, microseconds */
	/* time in our entry points (with what they call), microseconds */
	unsigned long long usCopyArea, usFillRect, usCopyWindow, usText, usBlock;
	unsigned long maxStale;				/* most boxes in stale */
} WarpRec, *WarpPtr;

#define WARPPTR(p) ((WarpPtr)((p)->driverPrivate))

static DevPrivateKeyRec warpGCPrivKeyRec;
#define WARP_GC_PRIV(gc) \
	((WarpGCPrivPtr)dixLookupPrivate(&(gc)->devPrivates, &warpGCPrivKeyRec))

static const OptionInfoRec *WarpAvailableOptions(int chipid, int busid);
static void WarpIdentify(int flags);
static Bool WarpProbe(DriverPtr drv, int flags);
static Bool WarpPreInit(ScrnInfoPtr pScrn, int flags);
static Bool WarpScreenInit(ScreenPtr pScreen, int argc, char **argv);
static Bool WarpCloseScreen(ScreenPtr pScreen);
static Bool WarpDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op, void *ptr);

_X_EXPORT DriverRec WARP = {
	.driverVersion	= WARP_VERSION,
	.driverName	= WARP_DRIVER_NAME,
	.Identify	= WarpIdentify,
	.Probe		= WarpProbe,
	.AvailableOptions = WarpAvailableOptions,
	.module		= NULL,
	.refCount	= 0,
	.driverFunc	= WarpDriverFunc,
};

static SymTabRec WarpChipsets[] = {
	{ 0, "warp" },
	{ -1, NULL }
};

static MODULESETUPPROTO(WarpSetup);

static XF86ModuleVersionInfo WarpVersRec = {
	"warp",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	WARP_MAJOR, WARP_MINOR, WARP_PATCH,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	MOD_CLASS_VIDEODRV,
	{ 0, 0, 0, 0 }
};

_X_EXPORT XF86ModuleData warpModuleData = { &WarpVersRec, WarpSetup, NULL };

static void *
WarpSetup(void *module, void *opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (!setupDone) {
		setupDone = TRUE;
		xf86AddDriver(&WARP, module, HaveDriverFuncs);
		return (void *)1;
	}
	if (errmaj)
		*errmaj = LDR_ONCEONLY;
	return NULL;
}

/* -------------------------------------------------------------------- */
/* Hardware                                                             */
/* -------------------------------------------------------------------- */

static inline uint32_t
rd(WarpPtr w, unsigned off)
{
	return *(volatile uint32_t *)(w->mmio + off);
}

static inline void
wr(WarpPtr w, unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(w->mmio + off) = v;
}

static unsigned long long
warp_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* timing for the stats line costs a syscall each on m68k: only with Debug */
#define WARP_NOW(w)		((w)->debug ? warp_us() : 0)
#define WARP_SINCE(w, t0)	((w)->debug ? warp_us() - (t0) : 0)

static void
warp_wait_idle(WarpPtr w)
{
	unsigned long long t0;
	unsigned long i;

	if (!(rd(w, REG_BLT_SR) & BLT_SR_BUSY))
		return;
	t0 = WARP_NOW(w);
	for (i = 0; i < 20000000UL; i++)
		if (!(rd(w, REG_BLT_SR) & BLT_SR_BUSY)) {
			w->usWait += WARP_SINCE(w, t0);
			return;
		}
	ErrorF("warp: blitter stuck busy (SR 0x%08x)\n", (unsigned)rd(w, REG_BLT_SR));
}

/* the blitter takes VRAM addresses; only bits 24:0 (the offset) are used */
static inline uint32_t
vram_off(WarpPtr w, int x, int y)
{
	return w->fboff + y * w->pitch + x * w->Bpp;
}

static void
warp_hw_copy(WarpPtr w, int sx, int sy, int dx, int dy, int width, int height)
{
	warp_wait_idle(w);
	wr(w, REG_BLT_SRC, vram_off(w, sx, sy));
	wr(w, REG_BLT_DST, vram_off(w, dx, dy));
	wr(w, REG_BLT_SRC_XY, ((uint32_t)sy << 16) | (uint32_t)sx);
	wr(w, REG_BLT_DST_XY, ((uint32_t)dy << 16) | (uint32_t)dx);
	wr(w, REG_BLT_WH, ((uint32_t)height << 16) | (uint32_t)width);
	wr(w, REG_BLT_SRC_BPR, w->pitch);
	wr(w, REG_BLT_DST_BPR, w->pitch);
	wr(w, REG_BLT_CR, BLT_CR_COPY | w->bltFmt);
}

static void
warp_hw_fill(WarpPtr w, int x, int y, int width, int height, uint32_t pixel)
{
	warp_wait_idle(w);
	wr(w, REG_BLT_DST, vram_off(w, x, y));
	wr(w, REG_BLT_DST_XY, ((uint32_t)y << 16) | (uint32_t)x);
	wr(w, REG_BLT_SRC_XY, ((uint32_t)y << 16) | (uint32_t)x);
	wr(w, REG_BLT_WH, ((uint32_t)height << 16) | (uint32_t)width);
	wr(w, REG_BLT_COLOR, pixel);
	wr(w, REG_BLT_SRC_BPR, w->pitch);
	wr(w, REG_BLT_DST_BPR, w->pitch);
	wr(w, REG_BLT_CR, BLT_CR_FILL | w->bltFmt);
}

/* -------------------------------------------------------------------- */
/* Shadow <-> VRAM                                                      */
/* -------------------------------------------------------------------- */

static inline void
move16_copy(void *dst, const void *src, unsigned long len)
{
	unsigned long cnt = len >> 4;

	if (!cnt)
		return;
	__asm__ volatile("1: move16 (%0)+,(%1)+\n\t"
			 "subq.l #1,%2\n\t"
			 "bne.s 1b"
			 : "+a"(src), "+a"(dst), "+d"(cnt) : : "memory");
}

/* copy the rows of a box, already in 16-byte columns, between the buffers */
static void
warp_copy_rows(WarpPtr w, unsigned char *to, const unsigned char *from, const BoxRec *b)
{
	int xb1 = b->x1 * w->Bpp, xb2 = b->x2 * w->Bpp, y;

	if (xb2 > w->pitch)
		xb2 = w->pitch;
	for (y = b->y1; y < b->y2; y++) {
		unsigned long off = (unsigned long)y * w->pitch + xb1;

		if (w->move16)
			move16_copy(to + off, from + off, xb2 - xb1);
		else
			memcpy(to + off, from + off, xb2 - xb1);
	}
}

/*
 * reg in whole 16-byte columns (the granule of move16): widened to every
 * column it touches, or (inner) narrowed to the columns it covers entirely
 */
static void
warp_columns(WarpPtr w, RegionPtr dst, RegionPtr src, Bool inner)
{
	int px = 16 / w->Bpp, n = RegionNumRects(src), m = 0;
	int xmax = w->pitch / w->Bpp;
	BoxPtr b = RegionRects(src), nb;
	BoxRec local[32];

	nb = n <= 32 ? local : malloc(n * sizeof(BoxRec));
	if (!n || !nb) {
		RegionNull(dst);
		if (n && !inner)	/* no memory: whole rows, still a superset */
			while (n--) {
				BoxRec r = { 0, b->y1, xmax, b->y2 };
				RegionRec t;

				RegionInit(&t, &r, 1);
				RegionUnion(dst, dst, &t);
				RegionUninit(&t);
				b++;
			}
		return;
	}
	while (n--) {
		int x1 = inner ? (b->x1 + px - 1) & ~(px - 1) : b->x1 & ~(px - 1);
		int x2 = inner ? b->x2 & ~(px - 1) : (b->x2 + px - 1) & ~(px - 1);

		if (x1 < 0)
			x1 = 0;
		if (x2 > xmax)
			x2 = xmax;
		if (x1 < x2) {
			nb[m].x1 = x1;
			nb[m].x2 = x2;
			nb[m].y1 = b->y1;
			nb[m].y2 = b->y2;
			m++;
		}
		b++;
	}
	if (!m)
		RegionNull(dst);
	else if (m == 1)
		RegionInit(dst, nb, 1);
	else if (!RegionInitBoxes(dst, nb, m))
		RegionNull(dst);
	if (nb != local)
		free(nb);
}

#define warp_widen(w, dst, src)	warp_columns(w, dst, src, FALSE)

/*
 * Bring the stale part of reg back into the shadow (pure VRAM reads), except
 * skip: columns the coming drawing overwrites entirely.
 */
static void
warp_refresh(WarpPtr w, RegionPtr reg, RegionPtr skip)
{
	RegionRec need;
	unsigned long long t0;
	int n;
	BoxPtr b;

	if (!RegionNotEmpty(&w->stale))
		return;
	t0 = WARP_NOW(w);
	warp_widen(w, &need, reg);
	RegionIntersect(&need, &need, &w->stale);
	if (skip && RegionNotEmpty(&need))
		RegionSubtract(&need, &need, skip);
	if (RegionNotEmpty(&need)) {
		warp_wait_idle(w);
		n = RegionNumRects(&need);
		b = RegionRects(&need);
		while (n--) {
			warp_copy_rows(w, w->shadow, w->fbstart, b);
			w->refreshBytes += (unsigned long long)(b->x2 - b->x1) * w->Bpp * (b->y2 - b->y1);
			b++;
		}
		RegionSubtract(&w->stale, &w->stale, &need);
		w->nRefresh++;
	}
	RegionUninit(&need);
	w->usRefresh += WARP_SINCE(w, t0);
}

/* the whole stale area: before leaving the VT, when VRAM stops being ours */
static void
warp_refresh_all(WarpPtr w)
{
	RegionRec all;

	if (!RegionNotEmpty(&w->stale) || !w->mmio)
		return;
	RegionNull(&all);
	RegionCopy(&all, &w->stale);
	warp_refresh(w, &all, NULL);
	RegionUninit(&all);
}

/* a solid box in the shadow, by the CPU */
static void
warp_shadow_fill(WarpPtr w, const BoxRec *b, uint32_t pixel)
{
	int width = b->x2 - b->x1, y, i;

	for (y = b->y1; y < b->y2; y++) {
		unsigned char *row = w->shadow + (unsigned long)y * w->pitch + b->x1 * w->Bpp;

		switch (w->Bpp) {
		case 1:
			memset(row, pixel, width);
			break;
		case 2:
			for (i = 0; i < width; i++)
				((uint16_t *)row)[i] = pixel;
			break;
		default:
			for (i = 0; i < width; i++)
				((uint32_t *)row)[i] = pixel;
			break;
		}
	}
}

/*
 * Put the deferred fills into the shadow wherever reg (exact pixels) meets
 * pend, except skip: replayed by the CPU in queue order, so later fills win.
 */
static void
warp_materialize(WarpPtr w, RegionPtr reg, RegionPtr skip)
{
	RegionRec need;
	int i;

	if (!RegionNotEmpty(&w->pend))
		return;
	RegionNull(&need);
	RegionIntersect(&need, reg, &w->pend);
	if (skip && RegionNotEmpty(&need))
		RegionSubtract(&need, &need, skip);
	if (RegionNotEmpty(&need)) {
		for (i = 0; i < w->nFillq; i++) {
			WarpFill *f = &w->fillq[i];
			RegionRec t;
			BoxPtr b;
			int n;

			if (RegionContainsRect(&need, &f->b) == rgnOUT)
				continue;
			RegionInit(&t, &f->b, 1);
			RegionIntersect(&t, &t, &need);
			n = RegionNumRects(&t);
			b = RegionRects(&t);
			while (n--) {
				warp_shadow_fill(w, b, f->pixel);
				w->materializeBytes += (unsigned long long)(b->x2 - b->x1) * w->Bpp *
						       (b->y2 - b->y1);
				b++;
			}
			RegionUninit(&t);
		}
		RegionSubtract(&w->pend, &w->pend, &need);
		w->nMaterialize++;
	}
	RegionUninit(&need);
}

/*
 * Debugging (Option "Verify"): after a push, VRAM must equal the shadow
 * everywhere outside the stale area.  Each push checks the next 1/16 of the
 * screen's rows, so a lasting fault is found within 16 pushes.
 */
static void
warp_verify(WarpPtr w)
{
	int band = (w->height + 15) / 16;
	int y1 = w->verifyRow, y2 = min(y1 + band, w->height);
	BoxRec all = { 0, y1, w->width, y2 };
	RegionRec chk;
	unsigned long bad = 0;
	int n, y, x;
	BoxPtr b;

	w->verifyRow = y2 >= w->height ? 0 : y2;
	RegionInit(&chk, &all, 1);
	RegionSubtract(&chk, &chk, &w->stale);
	n = RegionNumRects(&chk);
	b = RegionRects(&chk);
	while (n--) {
		int xb1 = b->x1 * w->Bpp, len = (b->x2 - b->x1) * w->Bpp;

		for (y = b->y1; y < b->y2; y++) {
			unsigned long off = (unsigned long)y * w->pitch + xb1;

			if (!memcmp(w->fbstart + off, w->shadow + off, len))
				continue;
			for (x = 0; x < len; x += w->Bpp) {
				if (!memcmp(w->fbstart + off + x, w->shadow + off + x, w->Bpp))
					continue;
				if (!bad && w->nVerifyBad < 20)
					ErrorF("warp: verify %lu: VRAM != shadow at %d,%d "
					       "(box %d,%d-%d,%d)\n", w->nVerify, b->x1 + x / w->Bpp,
					       y, b->x1, b->y1, b->x2, b->y2);
				bad++;
			}
		}
		b++;
	}
	RegionUninit(&chk);
	w->nVerify++;
	if (bad) {
		w->nVerifyBad++;
		w->verifyBadPixels += bad;
		/* resynchronise the band, so one fault is not counted again and again */
		for (y = y1; y < y2; y++)
			memcpy(w->shadow + (unsigned long)y * w->pitch,
			       w->fbstart + (unsigned long)y * w->pitch, w->width * w->Bpp);
	}
}

/* push the shadow's damaged boxes to VRAM, widened to 16-byte columns */
static void
WarpShadowUpdate(ScreenPtr pScreen, shadowBufPtr pBuf)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);
	RegionPtr dmg = DamageRegion(pBuf->pDamage);
	RegionRec push;
	unsigned long long t0;
	int n;
	BoxPtr b;

	w->pBuf = pBuf;
	if (!pScrn->vtSema || (!RegionNotEmpty(dmg) && !w->nFillq))
		return;
	t0 = WARP_NOW(w);

	/*
	 * Deferred fills first, in order, except where software drew over them
	 * since: the push brings that, and filling it first would flash.
	 */
	if (w->nFillq) {
		int i;

		for (i = 0; i < w->nFillq; i++) {
			WarpFill *f = &w->fillq[i];
			RegionRec t;
			BoxPtr b;
			int n;

			switch (RegionContainsRect(dmg, &f->b)) {
			case rgnIN:
				continue;
			case rgnOUT:
				RegionInit(&t, &f->b, 1);
				break;
			default:
				RegionInit(&t, &f->b, 1);
				RegionSubtract(&t, &t, dmg);
				break;
			}
			n = RegionNumRects(&t);
			b = RegionRects(&t);
			while (n--) {
				warp_hw_fill(w, b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1, f->pixel);
				w->nFillBoxes++;
				w->fillPixels += (unsigned long long)(b->x2 - b->x1) * (b->y2 - b->y1);
				b++;
			}
			RegionUninit(&t);
		}
	}
	warp_widen(w, &push, dmg);

	/*
	 * Never push over the VRAM-authoritative area: what software drew
	 * there was refreshed first, so this only guards against a path that
	 * bypassed the pre-op damage report (counted as stale_skip).
	 */
	if (RegionNotEmpty(&w->stale)) {
		RegionRec clash;

		RegionNull(&clash);
		RegionIntersect(&clash, &push, &w->stale);
		if (RegionNotEmpty(&clash)) {
			w->nStaleSkip++;
			RegionSubtract(&push, &push, &w->stale);
		}
		RegionUninit(&clash);
	}

	if (w->nFillq) {
		/* the push's edge columns may hold fill pixels the shadow lacks */
		warp_materialize(w, &push, NULL);
		/* what is left exists in VRAM only, once the fills are done */
		if (RegionNotEmpty(&w->pend)) {
			RegionRec wide;

			warp_widen(w, &wide, &w->pend);
			RegionUnion(&w->stale, &w->stale, &wide);
			RegionUninit(&wide);
			if (RegionNumRects(&w->stale) > w->maxStale)
				w->maxStale = RegionNumRects(&w->stale);
		}
		w->nFillq = 0;
		RegionEmpty(&w->pend);
	}
	/* the blitter may still be writing VRAM, and the push must land after it */
	if (w->mmio)
		warp_wait_idle(w);

	w->nUpdates++;
	n = RegionNumRects(&push);
	b = RegionRects(&push);
	while (n--) {
		warp_copy_rows(w, w->fbstart, w->shadow, b);
		w->updateBytes += (unsigned long long)(b->x2 - b->x1) * w->Bpp * (b->y2 - b->y1);
		b++;
	}
	RegionUninit(&push);
	w->usPush += WARP_SINCE(w, t0);
	if (w->verify)
		warp_verify(w);
}

static void *
WarpWindowLinear(ScreenPtr pScreen, CARD32 row, CARD32 offset, int mode,
		 CARD32 *size, void *closure)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);

	if (!pScrn->vtSema)
		return NULL;
	*size = w->pitch;
	return w->fbstart + row * w->pitch + offset;
}

/* push everything pending, so VRAM is right everywhere */
static void
warp_flush(ScreenPtr pScreen, WarpPtr w)
{
	if (w->pBuf && (w->nFillq || RegionNotEmpty(DamageRegion(w->pBuf->pDamage)))) {
		WarpShadowUpdate(pScreen, w->pBuf);
		DamageEmpty(w->pBuf->pDamage);
		w->nFlushes++;
	}
}

/*
 * After a blitter operation on dst (VRAM only): the area becomes
 * VRAM-authoritative, everyone tracking damage is told, and the shadow's
 * pending push drops it.
 */
static void
warp_hw_done(ScreenPtr pScreen, WarpPtr w, RegionPtr dst)
{
	RegionRec wide;

	warp_widen(w, &wide, dst);
	RegionUnion(&w->stale, &w->stale, &wide);
	if (RegionNumRects(&w->stale) > w->maxStale)
		w->maxStale = RegionNumRects(&w->stale);
	w->inHw++;
	DamageDamageRegion(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, dst);
	w->inHw--;
	RegionSubtract(DamageRegion(w->pBuf->pDamage), DamageRegion(w->pBuf->pDamage), &wide);
	RegionUninit(&wide);
}

/* reported by damage before any drawing into the screen pixmap */
static void
WarpDamagePreReport(DamagePtr pDamage, RegionPtr pRegion, void *closure)
{
	ScreenPtr pScreen = closure;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);

	if (w->inHw || !pScrn->vtSema)
		return;
	/* VRAM first, then the deferred fills on top of it */
	if (w->inOpaque) {
		RegionPtr drawn = w->opaqueDrawn ? w->opaqueDrawn : pRegion;

		warp_refresh(w, drawn, &w->opaqueIn);
		warp_materialize(w, drawn, &w->opaqueIn);
	} else {
		warp_refresh(w, pRegion, NULL);
		warp_materialize(w, pRegion, NULL);
	}
}

/* the shadow is not the whole truth somewhere */
static inline Bool
warp_shadow_partial(WarpPtr w)
{
	return RegionNotEmpty(&w->stale) || RegionNotEmpty(&w->pend);
}

/*
 * Around a drawing that overwrites every pixel of exact (screen coordinates);
 * drawn: everything it can touch, or NULL to rely on damage's report.
 * Returns whether the matching warp_opaque_end() is due.
 */
/*
 * Before shadow's own block handler: deferred fills with no software drawing
 * pending would never be pushed otherwise.
 */
static void WarpBlockHandler(ScreenPtr pScreen, void *timeout);
static void WarpCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc);

static void
WarpBlockHandler_body(ScreenPtr pScreen, void *timeout)
{
	WarpPtr w = WARPPTR(xf86ScreenToScrn(pScreen));

	if (w->nFillq)
		warp_flush(pScreen, w);
	pScreen->BlockHandler = w->BlockHandler;
	(*pScreen->BlockHandler)(pScreen, timeout);
	w->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = WarpBlockHandler;	/* the timed one, below */
}

static void
WarpBlockHandler(ScreenPtr pScreen, void *timeout)
{
	WarpPtr w = WARPPTR(xf86ScreenToScrn(pScreen));
	unsigned long long t0 = WARP_NOW(w);

	WarpBlockHandler_body(pScreen, timeout);
	w->usBlock += WARP_SINCE(w, t0);
}

static Bool
warp_opaque_begin(WarpPtr w, RegionPtr exact, RegionPtr drawn)
{
	if (w->inOpaque || !warp_shadow_partial(w))
		return FALSE;
	warp_columns(w, &w->opaqueIn, exact, TRUE);
	w->opaqueDrawn = drawn;
	w->inOpaque = TRUE;
	w->nOpaque++;
	return TRUE;
}

static void
warp_opaque_end(WarpPtr w)
{
	/* fb has drawn those columns whole: the shadow is right there now */
	RegionSubtract(&w->stale, &w->stale, &w->opaqueIn);
	RegionSubtract(&w->pend, &w->pend, &w->opaqueIn);
	RegionUninit(&w->opaqueIn);
	w->opaqueDrawn = NULL;
	w->inOpaque = FALSE;
}

/* clamped to what a BoxRec holds */
static void
warp_box(BoxPtr b, int x1, int y1, int x2, int y2)
{
#define WARP_CLAMP(v) ((v) < -32768 ? -32768 : (v) > 32767 ? 32767 : (v))
	b->x1 = WARP_CLAMP(x1);
	b->y1 = WARP_CLAMP(y1);
	b->x2 = WARP_CLAMP(x2);
	b->y2 = WARP_CLAMP(y2);
#undef WARP_CLAMP
}

/* any read of the screen (GetImage, copies, Render sources) */
static void
WarpSourceValidate(DrawablePtr pDraw, int x, int y, int width, int height,
		   unsigned int subWindowMode)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);

	if (!w->inHw && pScrn->vtSema && warp_shadow_partial(w) &&
	    (pDraw->type == DRAWABLE_WINDOW ?
	     (*pScreen->GetWindowPixmap)((WindowPtr)pDraw) == (*pScreen->GetScreenPixmap)(pScreen) :
	     (PixmapPtr)pDraw == (*pScreen->GetScreenPixmap)(pScreen))) {
		BoxRec b = { pDraw->x + x, pDraw->y + y,
			     pDraw->x + x + width, pDraw->y + y + height };
		RegionRec r;

		RegionInit(&r, &b, 1);
		warp_refresh(w, &r, NULL);
		warp_materialize(w, &r, NULL);
		RegionUninit(&r);
	}
	if (w->SourceValidate) {
		pScreen->SourceValidate = w->SourceValidate;
		(*pScreen->SourceValidate)(pDraw, x, y, width, height, subWindowMode);
		w->SourceValidate = pScreen->SourceValidate;
		pScreen->SourceValidate = WarpSourceValidate;
	}
}

/* -------------------------------------------------------------------- */
/* Accelerated operations: VRAM only, by the blitter                    */
/* -------------------------------------------------------------------- */

static Bool
warp_on_screen(ScreenPtr pScreen, DrawablePtr pDraw)
{
	if (pDraw->type != DRAWABLE_WINDOW || pDraw->pScreen != pScreen)
		return FALSE;
	/* not redirected by Composite: it draws to the screen pixmap */
	return (*pScreen->GetWindowPixmap)((WindowPtr)pDraw) ==
	       (*pScreen->GetScreenPixmap)(pScreen);
}

static Bool
warp_accel_ok(ScrnInfoPtr pScrn, WarpPtr w)
{
	return w->accel && w->mmio && w->pBuf && pScrn->vtSema;
}

static Bool
warp_gc_plain(GCPtr pGC, DrawablePtr pDraw)
{
	FbBits full = FbFullMask(pDraw->depth);

	return pGC->alu == GXcopy && (pGC->planemask & full) == full;
}

/* worth working out what a drawing overwrites: only over stale areas */
static Bool
warp_opaque_ok(ScrnInfoPtr pScrn, WarpPtr w, GCPtr pGC, DrawablePtr pDraw)
{
	return w->accel && pScrn->vtSema && warp_shadow_partial(w) &&
	       warp_gc_plain(pGC, pDraw) && warp_on_screen(pGC->pScreen, pDraw);
}

/* the rectangles as drawn: in screen coordinates, clipped */
static RegionPtr
warp_rects_region(DrawablePtr pDraw, GCPtr pGC, int nrect, xRectangle *rects)
{
	RegionPtr rgn = RegionFromRects(nrect, rects, CT_UNSORTED);

	RegionTranslate(rgn, pDraw->x, pDraw->y);
	RegionIntersect(rgn, rgn, pGC->pCompositeClip);
	return rgn;
}

/*
 * miCopyProc: destination boxes in screen coordinates, source at
 * box + (dx, dy), already in an overlap-safe order.  The blitter reads each
 * source row whole into its FIFO before writing (blitter.vhd) and walks rows
 * bottom-up when the destination is lower, so each box is one operation.
 */
static void
warpCopyNtoN(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC, BoxPtr pbox, int nbox,
	     int dx, int dy, Bool reverse, Bool upsidedown, Pixel bitplane, void *closure)
{
	WarpPtr w = closure;
	ScreenPtr pScreen = pDst->pScreen;
	RegionRec dst;
	int i;

	RegionNull(&dst);
	for (i = 0; i < nbox; i++) {
		BoxPtr b = &pbox[i];
		RegionRec t;

		if (b->x2 <= b->x1 || b->y2 <= b->y1)
			continue;
		warp_hw_copy(w, b->x1 + dx, b->y1 + dy, b->x1, b->y1,
			     b->x2 - b->x1, b->y2 - b->y1);
		w->copyPixels += (unsigned long long)(b->x2 - b->x1) * (b->y2 - b->y1);
		RegionInit(&t, b, 1);
		RegionUnion(&dst, &dst, &t);
		RegionUninit(&t);
	}
	w->nCopyBoxes += nbox;
	if (RegionNotEmpty(&dst))
		warp_hw_done(pScreen, w, &dst);
	RegionUninit(&dst);
}

#define WARP_GC_OP_PROLOGUE(pGC) \
	WarpGCPrivPtr pGCPriv = WARP_GC_PRIV(pGC); \
	const GCFuncs *oldFuncs = pGC->funcs; \
	pGC->funcs = pGCPriv->wrapFuncs; \
	pGC->ops = pGCPriv->wrapOps

/*
 * Every GC op is wrapped (layers inside us - damage - put back their own ops
 * table after each op, so a partial table would be lost after the first op we
 * do not wrap).  The epilogue reinstalls ours.
 */
static const GCOps warpGCOps;

#define WARP_GC_OP_EPILOGUE(pGC) \
	pGCPriv->wrapFuncs = pGC->funcs; \
	pGC->funcs = oldFuncs; \
	pGCPriv->wrapOps = pGC->ops; \
	pGC->ops = &warpGCOps

static RegionPtr
WarpCopyArea_body(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,
	     int srcx, int srcy, int width, int height, int dstx, int dsty)
{
	ScreenPtr pScreen = pGC->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);
	RegionPtr ret;
	WARP_GC_OP_PROLOGUE(pGC);

	w->nCopyAreaCalls++;
	if (warp_accel_ok(pScrn, w) &&
	    width * height >= w->minPixels &&
	    warp_on_screen(pScreen, pSrc) && warp_on_screen(pScreen, pDst) &&
	    pSrc->bitsPerPixel == pDst->bitsPerPixel &&
	    warp_gc_plain(pGC, pDst)) {
		/* VRAM must be right where the source is */
		warp_flush(pScreen, w);
		/* mi does the clipping and the exposures; the blitter the pixels */
		w->inHw++;
		ret = miDoCopy(pSrc, pDst, pGC, srcx, srcy, width, height, dstx, dsty,
			       warpCopyNtoN, 0, w);
		w->inHw--;
		w->nCopy++;
	} else if (pSrc->type == DRAWABLE_PIXMAP && warp_opaque_ok(pScrn, w, pGC, pDst)) {
		/* from a pixmap (double buffering): all of it that lies in the pixmap */
		int sx1 = max(srcx, 0), sy1 = max(srcy, 0);
		int sx2 = min(srcx + width, (int)pSrc->width);
		int sy2 = min(srcy + height, (int)pSrc->height);
		Bool op = FALSE;
		RegionRec r;
		BoxRec b;

		RegionNull(&r);
		if (sx1 < sx2 && sy1 < sy2) {
			warp_box(&b, pDst->x + dstx + sx1 - srcx, pDst->y + dsty + sy1 - srcy,
				 pDst->x + dstx + sx2 - srcx, pDst->y + dsty + sy2 - srcy);
			RegionInit(&r, &b, 1);	/* r was only nulled */
			RegionIntersect(&r, &r, pGC->pCompositeClip);
			op = warp_opaque_begin(w, &r, &r);
		}
		ret = (*pGC->ops->CopyArea)(pSrc, pDst, pGC, srcx, srcy, width, height,
					    dstx, dsty);
		if (op)
			warp_opaque_end(w);
		RegionUninit(&r);
	} else {
		ret = (*pGC->ops->CopyArea)(pSrc, pDst, pGC, srcx, srcy, width, height,
					    dstx, dsty);
	}

	WARP_GC_OP_EPILOGUE(pGC);
	return ret;
}

static void
WarpPolyFillRect_body(DrawablePtr pDraw, GCPtr pGC, int nrect, xRectangle *rects)
{
	ScreenPtr pScreen = pGC->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);
	Bool accel = FALSE;
	WARP_GC_OP_PROLOGUE(pGC);

	w->nFillCalls++;
	if (warp_accel_ok(pScrn, w) && pGC->fillStyle == FillSolid &&
	    warp_gc_plain(pGC, pDraw) && warp_on_screen(pScreen, pDraw)) {
		long area = 0;
		int i;

		for (i = 0; i < nrect && area < w->minPixels; i++)
			area += (long)rects[i].width * rects[i].height;
		accel = area >= w->minPixels;
	}

	if (accel) {
		RegionPtr rgn = warp_rects_region(pDraw, pGC, nrect, rects);
		int n = RegionNumRects(rgn);

		if (n && w->deferFills && n <= WARP_FILLQ) {
			/*
			 * Queued for the push.  The shadow and VRAM keep the old
			 * pixels until then; damage listeners hear of it now.
			 */
			BoxPtr b = RegionRects(rgn);
			RegionRec inner;

			if (w->nFillq + n > WARP_FILLQ) {
				warp_flush(pScreen, w);
				w->nFillFlush++;
			}
			while (n--) {
				w->fillq[w->nFillq].b = *b++;
				w->fillq[w->nFillq].pixel = pGC->fgPixel;
				w->nFillq++;
			}
			RegionUnion(&w->pend, &w->pend, rgn);
			/* the fill decides those columns now, not VRAM */
			warp_columns(w, &inner, rgn, TRUE);
			RegionSubtract(&w->stale, &w->stale, &inner);
			RegionUninit(&inner);
			w->inHw++;
			DamageDamageRegion(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, rgn);
			w->inHw--;
			/* nothing of the shadow to push there any more */
			RegionSubtract(DamageRegion(w->pBuf->pDamage), DamageRegion(w->pBuf->pDamage),
				       rgn);
			w->nFill++;
		} else if (n) {
			BoxPtr b = RegionRects(rgn);

			warp_flush(pScreen, w);
			w->nFillBoxes += n;
			while (n--) {
				warp_hw_fill(w, b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1,
					     pGC->fgPixel);
				w->fillPixels += (unsigned long long)(b->x2 - b->x1) * (b->y2 - b->y1);
				b++;
			}
			warp_hw_done(pScreen, w, rgn);
			w->nFill++;
		}
		RegionDestroy(rgn);
	} else if (pGC->fillStyle != FillStippled && warp_opaque_ok(pScrn, w, pGC, pDraw)) {
		/* solid, tiled and opaque-stippled fills cover every pixel */
		RegionPtr rgn = warp_rects_region(pDraw, pGC, nrect, rects);
		Bool op = warp_opaque_begin(w, rgn, rgn);

		(*pGC->ops->PolyFillRect)(pDraw, pGC, nrect, rects);
		if (op)
			warp_opaque_end(w);
		RegionDestroy(rgn);
	} else {
		(*pGC->ops->PolyFillRect)(pDraw, pGC, nrect, rects);
	}

	WARP_GC_OP_EPILOGUE(pGC);
}

static void
WarpCopyWindow_body(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
{
	ScreenPtr pScreen = pWin->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);
	BoxPtr ext = RegionExtents(prgnSrc);

	if (warp_accel_ok(pScrn, w) && warp_on_screen(pScreen, &pWin->drawable) &&
	    (long)(ext->x2 - ext->x1) * (ext->y2 - ext->y1) >= w->minPixels) {
		/* as fbCopyWindow, with the blitter doing the copy */
		PixmapPtr pPix = (*pScreen->GetScreenPixmap)(pScreen);
		int dx = ptOldOrg.x - pWin->drawable.x;
		int dy = ptOldOrg.y - pWin->drawable.y;
		RegionRec rgnDst;

		RegionTranslate(prgnSrc, -dx, -dy);
		RegionNull(&rgnDst);
		RegionIntersect(&rgnDst, &pWin->borderClip, prgnSrc);
		warp_flush(pScreen, w);
		w->inHw++;
		miCopyRegion(&pPix->drawable, &pPix->drawable, NULL, &rgnDst, dx, dy,
			     warpCopyNtoN, 0, w);
		w->inHw--;
		RegionUninit(&rgnDst);
		w->nCopy++;
		return;
	}

	pScreen->CopyWindow = w->CopyWindow;
	(*pScreen->CopyWindow)(pWin, ptOldOrg, prgnSrc);
	w->CopyWindow = pScreen->CopyWindow;
	pScreen->CopyWindow = WarpCopyWindow;
}

/* ---- GC wrapping (as miext/damage does it) ---- */

static void WarpValidateGC(GCPtr, unsigned long, DrawablePtr);
static void WarpChangeGC(GCPtr, unsigned long);
static void WarpCopyGC(GCPtr, unsigned long, GCPtr);
static void WarpDestroyGC(GCPtr);
static void WarpChangeClip(GCPtr, int, void *, int);
static void WarpDestroyClip(GCPtr);
static void WarpCopyClip(GCPtr, GCPtr);

static const GCFuncs warpGCFuncs = {
	WarpValidateGC, WarpChangeGC, WarpCopyGC, WarpDestroyGC,
	WarpChangeClip, WarpDestroyClip, WarpCopyClip
};

/* ---- pass-through ops: only restore our table afterwards ---- */

#define WARP_PASS(ret, name, decl, call) \
static ret Warp##name decl \
{ \
	WARP_GC_OP_PROLOGUE(pGC); \
	(*pGC->ops->name) call; \
	WARP_GC_OP_EPILOGUE(pGC); \
}

#define WARP_PASS_RET(ret, name, decl, call) \
static ret Warp##name decl \
{ \
	ret r; \
	WARP_GC_OP_PROLOGUE(pGC); \
	r = (*pGC->ops->name) call; \
	WARP_GC_OP_EPILOGUE(pGC); \
	return r; \
}

WARP_PASS(void, FillSpans, (DrawablePtr d, GCPtr pGC, int n, DDXPointPtr p, int *w, int s),
	  (d, pGC, n, p, w, s))
WARP_PASS(void, SetSpans, (DrawablePtr d, GCPtr pGC, char *src, DDXPointPtr p, int *w, int n, int s),
	  (d, pGC, src, p, w, n, s))
WARP_PASS_RET(RegionPtr, CopyPlane, (DrawablePtr s, DrawablePtr d, GCPtr pGC, int sx, int sy,
				     int w, int h, int dx, int dy, unsigned long plane),
	      (s, d, pGC, sx, sy, w, h, dx, dy, plane))
WARP_PASS(void, PolyPoint, (DrawablePtr d, GCPtr pGC, int mode, int n, DDXPointPtr p),
	  (d, pGC, mode, n, p))
WARP_PASS(void, Polylines, (DrawablePtr d, GCPtr pGC, int mode, int n, DDXPointPtr p),
	  (d, pGC, mode, n, p))
WARP_PASS(void, PolySegment, (DrawablePtr d, GCPtr pGC, int n, xSegment *s), (d, pGC, n, s))
WARP_PASS(void, PolyRectangle, (DrawablePtr d, GCPtr pGC, int n, xRectangle *r), (d, pGC, n, r))
WARP_PASS(void, PolyArc, (DrawablePtr d, GCPtr pGC, int n, xArc *a), (d, pGC, n, a))
WARP_PASS(void, FillPolygon, (DrawablePtr d, GCPtr pGC, int shape, int mode, int n, DDXPointPtr p),
	  (d, pGC, shape, mode, n, p))
WARP_PASS(void, PolyFillArc, (DrawablePtr d, GCPtr pGC, int n, xArc *a), (d, pGC, n, a))
static int WarpPolyText8(DrawablePtr d, GCPtr pGC, int x, int y, int n, char *c);
static int WarpPolyText16(DrawablePtr d, GCPtr pGC, int x, int y, int n, unsigned short *c);
WARP_PASS(void, ImageGlyphBlt, (DrawablePtr d, GCPtr pGC, int x, int y, unsigned int n,
				CharInfoPtr *ci, void *base),
	  (d, pGC, x, y, n, ci, base))
WARP_PASS(void, PolyGlyphBlt, (DrawablePtr d, GCPtr pGC, int x, int y, unsigned int n,
			       CharInfoPtr *ci, void *base),
	  (d, pGC, x, y, n, ci, base))
WARP_PASS(void, PushPixels, (GCPtr pGC, PixmapPtr bm, DrawablePtr d, int w, int h, int x, int y),
	  (pGC, bm, d, w, h, x, y))

/* ---- ops that overwrite a known area: no refresh of what they cover ---- */

static void
WarpPutImage(DrawablePtr d, GCPtr pGC, int depth, int x, int y, int width, int height,
	     int lp, int fmt, char *bits)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pGC->pScreen);
	WarpPtr w = WARPPTR(pScrn);
	Bool op = FALSE;
	RegionRec r;
	WARP_GC_OP_PROLOGUE(pGC);

	RegionNull(&r);
	/* XYPixmap writes plane by plane: not opaque in one go */
	if ((fmt == ZPixmap || fmt == XYBitmap) && width > 0 && height > 0 &&
	    warp_opaque_ok(pScrn, w, pGC, d)) {
		BoxRec b;

		warp_box(&b, d->x + x, d->y + y, d->x + x + width, d->y + y + height);
		RegionInit(&r, &b, 1);	/* r was only nulled */
		RegionIntersect(&r, &r, pGC->pCompositeClip);
		op = warp_opaque_begin(w, &r, &r);
	}
	(*pGC->ops->PutImage)(d, pGC, depth, x, y, width, height, lp, fmt, bits);
	if (op)
		warp_opaque_end(w);
	RegionUninit(&r);
	WARP_GC_OP_EPILOGUE(pGC);
}

/*
 * The background box ImageText fills whole (fbImageGlyphBlt): the advance of
 * the glyphs found, font ascent to descent.  Glyph ink may stick out of it, so
 * damage's report (which includes the ink) stays what can be touched.
 */
static Bool
warp_text_box(DrawablePtr d, GCPtr pGC, int x, int y, int count, void *chars,
	      FontEncoding enc, RegionPtr out)
{
	CharInfoPtr ci[256];
	unsigned long n, i;
	int adv = 0;
	BoxRec b;

	if (count <= 0 || count > 255)
		return FALSE;
	GetGlyphs(pGC->font, count, chars, enc, &n, ci);
	for (i = 0; i < n; i++)
		adv += ci[i]->metrics.characterWidth;
	if (!adv || FONTASCENT(pGC->font) + FONTDESCENT(pGC->font) <= 0)
		return FALSE;
	warp_box(&b, d->x + x + min(adv, 0), d->y + y - FONTASCENT(pGC->font),
		 d->x + x + max(adv, 0), d->y + y + FONTDESCENT(pGC->font));
	RegionInit(out, &b, 1);	/* out was only nulled */
	RegionIntersect(out, out, pGC->pCompositeClip);
	return TRUE;
}

/* -------------------------------------------------------------------- */
/* Text by the template blitter                                         */
/* -------------------------------------------------------------------- */

/*
 * One chunk of a run: the glyphs ORed into a 1-bit template x1..x2 x
 * y1..y1+h (screen coordinates, x1 at bit 0), then one blit per clip box,
 * each loading the template from the word its corner falls in.
 */
static void
warp_text_chunk(WarpPtr w, GCPtr pGC, CharInfoPtr *ci, int n, int pen, int base,
		int x1, int x2, int y1, int h, Bool image, RegionPtr done)
{
	int words = (x2 - x1 + 15) >> 4, bpr = words * 2, i, r, k, nb;
	uint32_t mask = w->Bpp == 1 ? 0xff : 0xffff;
	RegionRec clip;
	BoxRec box;
	BoxPtr b;

	warp_box(&box, x1, y1, x2, y1 + h);
	RegionInit(&clip, &box, 1);
	RegionIntersect(&clip, &clip, pGC->pCompositeClip);
	if (!RegionNotEmpty(&clip)) {
		RegionUninit(&clip);
		return;
	}

	memset(w->tmpl, 0, bpr * h + 4);
	for (i = 0; i < n; i++) {
		CharInfoPtr c = ci[i];
		xCharInfo *m = &c->metrics;
		int gw = GLYPHWIDTHPIXELS(c), gh = GLYPHHEIGHTPIXELS(c);
		int stride = GLYPHWIDTHBYTESPADDED(c);
		int gx = pen + m->leftSideBearing - x1, gy = base - m->ascent - y1;
		const unsigned char *src = (const unsigned char *)c->bits;

		pen += m->characterWidth;
		if (gw <= 0 || gh <= 0)
			continue;
		for (r = 0; r < gh; r++, src += stride) {
			uint16_t *row = w->tmpl + (gy + r) * words;

			/* 16 glyph bits at a time; glyph rows are MSB first, like the template */
			for (k = 0; k < gw; k += 16) {
				int kb = k >> 3, pos = gx + k;
				uint32_t v = (uint32_t)src[kb] << 8 | (kb + 1 < stride ? src[kb + 1] : 0);

				if (gw - k < 16)
					v &= 0xffff << (16 - (gw - k));
				v <<= 16 - (pos & 15);
				row[pos >> 4] |= v >> 16;
				row[(pos >> 4) + 1] |= v;
			}
		}
	}

	nb = RegionNumRects(&clip);
	b = RegionRects(&clip);
	while (nb--) {
		int dx = b->x1 - x1, dy = b->y1 - y1;
		int bw = b->x2 - b->x1, bh = b->y2 - b->y1;
		int first = dy * words + (dx >> 4);
		int last = (dy + bh - 1) * words + ((dx + bw - 1) >> 4);
		const uint16_t *s = w->tmpl + first;

		/* the buffer is single: the previous template must be done with */
		warp_wait_idle(w);
		wr(w, REG_PATBUFF + 4, 0);
		for (k = (last - first + 2) >> 1; k > 0; k--, s += 2)
			wr(w, REG_PATBUFF, (uint32_t)s[0] << 16 | s[1]);
		wr(w, REG_PB_SHIFT, dx & 15);
		wr(w, REG_PB_BPR, bpr);
		wr(w, REG_PB_WIDTH, bw);
		wr(w, REG_PB_HEIGHT, bh);
		wr(w, REG_PB_PAT_H, 0);
		wr(w, REG_PB_DST_BASE, w->pbDstBase);
		wr(w, REG_PB_DST_BPR, w->pitch);
		wr(w, REG_PB_DST_BPP, w->Bpp);
		wr(w, REG_PB_BG, pGC->bgPixel & mask);
		wr(w, REG_PB_FG, pGC->fgPixel & mask);
		wr(w, REG_PB_DST_X, b->x1);
		wr(w, REG_PB_DST_Y, b->y1);
		wr(w, REG_PB_CR, PB_CR_START | (image ? 0 : PB_CR_NO_BG));
		w->nTextBlits++;
		b++;
	}
	RegionUnion(done, done, &clip);
	RegionUninit(&clip);
}

/*
 * A run of glyphs drawn by the template blitter, straight into VRAM: JAM2
 * over the background box for ImageText, JAM1 over the ink for PolyText.
 * FALSE (nothing drawn) when the glyphs do not suit it: ImageText ink must
 * stay inside each glyph's cell, as the box is cut between glyphs.
 */
static Bool
warp_text_hw(ScreenPtr pScreen, WarpPtr w, DrawablePtr d, GCPtr pGC, int x, int y,
	     unsigned long n, CharInfoPtr *ci, Bool image)
{
	FontPtr font = pGC->font;
	int ox = d->x + x, oy = d->y + y;
	int asc = image ? FONTASCENT(font) : 0, desc = image ? FONTDESCENT(font) : 0;
	int h, maxw, px;
	unsigned long i, j;
	RegionRec done;

	if (!n)
		return FALSE;
	for (i = 0; i < n; i++) {
		xCharInfo *m = &ci[i]->metrics;

		if (m->characterWidth < 0)
			return FALSE;
		if (GLYPHWIDTHPIXELS(ci[i]) <= 0 || GLYPHHEIGHTPIXELS(ci[i]) <= 0)
			continue;
		if (image) {
			if (m->leftSideBearing < 0 || m->rightSideBearing > m->characterWidth ||
			    m->ascent > asc || m->descent > desc)
				return FALSE;
		} else {
			asc = max(asc, m->ascent);
			desc = max(desc, m->descent);
		}
	}
	h = asc + desc;
	if (h <= 0 || h > PATBUFF_WORDS)
		return FALSE;
	/* widest template: in the buffer, and its bytes per row in 8 bits */
	maxw = min(127, PATBUFF_WORDS / h) * 16;
	for (i = 0; i < n; i++)
		if ((image ? ci[i]->metrics.characterWidth : GLYPHWIDTHPIXELS(ci[i])) > maxw)
			return FALSE;

	/* VRAM has to be right first, and the queued fills done */
	warp_flush(pScreen, w);
	RegionNull(&done);
	px = ox;
	for (i = 0; i < n; i = j) {
		int cx1 = image ? px : INT_MAX, cx2 = image ? px : INT_MIN, pen = px;

		for (j = i; j < n; j++) {
			xCharInfo *m = &ci[j]->metrics;
			int nx1 = cx1, nx2 = cx2;

			if (image)
				nx2 = pen + m->characterWidth;
			else if (GLYPHWIDTHPIXELS(ci[j]) > 0 && GLYPHHEIGHTPIXELS(ci[j]) > 0) {
				nx1 = min(cx1, pen + m->leftSideBearing);
				nx2 = max(cx2, pen + m->rightSideBearing);
			}
			if (j > i && nx2 > nx1 && nx2 - nx1 > maxw)
				break;
			cx1 = nx1;
			cx2 = nx2;
			pen += m->characterWidth;
		}
		if (cx2 > cx1)
			warp_text_chunk(w, pGC, &ci[i], j - i, px, oy, cx1, cx2, oy - asc, h,
					image, &done);
		px = pen;
	}
	if (RegionNotEmpty(&done))
		warp_hw_done(pScreen, w, &done);
	RegionUninit(&done);
	w->nText++;
	return TRUE;
}

/* the template blitter can do it: on screen, 8/16 bpp, plain GC */
static Bool
warp_text_ok(ScrnInfoPtr pScrn, WarpPtr w, DrawablePtr d, GCPtr pGC, int count, Bool image)
{
	FbBits full = FbFullMask(d->depth);

	return w->textAccel && warp_accel_ok(pScrn, w) && count > 0 && count <= 255 &&
	       (pGC->planemask & full) == full && warp_on_screen(pGC->pScreen, d) &&
	       /* ImageText ignores function and fill style */
	       (image || (pGC->alu == GXcopy && pGC->fillStyle == FillSolid));
}

static int
warp_poly_text_body(DrawablePtr d, GCPtr pGC, int x, int y, int count, void *c, Bool wide)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pGC->pScreen);
	WarpPtr w = WARPPTR(pScrn);
	int ret;
	WARP_GC_OP_PROLOGUE(pGC);

	if (warp_text_ok(pScrn, w, d, pGC, count, FALSE)) {
		FontEncoding enc = !wide ? Linear8Bit :
				   FONTLASTROW(pGC->font) == 0 ? Linear16Bit : TwoD16Bit;
		CharInfoPtr ci[256];
		unsigned long n, i;

		GetGlyphs(pGC->font, count, c, enc, &n, ci);
		if (warp_text_hw(pGC->pScreen, w, d, pGC, x, y, n, ci, FALSE)) {
			/* as miPolyText: the pen position after the run */
			ret = x;
			for (i = 0; i < n; i++)
				ret += ci[i]->metrics.characterWidth;
			WARP_GC_OP_EPILOGUE(pGC);
			return ret;
		}
		w->nTextSoft++;
	}
	if (wide)
		ret = (*pGC->ops->PolyText16)(d, pGC, x, y, count, c);
	else
		ret = (*pGC->ops->PolyText8)(d, pGC, x, y, count, c);
	WARP_GC_OP_EPILOGUE(pGC);
	return ret;
}


/* ---- entry points timed for the stats line ---- */

static void warp_image_text_body(DrawablePtr d, GCPtr pGC, int x, int y, int n, void *c,
				 Bool wide);

static RegionPtr
WarpCopyArea(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,
	     int srcx, int srcy, int width, int height, int dstx, int dsty)
{
	WarpPtr w = WARPPTR(xf86ScreenToScrn(pGC->pScreen));
	unsigned long long t0 = WARP_NOW(w);
	RegionPtr r = WarpCopyArea_body(pSrc, pDst, pGC, srcx, srcy, width, height, dstx, dsty);

	w->usCopyArea += WARP_SINCE(w, t0);
	return r;
}

static void
WarpPolyFillRect(DrawablePtr pDraw, GCPtr pGC, int nrect, xRectangle *rects)
{
	WarpPtr w = WARPPTR(xf86ScreenToScrn(pGC->pScreen));
	unsigned long long t0 = WARP_NOW(w);

	WarpPolyFillRect_body(pDraw, pGC, nrect, rects);
	w->usFillRect += WARP_SINCE(w, t0);
}

static void
WarpCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
{
	WarpPtr w = WARPPTR(xf86ScreenToScrn(pWin->drawable.pScreen));
	unsigned long long t0 = WARP_NOW(w);

	WarpCopyWindow_body(pWin, ptOldOrg, prgnSrc);
	w->usCopyWindow += WARP_SINCE(w, t0);
}

static int
warp_poly_text(DrawablePtr d, GCPtr pGC, int x, int y, int count, void *c, Bool wide)
{
	WarpPtr w = WARPPTR(xf86ScreenToScrn(pGC->pScreen));
	unsigned long long t0 = WARP_NOW(w);
	int r = warp_poly_text_body(d, pGC, x, y, count, c, wide);

	w->usText += WARP_SINCE(w, t0);
	return r;
}

static void
warp_image_text(DrawablePtr d, GCPtr pGC, int x, int y, int n, void *c, Bool wide)
{
	WarpPtr w = WARPPTR(xf86ScreenToScrn(pGC->pScreen));
	unsigned long long t0 = WARP_NOW(w);

	warp_image_text_body(d, pGC, x, y, n, c, wide);
	w->usText += WARP_SINCE(w, t0);
}

static int
WarpPolyText8(DrawablePtr d, GCPtr pGC, int x, int y, int n, char *c)
{
	return warp_poly_text(d, pGC, x, y, n, c, FALSE);
}

static int
WarpPolyText16(DrawablePtr d, GCPtr pGC, int x, int y, int n, unsigned short *c)
{
	return warp_poly_text(d, pGC, x, y, n, c, TRUE);
}

static void
warp_image_text_body(DrawablePtr d, GCPtr pGC, int x, int y, int n, void *c, Bool wide)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pGC->pScreen);
	WarpPtr w = WARPPTR(pScrn);
	FontEncoding enc = !wide ? Linear8Bit :
			   FONTLASTROW(pGC->font) == 0 ? Linear16Bit : TwoD16Bit;
	Bool op = FALSE;
	RegionRec r;
	WARP_GC_OP_PROLOGUE(pGC);

	if (warp_text_ok(pScrn, w, d, pGC, n, TRUE)) {
		CharInfoPtr ci[256];
		unsigned long ng;

		GetGlyphs(pGC->font, n, c, enc, &ng, ci);
		if (warp_text_hw(pGC->pScreen, w, d, pGC, x, y, ng, ci, TRUE)) {
			if (w->verify && warp_text_box(d, pGC, x, y, n, c, enc, &r)) {
				/*
				 * Draw it again by fb into the shadow and compare: the
				 * background box is all the text's, whatever was there.
				 */
				RegionPtr dmg = DamageRegion(w->pBuf->pDamage);
				int nb = RegionNumRects(&r), yy, xx;
				BoxPtr b = RegionRects(&r);
				unsigned long bad = 0;

				warp_wait_idle(w);
				w->inHw++;
				if (wide)
					(*pGC->ops->ImageText16)(d, pGC, x, y, n, c);
				else
					(*pGC->ops->ImageText8)(d, pGC, x, y, n, c);
				w->inHw--;
				while (nb--) {
					for (yy = b->y1; yy < b->y2; yy++)
						for (xx = b->x1; xx < b->x2; xx++) {
							unsigned long off = (unsigned long)yy * w->pitch +
									    xx * w->Bpp;

							if (memcmp(w->fbstart + off, w->shadow + off, w->Bpp)) {
								if (!bad && w->nTextBad < 20)
									ErrorF("warp: text check %lu: VRAM != fb at "
									       "%d,%d (box %d,%d-%d,%d)\n",
									       w->nTextChecked, xx, yy, b->x1,
									       b->y1, b->x2, b->y2);
								bad++;
							}
						}
					b++;
				}
				w->nTextChecked++;
				if (bad)
					w->nTextBad++;
				/* the area stays stale: nothing of this to push */
				RegionSubtract(dmg, dmg, &r);
				RegionUninit(&r);
			}
			WARP_GC_OP_EPILOGUE(pGC);
			return;
		}
		w->nTextSoft++;
	}

	RegionNull(&r);
	/* the background is drawn with GXcopy whatever the function */
	if (w->accel && pScrn->vtSema && warp_shadow_partial(w) &&
	    (pGC->planemask & FbFullMask(d->depth)) == FbFullMask(d->depth) &&
	    warp_on_screen(pGC->pScreen, d) &&
	    warp_text_box(d, pGC, x, y, n, c, enc, &r))
		op = warp_opaque_begin(w, &r, NULL);
	if (wide)
		(*pGC->ops->ImageText16)(d, pGC, x, y, n, c);
	else
		(*pGC->ops->ImageText8)(d, pGC, x, y, n, c);
	if (op)
		warp_opaque_end(w);
	RegionUninit(&r);
	WARP_GC_OP_EPILOGUE(pGC);
}

static void
WarpImageText8(DrawablePtr d, GCPtr pGC, int x, int y, int n, char *c)
{
	warp_image_text(d, pGC, x, y, n, c, FALSE);
}

static void
WarpImageText16(DrawablePtr d, GCPtr pGC, int x, int y, int n, unsigned short *c)
{
	warp_image_text(d, pGC, x, y, n, c, TRUE);
}

static const GCOps warpGCOps = {
	WarpFillSpans, WarpSetSpans, WarpPutImage, WarpCopyArea, WarpCopyPlane,
	WarpPolyPoint, WarpPolylines, WarpPolySegment, WarpPolyRectangle, WarpPolyArc,
	WarpFillPolygon, WarpPolyFillRect, WarpPolyFillArc, WarpPolyText8, WarpPolyText16,
	WarpImageText8, WarpImageText16, WarpImageGlyphBlt, WarpPolyGlyphBlt, WarpPushPixels,
};

#define WARP_GC_FUNC_PROLOGUE(pGC) \
	WarpGCPrivPtr pGCPriv = WARP_GC_PRIV(pGC); \
	pGC->funcs = pGCPriv->wrapFuncs; \
	if (pGCPriv->wrapOps) \
		pGC->ops = pGCPriv->wrapOps

#define WARP_GC_FUNC_EPILOGUE(pGC) \
	pGCPriv->wrapFuncs = pGC->funcs; \
	pGC->funcs = &warpGCFuncs; \
	if (pGCPriv->wrapOps) { \
		pGCPriv->wrapOps = pGC->ops; \
		pGC->ops = &warpGCOps; \
	}

static Bool
WarpCreateGC(GCPtr pGC)
{
	ScreenPtr pScreen = pGC->pScreen;
	WarpPtr w = WARPPTR(xf86ScreenToScrn(pScreen));
	WarpGCPrivPtr priv = WARP_GC_PRIV(pGC);
	Bool ret;

	w->nCreateGC++;
	pScreen->CreateGC = w->CreateGC;
	ret = (*pScreen->CreateGC)(pGC);
	w->CreateGC = pScreen->CreateGC;
	pScreen->CreateGC = WarpCreateGC;
	if (ret) {
		priv->wrapOps = NULL;
		priv->wrapFuncs = pGC->funcs;
		pGC->funcs = &warpGCFuncs;
	}
	return ret;
}

static void
WarpValidateGC(GCPtr pGC, unsigned long changes, DrawablePtr pDraw)
{
	WARP_GC_FUNC_PROLOGUE(pGC);
	WARPPTR(xf86ScreenToScrn(pGC->pScreen))->nValidateGC++;
	(*pGC->funcs->ValidateGC)(pGC, changes, pDraw);
	pGCPriv->wrapOps = pGC->ops;
	WARP_GC_FUNC_EPILOGUE(pGC);
	{
		WarpPtr w = WARPPTR(xf86ScreenToScrn(pGC->pScreen));

		if (w->debug && w->nValidateGC <= 60)
			ErrorF("warp: validate gc %p draw type %d depth %d: ops %p (priv %p) "
			       "CopyArea ours %d PolyFillRect ours %d funcs ours %d\n",
			       (void *)pGC, pDraw->type, pDraw->depth, (void *)pGC->ops,
			       (void *)&warpGCOps, pGC->ops->CopyArea == WarpCopyArea,
			       pGC->ops->PolyFillRect == WarpPolyFillRect,
			       pGC->funcs == &warpGCFuncs);
	}
}

static void
WarpDestroyGC(GCPtr pGC)
{
	WARP_GC_FUNC_PROLOGUE(pGC);
	(*pGC->funcs->DestroyGC)(pGC);
	WARP_GC_FUNC_EPILOGUE(pGC);
}

static void
WarpChangeGC(GCPtr pGC, unsigned long mask)
{
	WARP_GC_FUNC_PROLOGUE(pGC);
	(*pGC->funcs->ChangeGC)(pGC, mask);
	WARP_GC_FUNC_EPILOGUE(pGC);
}

static void
WarpCopyGC(GCPtr pGCSrc, unsigned long mask, GCPtr pGCDst)
{
	WARP_GC_FUNC_PROLOGUE(pGCDst);
	(*pGCDst->funcs->CopyGC)(pGCSrc, mask, pGCDst);
	WARP_GC_FUNC_EPILOGUE(pGCDst);
}

static void
WarpChangeClip(GCPtr pGC, int type, void *pvalue, int nrects)
{
	WARP_GC_FUNC_PROLOGUE(pGC);
	(*pGC->funcs->ChangeClip)(pGC, type, pvalue, nrects);
	WARP_GC_FUNC_EPILOGUE(pGC);
}

static void
WarpCopyClip(GCPtr pgcDst, GCPtr pgcSrc)
{
	WARP_GC_FUNC_PROLOGUE(pgcDst);
	(*pgcDst->funcs->CopyClip)(pgcDst, pgcSrc);
	WARP_GC_FUNC_EPILOGUE(pgcDst);
}

static void
WarpDestroyClip(GCPtr pGC)
{
	WARP_GC_FUNC_PROLOGUE(pGC);
	(*pGC->funcs->DestroyClip)(pGC);
	WARP_GC_FUNC_EPILOGUE(pGC);
}

/* -------------------------------------------------------------------- */
/* Hardware cursor: the FPGA sprite, 32x48, 2 bits per pixel            */
/* -------------------------------------------------------------------- */

/* realized image: SPR_H lines x 2 longs; pixel i of a 16-pixel long at bits 2i/2i+1 */
static inline void
spr_put(uint32_t *img, int x, int y, unsigned v)
{
	img[y * 2 + (x >> 4)] |= (uint32_t)v << ((x & 15) * 2);
}

static inline int
bitmap_bit(const unsigned char *row, int x)
{
	if (screenInfo.bitmapBitOrder == MSBFirst)
		return (row[x >> 3] >> (7 - (x & 7))) & 1;
	return (row[x >> 3] >> (x & 7)) & 1;
}

static unsigned char *
WarpRealizeCursor(xf86CursorInfoPtr info, CursorPtr pCurs)
{
	CursorBitsPtr bits = pCurs->bits;
	int stride = BitmapBytePad(bits->width);
	uint32_t *img = calloc(SPR_H * 2, sizeof(uint32_t));
	int x, y;

	if (!img)
		return NULL;
	for (y = 0; y < bits->height && y < SPR_H; y++) {
		const unsigned char *src = bits->source + y * stride;
		const unsigned char *msk = bits->mask + y * stride;

		for (x = 0; x < bits->width && x < SPR_W; x++)
			if (bitmap_bit(msk, x))
				spr_put(img, x, y, bitmap_bit(src, x) ? 1 : 2);	/* fg : bg */
	}
	return (unsigned char *)img;
}

static void
warp_load_sprite(WarpPtr w, const uint32_t *img)
{
	volatile uint32_t *buf = (volatile uint32_t *)(w->mmio + REG_SPR_BUFF);
	int i;

	buf[1] = 0;			/* reset the FPGA's write address */
	for (i = 0; i < SPR_H * 2; i++)
		buf[0] = img[i];
	w->sprCtrl |= SPR_CTRL_HIRES;
	wr(w, REG_SPR_CTRL, w->sprCtrl);
}

static void
WarpLoadCursorImage(ScrnInfoPtr pScrn, unsigned char *bits)
{
	warp_load_sprite(WARPPTR(pScrn), (const uint32_t *)bits);
}

static void
WarpSetCursorColors(ScrnInfoPtr pScrn, int bg, int fg)
{
	WarpPtr w = WARPPTR(pScrn);

	wr(w, REG_SPR_COL0, fg & 0xffffff);
	wr(w, REG_SPR_COL1, bg & 0xffffff);
}

static void
WarpSetCursorPosition(ScrnInfoPtr pScrn, int x, int y)
{
	WarpPtr w = WARPPTR(pScrn);

	/* 12-bit signed: the FPGA clips a sprite hanging off the top/left */
	wr(w, REG_SPR_POS_X, (uint32_t)x & 0xfff);
	wr(w, REG_SPR_POS_Y, (uint32_t)y & 0xfff);
}

static void
WarpHideCursor(ScrnInfoPtr pScrn)
{
	WarpPtr w = WARPPTR(pScrn);

	w->sprCtrl &= ~SPR_CTRL_EN;
	wr(w, REG_SPR_CTRL, w->sprCtrl);
}

static void
WarpShowCursor(ScrnInfoPtr pScrn)
{
	WarpPtr w = WARPPTR(pScrn);

	w->sprCtrl |= SPR_CTRL_EN;
	wr(w, REG_SPR_CTRL, w->sprCtrl);
}

static Bool
WarpUseHWCursor(ScreenPtr pScreen, CursorPtr pCurs)
{
	return xf86ScreenToScrn(pScreen)->vtSema &&
	       pCurs->bits->width <= SPR_W && pCurs->bits->height <= SPR_H;
}

/*
 * ARGB cursors (Xcursor themes) mapped onto the sprite's three colours:
 * alpha < 50% transparent, otherwise black, grey or white by luminance.
 */
static Bool
WarpLoadCursorARGBCheck(ScrnInfoPtr pScrn, CursorPtr pCurs)
{
	WarpPtr w = WARPPTR(pScrn);
	CursorBitsPtr bits = pCurs->bits;
	uint32_t img[SPR_H * 2];
	int x, y;

	if (!bits->argb || bits->width > SPR_W || bits->height > SPR_H)
		return FALSE;
	memset(img, 0, sizeof(img));
	for (y = 0; y < bits->height; y++)
		for (x = 0; x < bits->width; x++) {
			CARD32 p = bits->argb[y * bits->width + x];
			unsigned a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
			unsigned lum;

			if (a < 0x80)
				continue;
			/* premultiplied: undo for the luminance */
			r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
			lum = (r * 77 + g * 151 + b * 28) >> 8;
			spr_put(img, x, y, lum < 85 ? 1 : lum < 170 ? 3 : 2);
		}
	wr(w, REG_SPR_COL0, 0x000000);
	wr(w, REG_SPR_COL1, 0xffffff);
	wr(w, REG_SPR_COL2, 0x808080);
	warp_load_sprite(w, img);
	return TRUE;
}

static Bool
WarpUseHWCursorARGB(ScreenPtr pScreen, CursorPtr pCurs)
{
	return WarpUseHWCursor(pScreen, pCurs);
}

static Bool
warp_cursor_init(ScreenPtr pScreen, WarpPtr w)
{
	xf86CursorInfoPtr info = xf86CreateCursorInfoRec();

	if (!info)
		return FALSE;
	info->MaxWidth = SPR_W;
	info->MaxHeight = SPR_H;
	info->Flags = HARDWARE_CURSOR_TRUECOLOR_AT_8BPP | HARDWARE_CURSOR_UPDATE_UNHIDDEN |
		      HARDWARE_CURSOR_ARGB;
	info->RealizeCursor = WarpRealizeCursor;
	info->LoadCursorImage = WarpLoadCursorImage;
	info->SetCursorColors = WarpSetCursorColors;
	info->SetCursorPosition = WarpSetCursorPosition;
	info->HideCursor = WarpHideCursor;
	info->ShowCursor = WarpShowCursor;
	info->UseHWCursor = WarpUseHWCursor;
	info->UseHWCursorARGB = WarpUseHWCursorARGB;
	info->LoadCursorARGBCheck = WarpLoadCursorARGBCheck;
	w->sprCtrl = rd(w, REG_SPR_CTRL) & ~(SPR_CTRL_EN | (0x0f << 2));
	wr(w, REG_SPR_CTRL, w->sprCtrl);
	if (!xf86InitCursor(pScreen, info)) {
		xf86DestroyCursorInfoRec(info);
		return FALSE;
	}
	w->cursorInfo = info;
	return TRUE;
}

/* -------------------------------------------------------------------- */
/* Driver boilerplate (from fbdev)                                      */
/* -------------------------------------------------------------------- */

static const OptionInfoRec *
WarpAvailableOptions(int chipid, int busid)
{
	return WarpOptions;
}

static void
WarpIdentify(int flags)
{
	xf86PrintChipsets(WARP_NAME, "driver for the CS-Lab Warp RTG frame buffer", WarpChipsets);
}

static Bool
WarpSwitchMode(ScrnInfoPtr pScrn, DisplayModePtr mode)
{
	return fbdevHWSwitchMode(pScrn, mode);
}

static void
WarpAdjustFrame(ScrnInfoPtr pScrn, int x, int y)
{
	fbdevHWAdjustFrame(pScrn, x, y);
}

static Bool
WarpEnterVT(ScrnInfoPtr pScrn)
{
	return fbdevHWEnterVT(pScrn);
}

static void
WarpLeaveVT(ScrnInfoPtr pScrn)
{
	WarpPtr w = WARPPTR(pScrn);

	if (w->mmio && w->pBuf) {
		/* deferred fills out, then the shadow complete before VRAM stops being ours */
		warp_flush(xf86ScrnToScreen(pScrn), w);
		warp_wait_idle(w);
		warp_refresh_all(w);
	}
	fbdevHWLeaveVT(pScrn);
}

static ModeStatus
WarpValidMode(ScrnInfoPtr pScrn, DisplayModePtr mode, Bool verbose, int flags)
{
	return fbdevHWValidMode(pScrn, mode, verbose, flags);
}

static Bool
WarpProbe(DriverPtr drv, int flags)
{
	GDevPtr *devSections;
	int numDevSections, i;
	Bool found = FALSE;

	if (flags & PROBE_DETECT)
		return FALSE;
	if ((numDevSections = xf86MatchDevice(WARP_DRIVER_NAME, &devSections)) <= 0)
		return FALSE;
	if (!xf86LoadDrvSubModule(drv, "fbdevhw"))
		return FALSE;

	for (i = 0; i < numDevSections; i++) {
		const char *dev = xf86FindOptionValue(devSections[i]->options, "fbdev");
		ScrnInfoPtr pScrn = NULL;
		int entity;

		if (!fbdevHWProbe(NULL, (char *)dev, NULL))
			continue;
		entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
		pScrn = xf86ConfigFbEntity(pScrn, 0, entity, NULL, NULL, NULL, NULL);
		if (!pScrn)
			continue;
		found = TRUE;
		pScrn->driverVersion = WARP_VERSION;
		pScrn->driverName = WARP_DRIVER_NAME;
		pScrn->name = WARP_NAME;
		pScrn->Probe = WarpProbe;
		pScrn->PreInit = WarpPreInit;
		pScrn->ScreenInit = WarpScreenInit;
		pScrn->SwitchMode = WarpSwitchMode;
		pScrn->AdjustFrame = WarpAdjustFrame;
		pScrn->EnterVT = WarpEnterVT;
		pScrn->LeaveVT = WarpLeaveVT;
		pScrn->ValidMode = WarpValidMode;
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "using %s\n", dev ? dev : "default device");
	}
	free(devSections);
	return found;
}

static Bool
WarpPreInit(ScrnInfoPtr pScrn, int flags)
{
	WarpPtr w;
	int default_depth, fbbpp;
	const char *name;

	if (flags & PROBE_DETECT)
		return FALSE;
	if (pScrn->numEntities != 1)
		return FALSE;

	pScrn->monitor = pScrn->confScreen->monitor;
	if (!pScrn->driverPrivate)
		pScrn->driverPrivate = xnfcalloc(sizeof(WarpRec), 1);
	w = WARPPTR(pScrn);
	w->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

	if (!fbdevHWInit(pScrn, NULL,
			 (char *)xf86FindOptionValue(w->pEnt->device->options, "fbdev")))
		return FALSE;
	default_depth = fbdevHWGetDepth(pScrn, &fbbpp);
	if (!xf86SetDepthBpp(pScrn, default_depth, default_depth, fbbpp,
			     Support32bppFb))
		return FALSE;
	xf86PrintDepthBpp(pScrn);
	if (pScrn->bitsPerPixel != 8 && pScrn->bitsPerPixel != 16 &&
	    pScrn->bitsPerPixel != 32) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "unsupported %d bpp\n",
			   pScrn->bitsPerPixel);
		return FALSE;
	}

	if (pScrn->depth > 8) {
		rgb zeros = { 0, 0, 0 };

		if (!xf86SetWeight(pScrn, zeros, zeros))
			return FALSE;
	}
	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;
	if (pScrn->depth > 8 && pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "only TrueColor above 8 bpp\n");
		return FALSE;
	}
	{
		Gamma zeros = { 0.0, 0.0, 0.0 };

		if (!xf86SetGamma(pScrn, zeros))
			return FALSE;
	}

	pScrn->progClock = TRUE;
	pScrn->rgbBits = 8;
	pScrn->chipset = "warp";
	pScrn->videoRam = fbdevHWGetVidmem(pScrn);
	name = fbdevHWGetName(pScrn);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "hardware: %s (video memory: %dkB)\n",
		   name, pScrn->videoRam / 1024);
	if (strcmp(name, "Warp RTG"))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "frame buffer \"%s\" is not the Warp RTG; acceleration off\n", name);

	xf86CollectOptions(pScrn, NULL);
	if (!(w->Options = malloc(sizeof(WarpOptions))))
		return FALSE;
	memcpy(w->Options, WarpOptions, sizeof(WarpOptions));
	xf86ProcessOptions(pScrn->scrnIndex, w->pEnt->device->options, w->Options);
	w->accel = xf86ReturnOptValBool(w->Options, OPTION_ACCEL, TRUE) && !strcmp(name, "Warp RTG");
	w->hwcursor = xf86ReturnOptValBool(w->Options, OPTION_HWCURSOR, TRUE) && !strcmp(name, "Warp RTG");
	w->move16 = xf86ReturnOptValBool(w->Options, OPTION_MOVE16, TRUE);
	w->debug = xf86ReturnOptValBool(w->Options, OPTION_DEBUG, FALSE);
	w->deferFills = xf86ReturnOptValBool(w->Options, OPTION_DEFERFILLS, TRUE);
	w->verify = xf86ReturnOptValBool(w->Options, OPTION_VERIFY, FALSE);
	w->textAccel = w->accel && xf86ReturnOptValBool(w->Options, OPTION_ACCELTEXT, TRUE);
	w->minPixels = 1024;
	xf86GetOptValInteger(w->Options, OPTION_MINPIXELS, &w->minPixels);
	xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
		   "blitter %s (min %d pixels, fills %s), HW cursor %s, move16 shadow update %s\n",
		   w->accel ? "on" : "off", w->minPixels,
		   w->deferFills ? "deferred to the push" : "immediate",
		   w->hwcursor ? "on" : "off", w->move16 ? "on" : "off");

	fbdevHWSetVideoModes(pScrn);
	{
		DisplayModePtr mode, first = mode = pScrn->modes;

		if (mode != NULL)
			do {
				mode->status = xf86CheckModeForMonitor(mode, pScrn->monitor);
				mode = mode->next;
			} while (mode != NULL && mode != first);
		xf86PruneDriverModes(pScrn);
	}
	if (pScrn->modes == NULL)
		fbdevHWUseBuildinMode(pScrn);
	pScrn->currentMode = pScrn->modes;
	pScrn->displayWidth = pScrn->virtualX;
	xf86PrintModes(pScrn);
	xf86SetDpi(pScrn, 0, 0);

	if (fbdevHWGetType(pScrn) != FBDEVHW_PACKED_PIXELS) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "not a packed-pixel frame buffer\n");
		return FALSE;
	}
	if (!xf86LoadSubModule(pScrn, "fb") || !xf86LoadSubModule(pScrn, "shadow"))
		return FALSE;
	return TRUE;
}

static Bool
WarpCreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);
	Bool ret;

	pScreen->CreateScreenResources = w->CreateScreenResources;
	ret = (*pScreen->CreateScreenResources)(pScreen);
	pScreen->CreateScreenResources = WarpCreateScreenResources;
	if (!ret)
		return FALSE;
	if (!shadowAdd(pScreen, (*pScreen->GetScreenPixmap)(pScreen),
		       WarpShadowUpdate, WarpWindowLinear, 0, NULL))
		return FALSE;
	if (w->accel) {
		/* reports every drawing into the screen before it happens */
		w->pDamagePre = DamageCreate(WarpDamagePreReport, NULL, DamageReportRawRegion,
					     TRUE, pScreen, pScreen);
		if (!w->pDamagePre)
			return FALSE;
		DamageRegister(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, w->pDamagePre);
	}
	return TRUE;
}

static void
WarpLoadPalette(ScrnInfoPtr pScrn, int num, int *i, LOCO *col, VisualPtr pVis)
{
	fbdevHWLoadPalette(pScrn, num, i, col, pVis);
}

static void
WarpDPMSSet(ScrnInfoPtr pScrn, int mode, int flags)
{
	fbdevHWDPMSSet(pScrn, mode, flags);
}

static Bool
WarpSaveScreen(ScreenPtr pScreen, int mode)
{
	return fbdevHWSaveScreen(pScreen, mode);
}

static Bool
WarpScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);
	VisualPtr visual;

	if (!(w->fbmem = fbdevHWMapVidmem(pScrn))) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "mapping of video memory failed\n");
		return FALSE;
	}
	w->fboff = fbdevHWLinearOffset(pScrn);
	w->fbstart = w->fbmem + w->fboff;

	fbdevHWSave(pScrn);
	if (!fbdevHWModeInit(pScrn, pScrn->currentMode)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "mode initialization failed\n");
		return FALSE;
	}
	fbdevHWSaveScreen(pScreen, SCREEN_SAVER_ON);
	fbdevHWAdjustFrame(pScrn, 0, 0);

	if (w->accel || w->hwcursor) {
		w->mmio = fbdevHWMapMMIO(pScrn);
		if (!w->mmio) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "no register mapping: blitter and HW cursor off\n");
			w->accel = w->hwcursor = FALSE;
		}
	}

	w->Bpp = pScrn->bitsPerPixel / 8;
	w->pitch = fbdevHWGetLineLength(pScrn);
	w->width = pScrn->virtualX;
	w->height = pScrn->virtualY;
	w->pbDstBase = (VRAM_MEM_BASE + w->fboff) >> 4;
	if ((w->fboff & 15) || (w->Bpp != 1 && w->Bpp != 2))
		w->textAccel = FALSE;	/* 32 bpp: the blitter's colour layout is P96's */
	w->bltFmt = BLT_CR_FMT(w->Bpp == 1 ? 0 : w->Bpp == 2 ? 1 : 2);
	if (w->pitch & 15) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "pitch %d not 16-byte aligned: move16 off\n",
			   w->pitch);
		w->move16 = FALSE;
	}
	/* the shadow mirrors the VRAM layout exactly, so offsets are shared */
	pScrn->displayWidth = w->pitch / w->Bpp;
	if (posix_memalign((void **)&w->shadow, 16, (size_t)w->pitch * pScrn->virtualY)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "cannot allocate the shadow buffer\n");
		return FALSE;
	}
	memset(w->shadow, 0, (size_t)w->pitch * pScrn->virtualY);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%dx%d, %d bpp, pitch %d, registers %s\n",
		   pScrn->virtualX, pScrn->virtualY, pScrn->bitsPerPixel, w->pitch,
		   w->mmio ? "mapped" : "not mapped");

	miClearVisualTypes();
	if (pScrn->bitsPerPixel > 8) {
		if (!miSetVisualTypes(pScrn->depth, TrueColorMask, pScrn->rgbBits, TrueColor))
			return FALSE;
	} else {
		if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
				      pScrn->rgbBits, pScrn->defaultVisual))
			return FALSE;
	}
	if (!miSetPixmapDepths())
		return FALSE;

	if (!fbScreenInit(pScreen, w->shadow, pScrn->virtualX, pScrn->virtualY,
			  pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
			  pScrn->bitsPerPixel))
		return FALSE;

	if (pScrn->bitsPerPixel > 8) {
		visual = pScreen->visuals + pScreen->numVisuals;
		while (--visual >= pScreen->visuals) {
			if ((visual->class | DynamicClass) == DirectColor) {
				visual->offsetRed = pScrn->offset.red;
				visual->offsetGreen = pScrn->offset.green;
				visual->offsetBlue = pScrn->offset.blue;
				visual->redMask = pScrn->mask.red;
				visual->greenMask = pScrn->mask.green;
				visual->blueMask = pScrn->mask.blue;
			}
		}
	}
	if (!fbPictureInit(pScreen, NULL, 0))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "Render extension initialisation failed\n");

	if (!shadowSetup(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "shadow framebuffer initialization failed\n");
		return FALSE;
	}
	w->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = WarpCreateScreenResources;

	/* after shadowSetup: our wrappers sit outside damage's */
	RegionNull(&w->stale);
	RegionNull(&w->pend);
	w->nFillq = 0;
	if (w->accel && w->deferFills && !(w->fillq = malloc(WARP_FILLQ * sizeof(WarpFill))))
		w->deferFills = FALSE;
	if (w->accel) {
		w->BlockHandler = pScreen->BlockHandler;
		pScreen->BlockHandler = WarpBlockHandler;
	}
	if (w->accel) {
		if (!dixRegisterPrivateKey(&warpGCPrivKeyRec, PRIVATE_GC, sizeof(WarpGCPrivRec)))
			return FALSE;
		w->SourceValidate = pScreen->SourceValidate;
		pScreen->SourceValidate = WarpSourceValidate;
		w->CreateGC = pScreen->CreateGC;
		pScreen->CreateGC = WarpCreateGC;
		w->CopyWindow = pScreen->CopyWindow;
		pScreen->CopyWindow = WarpCopyWindow;
	}

	xf86SetBlackWhitePixels(pScreen);
	xf86SetBackingStore(pScreen);
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());
	if (w->hwcursor && !warp_cursor_init(pScreen, w)) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "HW cursor init failed, using SW cursor\n");
		w->hwcursor = FALSE;
	}

	if (!miCreateDefColormap(pScreen))
		return FALSE;
	if (!xf86HandleColormaps(pScreen, 256, 8, WarpLoadPalette, NULL, CMAP_PALETTED_TRUECOLOR))
		return FALSE;
	xf86DPMSInit(pScreen, WarpDPMSSet, 0);
	pScreen->SaveScreen = WarpSaveScreen;

	w->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = WarpCloseScreen;
	return TRUE;
}

static Bool
WarpCloseScreen(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WarpPtr w = WARPPTR(pScrn);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "stats: %lu copies (%lu boxes, %llu px), %lu fills (%lu boxes, %llu px), "
		   "%lu shadow updates (%llu bytes), %lu flushes; GC: %lu created, %lu validated, "
		   "%lu CopyArea calls, %lu PolyFillRect calls; CreateGC still ours: %d; "
		   "%lu shadow refreshes (%llu bytes), stale_skip %lu; "
		   "time: blitter wait %llu ms, refresh %llu ms, push %llu ms; max stale boxes %lu; "
		   "%lu opaque ops; %lu fill materializations (%llu bytes), %lu queue-full flushes; "
		   "verify: %lu checks, %lu bad (%llu pixels); "
		   "text: %lu runs by the template blitter (%lu blits), %lu in software, "
		   "%lu checked, %lu bad; time in: CopyArea %llu ms, PolyFillRect %llu ms, "
		   "CopyWindow %llu ms, text %llu ms, block handlers %llu ms\n",
		   w->nCopy, w->nCopyBoxes, w->copyPixels, w->nFill, w->nFillBoxes,
		   w->fillPixels, w->nUpdates, w->updateBytes, w->nFlushes,
		   w->nCreateGC, w->nValidateGC, w->nCopyAreaCalls, w->nFillCalls,
		   pScreen->CreateGC == WarpCreateGC, w->nRefresh, w->refreshBytes,
		   w->nStaleSkip, w->usWait / 1000, w->usRefresh / 1000, w->usPush / 1000,
		   w->maxStale, w->nOpaque, w->nMaterialize, w->materializeBytes, w->nFillFlush,
		   w->nVerify, w->nVerifyBad, w->verifyBadPixels, w->nText, w->nTextBlits,
		   w->nTextSoft, w->nTextChecked, w->nTextBad, w->usCopyArea / 1000,
		   w->usFillRect / 1000, w->usCopyWindow / 1000, w->usText / 1000, w->usBlock / 1000);

	if (w->pDamagePre) {
		DamageUnregister(w->pDamagePre);
		DamageDestroy(w->pDamagePre);
		w->pDamagePre = NULL;
	}
	RegionUninit(&w->stale);
	RegionNull(&w->stale);
	RegionUninit(&w->pend);
	RegionNull(&w->pend);
	w->nFillq = 0;
	free(w->fillq);
	w->fillq = NULL;

	if (w->mmio) {
		warp_wait_idle(w);
		if (w->hwcursor) {
			w->sprCtrl &= ~SPR_CTRL_EN;
			wr(w, REG_SPR_CTRL, w->sprCtrl);
		}
	}
	if (w->cursorInfo) {
		xf86DestroyCursorInfoRec(w->cursorInfo);
		w->cursorInfo = NULL;
	}
	fbdevHWRestore(pScrn);
	fbdevHWUnmapVidmem(pScrn);
	if (w->mmio) {
		fbdevHWUnmapMMIO(pScrn);
		w->mmio = NULL;
	}
	if (w->shadow) {
		shadowRemove(pScreen, (*pScreen->GetScreenPixmap)(pScreen));
		free(w->shadow);
		w->shadow = NULL;
	}
	w->pBuf = NULL;
	pScrn->vtSema = FALSE;

	if (w->accel) {
		pScreen->CreateGC = w->CreateGC;
		pScreen->CopyWindow = w->CopyWindow;
		pScreen->SourceValidate = w->SourceValidate;
		pScreen->BlockHandler = w->BlockHandler;
	}
	pScreen->CreateScreenResources = w->CreateScreenResources;
	pScreen->CloseScreen = w->CloseScreen;
	return (*pScreen->CloseScreen)(pScreen);
}

static Bool
WarpDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op, void *ptr)
{
	switch (op) {
	case GET_REQUIRED_HW_INTERFACES:
		*(CARD32 *)ptr = 0;
		return TRUE;
	default:
		return FALSE;
	}
}
