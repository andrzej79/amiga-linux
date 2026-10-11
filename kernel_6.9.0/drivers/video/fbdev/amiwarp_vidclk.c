// SPDX-License-Identifier: GPL-2.0
/*
 * CS-Lab Warp - video pixel clock (Xilinx 7-series MMCME2 via DRP).
 *
 * Port of csWarpAmigaSoftware common/mmcm_drp.c and common/vidclk_hw.c
 * (CS-Lab), which port Xilinx XAPP888 mmcme2_drp.v / mmcme2_drp_func.h.
 * Integer, 32-bit maths only, kept step for step with the original: those
 * files are checked bit for bit against vectors simulated from the HDL.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/zorro.h>

#include <asm/cswarpdefs.h>

#include "amiwarp_vidclk.h"

#define FRAC_PRECISION		10u
#define MMCM_DRP_PROG_LEN	23
#define VIDCLK_LOCK_POLLS	100000ul	/* SR reads (~300 ns each) */

struct mmcm_counter {
	u8 divide;	/* 1..128 */
	u8 frac_en;	/* fractional mode, CLKFBOUT and CLKOUT0 only */
	u16 frac;	/* 0..875 in steps of 125 */
	u32 phase;	/* millidegrees */
	u32 duty;	/* duty * 100000 */
};

struct mmcm_config {
	u8 divclk;
	struct mmcm_counter clkfb;
	struct mmcm_counter clkout[7];
};

/* new = (old & mask) | data; a mask bit of 1 keeps the old bit */
struct mmcm_drp_write {
	u16 addr;
	u16 mask;
	u16 data;
};

struct count_regs {
	u16 reg1;
	u16 reg2;
	u16 shared;
};

/* mmcm_lock_lookup(), indexed by M - 1, split into top byte and low 32 bits */
static const u8 lock_hi[64] = {
	0x31, 0x31, 0x42, 0x5A, 0x73, 0x8C, 0x9C, 0xB5, 0xCE, 0xE7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const u32 lock_lo[64] = {
	0xBE8FA401ul, 0xBE8FA401ul, 0x3E8FA401ul, 0xFE8FA401ul, 0xBE8FA401ul, 0x7E8FA401ul, 0xFE8FA401ul, 0xBE8FA401ul,
	0x7E8FA401ul, 0x3E8FA401ul, 0xF84FA401ul, 0xF39FA401ul, 0xEEEFA401ul, 0xEBCFA401ul, 0xE8AFA401ul, 0xE71FA401ul,
	0xE3FFA401ul, 0xE26FA401ul, 0xE0DFA401ul, 0xDF4FA401ul, 0xDDBFA401ul, 0xDC2FA401ul, 0xDA9FA401ul, 0xD90FA401ul,
	0xD90FA401ul, 0xD77FA401ul, 0xD5EFA401ul, 0xD5EFA401ul, 0xD45FA401ul, 0xD45FA401ul, 0xD2CFA401ul, 0xD2CFA401ul,
	0xD2CFA401ul, 0xD13FA401ul, 0xD13FA401ul, 0xD13FA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul,
	0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul,
	0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul,
	0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul, 0xCFAFA401ul};

/* mmcm_filter_lookup(), BANDWIDTH "OPTIMIZED", indexed by M - 1 */
static const u16 filter_opt[64] = {
	0x0BC, 0x13C, 0x16C, 0x1DC, 0x35C, 0x3AC, 0x3B4, 0x3CC, 0x394, 0x3D4, 0x3E4, 0x344, 0x3E4, 0x3E4, 0x3E4, 0x3E4,
	0x3D4, 0x3D4, 0x304, 0x304, 0x304, 0x170, 0x170, 0x170, 0x170, 0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0D0,
	0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0D0, 0x0A0, 0x0A0, 0x0A0, 0x0A0, 0x0A0, 0x1C4, 0x1C4,
	0x130, 0x130, 0x130, 0x130, 0x184, 0x184, 0x158, 0x158, 0x158, 0x090, 0x090, 0x090, 0x090, 0x128, 0x0F0, 0x0F0};

/* round_frac(): the HDL tests decimal[FRAC_PRECISION - precision] of a [32:1] vector */
static u32 round_frac(u32 decimal, u32 precision)
{
	if (decimal & (1ul << (FRAC_PRECISION - 1u - precision)))
		return decimal + (1ul << (FRAC_PRECISION - precision));
	return decimal;
}

/* mmcm_divider(): {edge, no_count, high[5:0], low[5:0]} */
static u16 mmcm_divider(u32 divide, u32 duty)
{
	u32 duty_fix, temp, high, low, edge, no_count;

	if (divide >= 64u) {
		u32 duty_min = ((divide - 64u) * 100000ul) / divide;
		u32 duty_max = (6450000ul + divide / 2u) / divide;

		if (duty > duty_max)
			duty = duty_max;
		if (duty < duty_min)
			duty = duty_min;
	}
	duty_fix = (duty << FRAC_PRECISION) / 100000ul;

	if (divide == 1u) {
		high = 1u;
		edge = 0u;
		low = 1u;
		no_count = 1u;
	} else {
		temp = round_frac(duty_fix * divide, 1u);
		high = (temp >> FRAC_PRECISION) & 0x7Fu;
		edge = (temp >> (FRAC_PRECISION - 1u)) & 1u;
		if (high == 0u) {
			high = 1u;
			edge = 0u;
		}
		if (high == divide) {
			high = divide - 1u;
			edge = 1u;
		}
		low = divide - high;
		no_count = 0u;
	}
	return (u16)((edge << 13) | (no_count << 12) | ((high & 0x3Fu) << 6) | (low & 0x3Fu));
}

/* mmcm_phase(): {mx = 0, phase_mux[2:0], delay_time[5:0]} */
static u16 mmcm_phase(u32 divide, u32 phase)
{
	u32 phase_fixed = (phase << FRAC_PRECISION) / 1000u;
	u32 phase_in_cycles = (phase_fixed * divide) / 360u;	/* wraps at 32 bits, as the HDL */
	u32 temp = round_frac(phase_in_cycles, 3u);
	u32 phase_mux = (temp >> (FRAC_PRECISION - 3u)) & 0x7u;
	u32 delay_time = (temp >> FRAC_PRECISION) & 0x3Fu;

	return (u16)((phase_mux << 6) | delay_time);
}

static void mmcm_count_calc(u32 divide, u32 phase, u32 duty, struct count_regs *r)
{
	u16 div = mmcm_divider(divide, duty);
	u16 ph = mmcm_phase(divide, phase);

	r->reg2 = (u16)((((ph >> 9) & 0x3u) << 8) | (((div >> 12) & 0x3u) << 6) | (ph & 0x3Fu));
	r->reg1 = (u16)((((ph >> 6) & 0x7u) << 13) | (div & 0x0FFFu));
	r->shared = 0u;
}

/* mmcm_frac_count_calc(): the HDL's 8-bit intermediates wrap like u8 */
static void mmcm_frac_count_calc(u32 divide, u32 phase, u32 frac, struct count_regs *r)
{
	u8 frac8 = (u8)((frac / 125u) & 0x7u);
	u8 even_high = (u8)(divide >> 1);
	u8 even_low = even_high;
	u8 odd = (u8)(divide - even_high - even_low);
	u8 odd_and_frac = (u8)(8u * odd + frac8);
	u8 lt_frac = (u8)(even_high - (odd_and_frac <= 9u));
	u8 ht_frac = (u8)(even_low - (odd_and_frac <= 8u));
	u8 pm_fall = (u8)(((odd & 0x7Fu) << 2) + (frac8 >> 1));
	u32 wf_fall = ((odd_and_frac >= 2u && odd_and_frac <= 9u) || (frac8 == 1u && divide == 2u)) ? 1u : 0u;
	u32 wf_rise = (odd_and_frac >= 1u && odd_and_frac <= 8u) ? 1u : 0u;
	u32 a_per_in_octets = 8u * divide + frac / 125u;
	u32 a_phase_in_cycles = ((phase + 10u) * a_per_in_octets) / 360000ul;
	u8 lo8 = (u8)a_phase_in_cycles;
	u8 pm_rise_frac = (lo8 == 0u) ? 0u : (u8)(lo8 - (lo8 & 0xF8u));
	u8 dt = (u8)((((phase + 10u) * a_per_in_octets) / 8u) / 360000ul);
	u8 pm_rise_frac_filt = (pm_rise_frac >= 8u) ? (u8)(pm_rise_frac - 8u) : pm_rise_frac;
	u8 pm_fall_frac = (u8)(pm_fall + pm_rise_frac);
	u8 pm_fall_frac_filt = (u8)(pm_fall + pm_rise_frac - (pm_fall_frac & 0xF8u));

	r->shared = (u16)((0x3u << 4) | ((pm_fall_frac_filt & 0x7u) << 1) | wf_fall);
	r->reg2 = (u16)(((u32)frac8 << 12) | (1u << 11) | (wf_rise << 10) | (dt & 0x3Fu));
	r->reg1 = (u16)(((u32)(pm_rise_frac_filt & 0x7u) << 13) | ((u32)(ht_frac & 0x3Fu) << 6) |
			(lt_frac & 0x3Fu));
}

static bool counter_valid(const struct mmcm_counter *k, bool frac_allowed)
{
	if (k->divide < 1u || k->divide > 128u || k->phase > 360000ul)
		return false;
	if (k->frac_en)
		return frac_allowed && k->frac <= 875u && (k->frac % 125u) == 0u;
	return k->duty > 0ul && k->duty < 100000ul;
}

static int mmcm_drp_build(const struct mmcm_config *c, struct mmcm_drp_write *prog)
{
	struct count_regs out[7], fb, div;
	u32 m, lock_h, lock_l;
	u16 filt;
	int i;

	if (c->divclk < 1u || c->divclk > 106u || c->clkfb.divide < 2u || c->clkfb.divide > 64u ||
	    !counter_valid(&c->clkfb, true))
		return -1;
	for (i = 0; i < 7; i++)
		if (!counter_valid(&c->clkout[i], i == 0))
			return -1;

	if (c->clkout[0].frac_en)
		mmcm_frac_count_calc(c->clkout[0].divide, c->clkout[0].phase, c->clkout[0].frac, &out[0]);
	else
		mmcm_count_calc(c->clkout[0].divide, c->clkout[0].phase, c->clkout[0].duty, &out[0]);
	for (i = 1; i < 7; i++)
		mmcm_count_calc(c->clkout[i].divide, c->clkout[i].phase, c->clkout[i].duty, &out[i]);
	if (c->clkfb.frac_en)
		mmcm_frac_count_calc(c->clkfb.divide, c->clkfb.phase, c->clkfb.frac, &fb);
	else
		mmcm_count_calc(c->clkfb.divide, c->clkfb.phase, 50000ul, &fb);
	mmcm_count_calc(c->divclk, 0ul, 50000ul, &div);

	m = c->clkfb.divide;
	lock_h = lock_hi[m - 1u];
	lock_l = lock_lo[m - 1u];
	filt = filter_opt[m - 1u];

#define DRP(n, a, msk, d) do { prog[n].addr = (a); prog[n].mask = (msk); prog[n].data = (u16)(d); } while (0)
	/* the order and masks of mmcme2_drp.v's rom[] for one state */
	DRP(0,  0x28, 0x0000, 0xFFFFu);				/* power bits */
	DRP(1,  0x09, 0x8000, out[0].reg2);			/* CLKOUT0 */
	DRP(2,  0x08, 0x1000, out[0].reg1);
	DRP(3,  0x0A, 0x1000, out[1].reg1);			/* CLKOUT1 */
	DRP(4,  0x0B, 0xFC00, out[1].reg2);
	DRP(5,  0x0C, 0x1000, out[2].reg1);			/* CLKOUT2 */
	DRP(6,  0x0D, 0xFC00, out[2].reg2);
	DRP(7,  0x0E, 0x1000, out[3].reg1);			/* CLKOUT3 */
	DRP(8,  0x0F, 0xFC00, out[3].reg2);
	DRP(9,  0x10, 0x1000, out[4].reg1);			/* CLKOUT4 */
	DRP(10, 0x11, 0xFC00, out[4].reg2);
	DRP(11, 0x06, 0x1000, out[5].reg1);			/* CLKOUT5, + CLKOUT0's shared bits */
	DRP(12, 0x07, 0xC000, c->clkout[0].frac_en
			      ? ((out[5].reg2 & 0xC000u) | ((out[0].shared & 0xFu) << 10) | (out[5].reg2 & 0x03FFu))
			      : out[5].reg2);
	DRP(13, 0x12, 0x1000, out[6].reg1);			/* CLKOUT6, + CLKFBOUT's shared bits */
	DRP(14, 0x13, 0xC000, c->clkfb.frac_en
			      ? ((out[6].reg2 & 0xC000u) | ((fb.shared & 0xFu) << 10) | (out[6].reg2 & 0x03FFu))
			      : out[6].reg2);
	DRP(15, 0x16, 0xC000, ((div.reg2 >> 6) & 0x3u) << 12 | (div.reg1 & 0x0FFFu));	/* DIVCLK */
	DRP(16, 0x14, 0x1000, fb.reg1);				/* CLKFBOUT */
	DRP(17, 0x15, 0x8000, fb.reg2);
	/* lock: LOCK[29:20]; {LOCK[34:30], LOCK[9:0]}; {LOCK[39:35], LOCK[19:10]} */
	DRP(18, 0x18, 0xFC00, (lock_l >> 20) & 0x3FFu);
	DRP(19, 0x19, 0x8000, ((((lock_h << 2) | (lock_l >> 30)) & 0x1Fu) << 10) | (lock_l & 0x3FFu));
	DRP(20, 0x1A, 0x8000, (((lock_h >> 3) & 0x1Fu) << 10) | ((lock_l >> 10) & 0x3FFu));
	/* digital filter */
	DRP(21, 0x4E, 0x66FF, (((filt >> 9) & 0x1u) << 15) | (((filt >> 7) & 0x3u) << 11) |
			      (((filt >> 6) & 0x1u) << 8));
	DRP(22, 0x4F, 0x666F, (((filt >> 5) & 0x1u) << 15) | (((filt >> 3) & 0x3u) << 11) |
			      (((filt >> 1) & 0x3u) << 7) | ((filt & 0x1u) << 4));
#undef DRP
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Video pixel clock                                                        */
/* ------------------------------------------------------------------------ */

const struct warp_vidclk_cfg warp_vidclk_native = { 8u, 2u, 346u };	/* 108.125 MHz */

u32 warp_vidclk_hz(const struct warp_vidclk_cfg *c)
{
	u32 dd = (u32)c->D * c->d;

	return (5000000ul * c->m8 + dd / 2u) / dd;
}

/* round(hz * dd / 5 MHz) without a 64-bit product */
static u32 m8_nearest(u32 hz, u32 dd)
{
	u32 ad = (hz / 5000u) * dd;
	u32 p = ad / 1000u;
	u32 s = (ad % 1000u) * 5000u + (hz % 5000u) * dd;

	p += s / 5000000ul;
	s %= 5000000ul;
	return (s >= 2500000ul) ? p + 1u : p;
}

u32 warp_vidclk_find(u32 hz, struct warp_vidclk_cfg *c)
{
	u32 D, d, best_err = 0xFFFFFFFFul, best_hz = 0;
	struct warp_vidclk_cfg best = warp_vidclk_native;

	hz = clamp_t(u32, hz, VIDCLK_MIN_HZ, VIDCLK_MAX_HZ);
	for (D = 1u; D <= 20u; D++) {
		for (d = 1u; d <= 25u; d++) {
			struct warp_vidclk_cfg k;
			u32 m8, achieved, err;

			m8 = m8_nearest(hz, D * d);
			/* achieved VCO = 25 MHz * m8 / D within 600..1200 MHz */
			if (m8 < 16u || m8 > 512u || m8 < 24u * D || m8 > 48u * D)
				continue;
			k.D = (u8)D;
			k.d = (u8)d;
			k.m8 = (u16)m8;
			achieved = warp_vidclk_hz(&k);
			err = (achieved > hz) ? achieved - hz : hz - achieved;
			if (err < best_err || (err == best_err && D == best.D && m8 > best.m8)) {
				best_err = err;
				best_hz = achieved;
				best = k;
			}
		}
	}
	*c = best;
	return best_hz;
}

static void vidclk_mmcm_config(const struct warp_vidclk_cfg *c, struct mmcm_config *mc)
{
	int i;

	mc->divclk = c->D;
	mc->clkfb.divide = (u8)(c->m8 >> 3);
	mc->clkfb.frac = (u16)((c->m8 & 7u) * 125u);
	mc->clkfb.frac_en = (c->m8 & 7u) != 0u;
	mc->clkfb.phase = 0ul;
	mc->clkfb.duty = 50000ul;
	for (i = 0; i < 7; i++) {
		mc->clkout[i].divide = 100u;
		mc->clkout[i].frac_en = 0u;
		mc->clkout[i].frac = 0u;
		mc->clkout[i].phase = 0ul;
		mc->clkout[i].duty = 50000ul;
	}
	mc->clkout[1].divide = c->d;
	mc->clkout[2].divide = (u8)(5u * c->d);
}

/* ------------------------------------------------------------------------ */
/* Hardware                                                                 */
/* ------------------------------------------------------------------------ */

bool warp_vidclk_has_drp(void __iomem *regs)
{
	u32 sr = z_readl(regs + WARP_PIXCLK_SR);

	return sr != 0xFFFFFFFFul && (sr & PIXCLK_SR_DRP_IF);
}

static s32 drp_read(void __iomem *regs, u32 addr)
{
	u32 v;

	z_writel(addr & 0x7Ful, regs + WARP_PIXCLK_DRP_ADDR);
	v = z_readl(regs + WARP_PIXCLK_DRP_DATA);
	if (z_readl(regs + WARP_PIXCLK_SR) & PIXCLK_SR_DRP_TIMEOUT)
		return -1;
	return (s32)(v & 0xFFFFul);
}

static bool prog_loaded(void __iomem *regs, const struct mmcm_drp_write *prog)
{
	int i;

	if ((z_readl(regs + WARP_PIXCLK_CR) & PIXCLK_CR_MMCM_RST) ||
	    !(z_readl(regs + WARP_PIXCLK_SR) & PIXCLK_SR_LOCKED))
		return false;
	for (i = 0; i < MMCM_DRP_PROG_LEN; i++) {
		s32 v = drp_read(regs, prog[i].addr);

		if (v < 0 || ((u32)v & ~(u32)prog[i].mask & 0xFFFFul) != prog[i].data)
			return false;
	}
	return true;
}

int warp_vidclk_apply(void __iomem *regs, const struct warp_vidclk_cfg *cfg, bool force)
{
	struct mmcm_drp_write prog[MMCM_DRP_PROG_LEN];
	struct mmcm_config mc;
	int status = VIDCLK_OK;
	u32 i;

	if (!warp_vidclk_has_drp(regs))
		return VIDCLK_ERR_NO_DRP;

	vidclk_mmcm_config(cfg, &mc);
	if (mmcm_drp_build(&mc, prog) != 0)
		return VIDCLK_ERR_CONFIG;
	if (!force && prog_loaded(regs, prog))
		return VIDCLK_UNCHANGED;

	/* XAPP888: reset, the read-modify-writes in the HDL's order, release */
	z_writel(z_readl(regs + WARP_PIXCLK_CR) | PIXCLK_CR_MMCM_RST, regs + WARP_PIXCLK_CR);
	for (i = 0; i < MMCM_DRP_PROG_LEN; i++) {
		s32 old = drp_read(regs, prog[i].addr);

		if (old < 0) {
			status = VIDCLK_ERR_DRP_TIMEOUT;
			break;
		}
		z_writel(((u32)old & prog[i].mask) | prog[i].data, regs + WARP_PIXCLK_DRP_DATA);
		if (z_readl(regs + WARP_PIXCLK_SR) & PIXCLK_SR_DRP_TIMEOUT) {
			status = VIDCLK_ERR_DRP_TIMEOUT;
			break;
		}
	}
	z_writel(z_readl(regs + WARP_PIXCLK_CR) & ~PIXCLK_CR_MMCM_RST, regs + WARP_PIXCLK_CR);

	for (i = 0; i < VIDCLK_LOCK_POLLS; i++)
		if (z_readl(regs + WARP_PIXCLK_SR) & PIXCLK_SR_LOCKED)
			break;
	if (i == VIDCLK_LOCK_POLLS && status == VIDCLK_OK)
		status = VIDCLK_ERR_LOCK;
	return status;
}

const char *warp_vidclk_status_text(int status)
{
	switch (status) {
	case VIDCLK_OK:			return "programmed, locked";
	case VIDCLK_UNCHANGED:		return "already set, not reprogrammed";
	case VIDCLK_ERR_NO_DRP:		return "FPGA has no video PLL DRP interface (update the firmware package)";
	case VIDCLK_ERR_CONFIG:		return "configuration out of range";
	case VIDCLK_ERR_DRP_TIMEOUT:	return "DRP access timed out";
	case VIDCLK_ERR_LOCK:		return "programmed, but the PLL did not lock";
	default:			return "unknown status";
	}
}
