// SPDX-License-Identifier: GPL-2.0
/*
 * CS-Lab Warp turbo board - core driver
 *
 * Copyright (C) 2024-2026 Andrzej Rogozynski, CS-Lab s.c.
 *
 * Owns the Warp CTRL Zorro board and everything on it that is shared:
 *
 *  - the 4 KB dual-port RAM command/reply frame.  One exchange at a time:
 *    the 68k writes a command, sets MP_ARM, the ARM sets MR_ARM when it has
 *    taken it and MP_68K when the reply is in the frame.  frame_lock makes
 *    each exchange exclusive (cswarp.library's frameLock()).
 *  - the INT2 (IRQ_AMIGA_PORTS) handler, shared with the CIAs.  It wakes the
 *    frame owner on MR_ARM/MP_68K and rings the async unit doorbells from the
 *    DPREG register file (IRQ_PEND), clearing every flag before dispatch: the
 *    line is level sensitive.
 *  - board identity (FPGA DPREG id, SysCfg id, firmware version, board type,
 *    IDE mode), checked once at probe.
 *
 * Function drivers are MFD children: cswarp-net, cswarp-ata, cswarp-fb.
 *
 * Requires firmware package 3.000 or later (DPREG register file).
 */

#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mfd/core.h>
#include <linux/mfd/cswarp.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/zorro.h>

#include <asm/amigaints.h>

#define DRV_NAME	"cswarp-core"

unsigned int cswarp_dbg_mask = CSWARP_DBG_DEFAULT;
module_param_named(dbg, cswarp_dbg_mask, uint, 0644);
MODULE_PARM_DESC(dbg, "Debug message mask: 0x1 core, 0x2 mbox, 0x4 irq, 0x8 net, 0x10 net-pkt, 0x20 fb, 0x40 fb-accel, 0x80 ata, 0x100 ata-io");
EXPORT_SYMBOL_GPL(cswarp_dbg_mask);

static bool use_irq = true;
module_param(use_irq, bool, 0444);
MODULE_PARM_DESC(use_irq, "Use INT2 for the frame handshake and doorbells (0: poll, for bring-up)");

static unsigned int xfer_timeout_ms = 5000;
module_param(xfer_timeout_ms, uint, 0644);
MODULE_PARM_DESC(xfer_timeout_ms, "Frame exchange timeout in ms");

static unsigned int spin_us = 100;
module_param(spin_us, uint, 0644);
MODULE_PARM_DESC(spin_us, "Busy-wait this long for the ARM before sleeping (us)");

#define CSWARP_POLL_SLICE_MS	10	/* irq fallback: re-check the CR this often */
#define CSWARP_DOORBELL_POLL_MS	20	/* doorbell polling with use_irq=0 */

/* Wire layout checks against WarpAmiCommData.h (pack(2), big-endian) */
static_assert(sizeof(DprCmdHeader) == 4);
static_assert(offsetof(DprCmdEthIfSend, ifNr) == 4);
static_assert(offsetof(DprCmdEthIfSend, pktSize) == 6);
static_assert(offsetof(DprCmdEthIfSend, packet) == 8);
static_assert(offsetof(DprRplEthIfRecv, ifNr) == 4);
static_assert(offsetof(DprRplEthIfRecv, pktSize) == 6);
static_assert(offsetof(DprRplEthIfRecv, packet) == 8);
static_assert(offsetof(DprRplEthIfInfo, bps) == 8);
static_assert(offsetof(DprRplEthIfInfo, mtu) == 12);
static_assert(offsetof(DprRplEthIfInfo, mac) == 14);
static_assert(sizeof(DprRplEthIfInfo) == 22);
static_assert(offsetof(DprRplDiagMsg, hwVer) == 24);
static_assert(offsetof(DprRplDiagMsg, fmwVer) == 28);
static_assert(offsetof(DprRplDiagMsg, boardName) == 40);
static_assert(offsetof(DprRplDiagMsg, nativeIdeEnabled) == 340);
static_assert(offsetof(DprRplDiagMsg, IDE_IOR_as) == 352);
static_assert(offsetof(DprRplDiagMsg, timeZoneCorrSecs) == 358);
static_assert(offsetof(DprRplDiagMsg, warpBoardType) == 362);
static_assert(offsetof(DprRplDiagMsg, wifiState) == 364);
static_assert(sizeof(DprRplDiagMsg) == 412);
static_assert(sizeof(DprRplDiagMsg) <= DPR_FRAME_SIZE);

/* ------------------------------------------------------------------------ */
/* DPREG register file                                                      */
/* ------------------------------------------------------------------------ */

static inline u32 dpreg_read(struct cswarp_core *wc, unsigned int idx)
{
	return cswarp_readl(wc, WARP_OFFSET_DPREG_CR + 4 * idx);
}

static inline void dpreg_write(struct cswarp_core *wc, unsigned int idx, u32 val)
{
	cswarp_writel(wc, val, WARP_OFFSET_DPREG_CR + 4 * idx);
}

static const char *const board_names[] = {
	[WARP_BOARD_UNDEFINED]	= "undefined",
	[WARP_BOARD_530]	= "Warp530",
	[WARP_BOARD_540]	= "Warp540",
	[WARP_BOARD_560]	= "Warp560",
	[WARP_BOARD_1230]	= "Warp1230",
	[WARP_BOARD_1240]	= "Warp1240",
	[WARP_BOARD_1260]	= "Warp1260",
	[WARP_BOARD_7030]	= "Warp7030",
	[WARP_BOARD_7040]	= "Warp7040",
	[WARP_BOARD_7060]	= "Warp7060",
	[WARP_BOARD_CDTV30]	= "WarpCDTV30",
	[WARP_BOARD_CDTV40]	= "WarpCDTV40",
	[WARP_BOARD_CDTV60]	= "WarpCDTV60",
	[WARP_BOARD_1260X]	= "Warp1260X",
};

const char *cswarp_board_type_name(u8 type)
{
	if (type < ARRAY_SIZE(board_names) && board_names[type])
		return board_names[type];
	return "unknown";
}
EXPORT_SYMBOL_GPL(cswarp_board_type_name);

/* ------------------------------------------------------------------------ */
/* Interrupt                                                                */
/* ------------------------------------------------------------------------ */

/* Ring the doorbells in pend.  Called with IRQ_IF already cleared. */
static void cswarp_dispatch_units(struct cswarp_core *wc, u32 pend)
{
	unsigned int n;

	spin_lock(&wc->unit_lock);
	for (n = 0; n < AUNIT_MAX && pend; n++, pend >>= 1) {
		if (!(pend & 1))
			continue;
		wc->stats.unit_irqs[n]++;
		if (wc->units[n].fn)
			wc->units[n].fn(wc->units[n].ctx);
		else
			wc->stats.unit_unclaimed++;
	}
	spin_unlock(&wc->unit_lock);
}

static irqreturn_t cswarp_irq(int irq, void *data)
{
	struct cswarp_core *wc = data;
	bool handled = false, wake = false;
	u32 cr, pend;

	cr = dpreg_read(wc, DPREG_IDX_CR);
	if (cr & DPREG_CR_MR_ARM) {
		dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_CLR | DPREG_CR_MR_ARM);
		WRITE_ONCE(wc->frame_mr, 1);
		wake = true;
	}
	if (cr & DPREG_CR_MP_68K) {
		dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_CLR | DPREG_CR_MP_68K);
		WRITE_ONCE(wc->frame_mp, 1);
		wake = true;
	}
	if (cr & DPREG_CR_LEGACY_DOORBELLS) {
		/* nothing should set these; clear them or INT2 stays asserted */
		dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_CLR | DPREG_CR_LEGACY_DOORBELLS);
		dev_warn_ratelimited(wc->dev, "legacy CR doorbell bits set (CR 0x%08x), cleared\n", cr);
		handled = true;
	}
	if (wake) {
		wake_up(&wc->frame_wq);
		handled = true;
	}

	/* Clear before looking: an event raised after the clear re-asserts INT2 */
	pend = dpreg_read(wc, DPREG_IDX_IRQ_PEND);
	if (pend) {
		dpreg_write(wc, DPREG_IDX_IRQ_IF, DPREG_CLR | pend);
		cswarp_dispatch_units(wc, pend);
		handled = true;
	}

	if (cswarp_dbg_on(CSWARP_DBG_IRQ) && handled)
		dev_info(wc->dev, "dbg: irq CR 0x%08x PEND 0x%08x\n", cr, pend);

	if (!handled) {
		wc->stats.irqs_none++;
		return IRQ_NONE;
	}
	wc->stats.irqs++;
	return IRQ_HANDLED;
}

/* Doorbell polling, use_irq=0 only */
static void cswarp_poll_timer(struct timer_list *t)
{
	struct cswarp_core *wc = from_timer(wc, t, poll_timer);
	u32 pend;

	pend = dpreg_read(wc, DPREG_IDX_IRQ_PEND);
	if (pend) {
		dpreg_write(wc, DPREG_IDX_IRQ_IF, DPREG_CLR | pend);
		cswarp_dispatch_units(wc, pend);
	}
	mod_timer(&wc->poll_timer, jiffies + msecs_to_jiffies(CSWARP_DOORBELL_POLL_MS));
}

/* ------------------------------------------------------------------------ */
/* Frame exchange                                                           */
/* ------------------------------------------------------------------------ */

/*
 * Wait for *flag (set by the irq) or, as a fallback, for the CR bit itself:
 * with use_irq=0 that is the only way, with the irq on it catches a missed
 * interrupt and counts it, which is worth knowing during bring-up.
 */
static int cswarp_wait_flag(struct cswarp_core *wc, int *flag, u32 cr_bit,
			    unsigned long deadline)
{
	ktime_t spin_end = ktime_add_us(ktime_get(), spin_us);

	for (;;) {
		u32 cr;

		if (READ_ONCE(*flag))
			return 0;

		cr = dpreg_read(wc, DPREG_IDX_CR);
		if (cr & cr_bit) {
			/*
			 * The bit is set but the irq has not run (yet).  Take it
			 * ourselves while spinning, or count a missed irq once
			 * we are past the spin phase.
			 */
			if (!wc->use_irq || ktime_after(ktime_get(), spin_end)) {
				dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_CLR | cr_bit);
				if (wc->use_irq) {
					wc->stats.irq_missed++;
					dev_warn_ratelimited(wc->dev,
						"frame handshake bit 0x%x found by polling (irq missed?) CR 0x%08x\n",
						cr_bit, cr);
				}
				WRITE_ONCE(*flag, 1);
				return 0;
			}
		}

		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;

		if (ktime_before(ktime_get(), spin_end)) {
			udelay(2);
			continue;
		}
		if (wc->use_irq)
			wait_event_timeout(wc->frame_wq, READ_ONCE(*flag),
					   msecs_to_jiffies(CSWARP_POLL_SLICE_MS));
		else
			usleep_range(50, 200);
	}
}

static void cswarp_dump_frame(struct cswarp_core *wc, const char *why)
{
	u8 buf[32];

	z_memcpy_fromio(buf, cswarp_frame(wc), sizeof(buf));
	dev_err(wc->dev, "%s: cmd %u, CR 0x%08x, IRQ_IF 0x%08x, IRQ_IE 0x%08x\n",
		why, wc->last_cmd, dpreg_read(wc, DPREG_IDX_CR),
		dpreg_read(wc, DPREG_IDX_IRQ_IF), dpreg_read(wc, DPREG_IDX_IRQ_IE));
	print_hex_dump(KERN_ERR, "cswarp frame: ", DUMP_PREFIX_OFFSET, 16, 1,
		       buf, sizeof(buf), false);
}

void __iomem *cswarp_frame_lock(struct cswarp_core *wc)
{
	mutex_lock(&wc->frame_lock);
	if (unlikely(wc->broken)) {
		mutex_unlock(&wc->frame_lock);
		return IOMEM_ERR_PTR(-EIO);
	}
	return cswarp_frame(wc);
}
EXPORT_SYMBOL_GPL(cswarp_frame_lock);

void cswarp_frame_unlock(struct cswarp_core *wc)
{
	mutex_unlock(&wc->frame_lock);
}
EXPORT_SYMBOL_GPL(cswarp_frame_unlock);

int cswarp_frame_exchange(struct cswarp_core *wc, bool want_reply, u32 expect_rpl)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(READ_ONCE(xfer_timeout_ms));
	ktime_t t0 = 0;
	u32 cmd, rpl = CSWARP_RPL_ANY;
	int err;

	lockdep_assert_held(&wc->frame_lock);
	if (wc->broken)
		return -EIO;

	cmd = z_readl(cswarp_frame(wc));
	wc->last_cmd = cmd;
	wc->stats.exchanges++;
	if (cswarp_dbg_on(CSWARP_DBG_MBOX))
		t0 = ktime_get();

	/* stale handshake bits would end the waits below at once */
	dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_CLR | DPREG_CR_MR_ARM | DPREG_CR_MP_68K);
	WRITE_ONCE(wc->frame_mr, 0);
	WRITE_ONCE(wc->frame_mp, 0);

	dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_SET | DPREG_CR_IE_ARM |
		    (wc->use_irq ? DPREG_CR_IE_68K : 0));
	/* ring the ARM */
	dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_SET | DPREG_CR_MP_ARM);

	err = cswarp_wait_flag(wc, &wc->frame_mr, DPREG_CR_MR_ARM, deadline);
	if (!err && want_reply)
		err = cswarp_wait_flag(wc, &wc->frame_mp, DPREG_CR_MP_68K, deadline);

	dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_CLR | DPREG_CR_IE_ARM | DPREG_CR_IE_68K);

	if (err) {
		/*
		 * The ARM may still write its reply into the frame later; never
		 * let another exchange use it.  Recover with
		 * echo 1 > /sys/bus/zorro/devices/<ctrl>/recover
		 */
		wc->broken = true;
		wc->stats.timeouts++;
		cswarp_dump_frame(wc, want_reply && READ_ONCE(wc->frame_mr) ?
				  "reply timeout (ARM took the command)" :
				  "command timeout (ARM did not take the command)");
		dev_err(wc->dev, "frame disabled after timeout; all board requests now fail\n");
		return -ETIMEDOUT;
	}

	if (want_reply) {
		rpl = z_readl(cswarp_frame(wc));
		if (expect_rpl != CSWARP_RPL_ANY && rpl != expect_rpl) {
			wc->stats.bad_replies++;
			dev_err_ratelimited(wc->dev, "cmd %u: reply %u, expected %u\n",
					    cmd, rpl, expect_rpl);
			return -EPROTO;
		}
	}

	if (cswarp_dbg_on(CSWARP_DBG_MBOX))
		dev_info(wc->dev, "dbg: xfer cmd %u -> rpl %d in %lld us\n", cmd,
			 want_reply ? (int)rpl : -1,
			 ktime_us_delta(ktime_get(), t0));
	return 0;
}
EXPORT_SYMBOL_GPL(cswarp_frame_exchange);

int cswarp_xfer(struct cswarp_core *wc, const void *cmd, size_t clen,
		void *rpl, size_t rlen, u32 expect_rpl)
{
	void __iomem *frame;
	int err;

	if (WARN_ON(clen > DPR_FRAME_SIZE || rlen > DPR_FRAME_SIZE))
		return -EINVAL;

	frame = cswarp_frame_lock(wc);
	if (IS_ERR(frame))
		return PTR_ERR(frame);
	z_memcpy_toio(frame, cmd, clen);
	err = cswarp_frame_exchange(wc, rpl != NULL, expect_rpl);
	if (!err && rpl)
		z_memcpy_fromio(rpl, frame, rlen);
	cswarp_frame_unlock(wc);
	return err;
}
EXPORT_SYMBOL_GPL(cswarp_xfer);

/* ------------------------------------------------------------------------ */
/* Async units                                                              */
/* ------------------------------------------------------------------------ */

int cswarp_unit_register(struct cswarp_core *wc, unsigned int unit,
			 cswarp_unit_fn fn, void *ctx)
{
	unsigned long flags;

	if (unit >= AUNIT_MAX || !fn)
		return -EINVAL;

	spin_lock_irqsave(&wc->unit_lock, flags);
	if (wc->units[unit].fn) {
		spin_unlock_irqrestore(&wc->unit_lock, flags);
		return -EBUSY;
	}
	wc->units[unit].fn = fn;
	wc->units[unit].ctx = ctx;
	wc->unit_ie |= BIT(unit);
	/* clear first, enable second: the term is level sensitive */
	dpreg_write(wc, DPREG_IDX_IRQ_IF, DPREG_CLR | BIT(unit));
	dpreg_write(wc, DPREG_IDX_IRQ_IE, DPREG_SET | BIT(unit));
	spin_unlock_irqrestore(&wc->unit_lock, flags);

	cswarp_dbg(CSWARP_DBG_CORE, wc->dev, "unit %u registered, IRQ_IE 0x%08x\n",
		   unit, dpreg_read(wc, DPREG_IDX_IRQ_IE));
	return 0;
}
EXPORT_SYMBOL_GPL(cswarp_unit_register);

void cswarp_unit_unregister(struct cswarp_core *wc, unsigned int unit)
{
	unsigned long flags;

	if (unit >= AUNIT_MAX)
		return;

	spin_lock_irqsave(&wc->unit_lock, flags);
	dpreg_write(wc, DPREG_IDX_IRQ_IE, DPREG_CLR | BIT(unit));
	dpreg_write(wc, DPREG_IDX_IRQ_IF, DPREG_CLR | BIT(unit));
	wc->unit_ie &= ~BIT(unit);
	wc->units[unit].fn = NULL;
	wc->units[unit].ctx = NULL;
	spin_unlock_irqrestore(&wc->unit_lock, flags);

	cswarp_dbg(CSWARP_DBG_CORE, wc->dev, "unit %u unregistered\n", unit);
}
EXPORT_SYMBOL_GPL(cswarp_unit_unregister);

/* ------------------------------------------------------------------------ */
/* Identity                                                                 */
/* ------------------------------------------------------------------------ */

static void cswarp_fw_ver_str(u32 ver, char *buf, size_t len)
{
	if (ver == WARP_FMW_VER_DEVBUILD)
		snprintf(buf, len, "dev build");
	else
		/* release 30.54 is reported as 0x3054 */
		snprintf(buf, len, "%x%x.%02x", (ver >> 12) & 0xf, (ver >> 8) & 0xf, ver & 0xff);
}

/* Silence every source that can hold INT2 asserted before the handler is up */
static void cswarp_quiesce(struct cswarp_core *wc)
{
	cswarp_dbg(CSWARP_DBG_CORE, wc->dev,
		   "quiesce: CR 0x%08x IRQ_IF 0x%08x IRQ_IE 0x%08x vblank irq_sr 0x%08x irq_cr 0x%08x\n",
		   dpreg_read(wc, DPREG_IDX_CR), dpreg_read(wc, DPREG_IDX_IRQ_IF),
		   dpreg_read(wc, DPREG_IDX_IRQ_IE), cswarp_readl(wc, WARP_IRQ_SR),
		   cswarp_readl(wc, WARP_IRQ_CR));

	/*
	 * Legacy doorbells, our own enable and any stale handshake flags.
	 * MP_ARM (a message AmigaOS may have left for the ARM) is left alone;
	 * a reply to it is cleared before our first exchange.
	 */
	dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_CLR | DPREG_CR_LEGACY_DOORBELLS |
		    DPREG_CR_IE_68K | DPREG_CR_IE_ARM | DPREG_CR_MR_ARM | DPREG_CR_MP_68K);
	/* cswarp.library enables every unit; AmigaOS is gone now */
	dpreg_write(wc, DPREG_IDX_IRQ_IE, DPREG_CLR | DPREG_ALL_BITS);
	dpreg_write(wc, DPREG_IDX_IRQ_IF, DPREG_CLR | DPREG_ALL_BITS);
	/* P96 may have left the vblank irq enabled: disable and acknowledge */
	cswarp_writel(wc, WARP_IRQ_CR_VBLANK_CLR, WARP_IRQ_CR);
	wc->unit_ie = 0;

	cswarp_dbg(CSWARP_DBG_CORE, wc->dev,
		   "quiesced: CR 0x%08x IRQ_IF 0x%08x IRQ_IE 0x%08x irq_cr 0x%08x\n",
		   dpreg_read(wc, DPREG_IDX_CR), dpreg_read(wc, DPREG_IDX_IRQ_IF),
		   dpreg_read(wc, DPREG_IDX_IRQ_IE), cswarp_readl(wc, WARP_IRQ_CR));
}

static int cswarp_read_diag(struct cswarp_core *wc)
{
	DprCmdHeader cmd = { .cmd = dpcmdGetDiag };
	DprRplDiagMsg *diag;
	char ver[16];
	int err;

	diag = kzalloc(sizeof(*diag), GFP_KERNEL);
	if (!diag)
		return -ENOMEM;

	err = cswarp_xfer(wc, &cmd, sizeof(cmd), diag, sizeof(*diag), dprplDiagFrame);
	if (err) {
		dev_err(wc->dev, "dpcmdGetDiag failed (%d): no answer from the board's ARM\n", err);
		goto out;
	}

	wc->fw_ver = diag->fmwVer;
	wc->hw_ver = diag->hwVer;
	wc->board_type = diag->warpBoardType;
	wc->native_ide = diag->nativeIdeEnabled != 0;
	wc->ide_timing[0] = diag->IDE_IOR_as;
	wc->ide_timing[1] = diag->IDE_IOR_ng;
	wc->ide_timing[2] = diag->IDE_IOW_as;
	wc->ide_timing[3] = diag->IDE_IOW_ng;
	wc->ide_timing[4] = diag->IDE_ACK_as;
	strscpy(wc->board_name, diag->boardName, sizeof(wc->board_name));

	cswarp_fw_ver_str(wc->fw_ver, ver, sizeof(ver));
	dev_info(wc->dev, "%s (type %u: %s), hw 0x%x, firmware %s (0x%04x), IDE %s\n",
		 wc->board_name, wc->board_type, cswarp_board_type_name(wc->board_type),
		 wc->hw_ver, ver, wc->fw_ver, wc->native_ide ? "native" : "Gayle emulation");
	cswarp_dbg(CSWARP_DBG_CORE, wc->dev,
		   "diag: kick slot %u, usb %u/%u, wifi state %u rssi %d (valid %u) ch %u, L2 %u KB %u-way %u B lines, hdmi %u\n",
		   diag->kickstartNr, diag->usbDevStatus[0], diag->usbDevStatus[1],
		   diag->wifiState, diag->wifiRssi, diag->wifiRssiValid, diag->wifiChannel,
		   diag->l2CacheSizeKB, diag->l2CacheWays, diag->l2CacheLineBytes,
		   diag->hdmiEnabled);
	cswarp_dbg(CSWARP_DBG_CORE, wc->dev, "diag: IDE timing IOR %u/%u IOW %u/%u ACK %u\n",
		   wc->ide_timing[0], wc->ide_timing[1], wc->ide_timing[2],
		   wc->ide_timing[3], wc->ide_timing[4]);
out:
	kfree(diag);
	return err;
}

/* ------------------------------------------------------------------------ */
/* sysfs                                                                    */
/* ------------------------------------------------------------------------ */

static ssize_t firmware_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_core *wc = dev_get_drvdata(dev);
	char ver[16];

	cswarp_fw_ver_str(wc->fw_ver, ver, sizeof(ver));
	return sysfs_emit(buf, "%s\n", ver);
}
static DEVICE_ATTR_RO(firmware_version);

static ssize_t board_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_core *wc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s (%s) hw 0x%x fpga syscfg 0x%08x\n", wc->board_name,
			  cswarp_board_type_name(wc->board_type), wc->hw_ver, wc->syscfg_id);
}
static DEVICE_ATTR_RO(board);

static ssize_t native_ide_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_core *wc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", wc->native_ide);
}
static DEVICE_ATTR_RO(native_ide);

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_core *wc = dev_get_drvdata(dev);
	struct cswarp_stats *s = &wc->stats;
	int len, n;

	len = sysfs_emit(buf,
			 "exchanges %lu\ntimeouts %lu\nbad_replies %lu\nbroken %d\n"
			 "irqs %lu\nirqs_none %lu\nirq_missed %lu\nunit_unclaimed %lu\n"
			 "irq_ie 0x%08x\nuse_irq %d\n",
			 s->exchanges, s->timeouts, s->bad_replies, wc->broken,
			 s->irqs, s->irqs_none, s->irq_missed, s->unit_unclaimed,
			 wc->unit_ie, wc->use_irq);
	for (n = 0; n < AUNIT_MAX; n++)
		if (s->unit_irqs[n])
			len += sysfs_emit_at(buf, len, "unit%d_irqs %lu\n", n, s->unit_irqs[n]);
	return len;
}
static DEVICE_ATTR_RO(stats);

static ssize_t regs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_core *wc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "CR 0x%08x\nID 0x%08x\nIRQ_IF 0x%08x\nIRQ_IE 0x%08x\nIRQ_PEND 0x%08x\n"
			  "PIXCLK_SR 0x%08x\nIRQ_SR 0x%08x\nIRQ_CR 0x%08x\n",
			  dpreg_read(wc, DPREG_IDX_CR), dpreg_read(wc, DPREG_IDX_ID),
			  dpreg_read(wc, DPREG_IDX_IRQ_IF), dpreg_read(wc, DPREG_IDX_IRQ_IE),
			  dpreg_read(wc, DPREG_IDX_IRQ_PEND), cswarp_readl(wc, WARP_PIXCLK_SR),
			  cswarp_readl(wc, WARP_IRQ_SR), cswarp_readl(wc, WARP_IRQ_CR));
}
static DEVICE_ATTR_RO(regs);

/* Re-enable the frame after a timeout (bring-up aid) */
static ssize_t recover_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct cswarp_core *wc = dev_get_drvdata(dev);

	mutex_lock(&wc->frame_lock);
	dev_warn(dev, "recover: CR 0x%08x, re-enabling the frame\n", dpreg_read(wc, DPREG_IDX_CR));
	dpreg_write(wc, DPREG_IDX_CR, DPREG_CR_CLR | DPREG_CR_MR_ARM | DPREG_CR_MP_68K |
		    DPREG_CR_IE_68K | DPREG_CR_IE_ARM);
	wc->broken = false;
	mutex_unlock(&wc->frame_lock);
	return count;
}
static DEVICE_ATTR_WO(recover);

static struct attribute *cswarp_attrs[] = {
	&dev_attr_firmware_version.attr,
	&dev_attr_board.attr,
	&dev_attr_native_ide.attr,
	&dev_attr_stats.attr,
	&dev_attr_regs.attr,
	&dev_attr_recover.attr,
	NULL
};

static const struct attribute_group cswarp_attr_group = {
	.attrs = cswarp_attrs,
};

/* ------------------------------------------------------------------------ */
/* Probe                                                                    */
/* ------------------------------------------------------------------------ */

static const struct mfd_cell cswarp_cells[] = {
	{ .name = "cswarp-net" },
	{ .name = "cswarp-ata" },
	{ .name = "cswarp-fb" },
};

static int cswarp_probe(struct zorro_dev *z, const struct zorro_device_id *ent)
{
	struct cswarp_core *wc;
	u32 id;
	int err;

	wc = devm_kzalloc(&z->dev, sizeof(*wc), GFP_KERNEL);
	if (!wc)
		return -ENOMEM;

	wc->dev = &z->dev;
	wc->zdev = z;
	wc->phys = zorro_resource_start(z);
	wc->size = zorro_resource_len(z);
	wc->use_irq = use_irq;
	mutex_init(&wc->frame_lock);
	init_waitqueue_head(&wc->frame_wq);
	spin_lock_init(&wc->unit_lock);
	timer_setup(&wc->poll_timer, cswarp_poll_timer, 0);

	dev_info(wc->dev, "Warp CTRL board at 0x%08lx, size 0x%lx\n", wc->phys, wc->size);

	if (wc->size < WARP_OFFSET_ATA + WARP_ATA_WINDOW_SIZE) {
		dev_err(wc->dev, "CTRL board window too small (0x%lx)\n", wc->size);
		return -ENODEV;
	}

	if (!devm_request_mem_region(wc->dev, wc->phys, wc->size, DRV_NAME))
		dev_warn(wc->dev, "could not claim the CTRL board region, continuing\n");

	wc->base = z_ioremap(wc->phys, wc->size);
	if (!wc->base) {
		dev_err(wc->dev, "cannot map the CTRL board\n");
		return -ENOMEM;
	}
	cswarp_dbg(CSWARP_DBG_CORE, wc->dev, "CTRL mapped at %p\n", wc->base);

	/*
	 * An older bitstream aliases every even index onto CR and returns CR
	 * for every read; bit 31 of CR always reads 0, so the id cannot be
	 * faked.  Nothing but CR may be written before this matches.
	 */
	id = dpreg_read(wc, DPREG_IDX_ID);
	wc->syscfg_id = cswarp_readl(wc, WARP_SYSCFG_ID);
	dev_info(wc->dev, "FPGA: DPREG id 0x%08x, SysCfg id 0x%08x\n", id, wc->syscfg_id);
	if (id != DPREG_ID_VALUE) {
		dev_err(wc->dev, "FPGA image too old (no DPREG register file, id 0x%08x != 0x%08lx)\n",
			id, DPREG_ID_VALUE);
		dev_err(wc->dev, "update the Warp firmware package to 3.000 or later\n");
		err = -ENODEV;
		goto err_unmap;
	}

	cswarp_quiesce(wc);
	zorro_set_drvdata(z, wc);

	if (wc->use_irq) {
		err = request_irq(IRQ_AMIGA_PORTS, cswarp_irq, IRQF_SHARED, DRV_NAME, wc);
		if (err) {
			dev_err(wc->dev, "cannot get IRQ_AMIGA_PORTS (%d), falling back to polling\n", err);
			wc->use_irq = false;
		} else {
			cswarp_dbg(CSWARP_DBG_CORE, wc->dev, "INT2 handler installed (irq %d)\n",
				   IRQ_AMIGA_PORTS);
		}
	}
	if (!wc->use_irq) {
		dev_warn(wc->dev, "running without interrupts (polling)\n");
		mod_timer(&wc->poll_timer, jiffies + msecs_to_jiffies(CSWARP_DOORBELL_POLL_MS));
	}

	err = cswarp_read_diag(wc);
	if (err)
		goto err_irq;

	if (!cswarp_fw_at_least(wc, WARP_FMW_VER_MIN_LINUX)) {
		dev_err(wc->dev, "firmware 0x%04x too old: Linux needs release 3.000 (0x%04x) or later\n",
			wc->fw_ver, WARP_FMW_VER_MIN_LINUX);
		err = -ENODEV;
		goto err_irq;
	}

	err = devm_device_add_group(wc->dev, &cswarp_attr_group);
	if (err)
		dev_warn(wc->dev, "sysfs attributes not created (%d)\n", err);

	err = mfd_add_devices(wc->dev, PLATFORM_DEVID_NONE, cswarp_cells,
			      ARRAY_SIZE(cswarp_cells), NULL, 0, NULL);
	if (err) {
		dev_err(wc->dev, "cannot add function devices (%d)\n", err);
		goto err_irq;
	}
	cswarp_dbg(CSWARP_DBG_CORE, wc->dev, "function devices added: net, ata, fb\n");
	return 0;

err_irq:
	if (wc->use_irq)
		free_irq(IRQ_AMIGA_PORTS, wc);
	else
		del_timer_sync(&wc->poll_timer);
	cswarp_quiesce(wc);
	zorro_set_drvdata(z, NULL);
err_unmap:
	z_iounmap(wc->base);
	return err;
}

static void cswarp_remove(struct zorro_dev *z)
{
	struct cswarp_core *wc = zorro_get_drvdata(z);

	mfd_remove_devices(wc->dev);
	if (wc->use_irq)
		free_irq(IRQ_AMIGA_PORTS, wc);
	else
		del_timer_sync(&wc->poll_timer);
	cswarp_quiesce(wc);
	z_iounmap(wc->base);
	dev_info(wc->dev, "removed (%lu exchanges, %lu timeouts, %lu irqs)\n",
		 wc->stats.exchanges, wc->stats.timeouts, wc->stats.irqs);
}

static const struct zorro_device_id cswarp_zorro_tbl[] = {
	{ ZORRO_PROD_CSLAB_WARP_CTRL },
	{ 0 }
};
MODULE_DEVICE_TABLE(zorro, cswarp_zorro_tbl);

static struct zorro_driver cswarp_driver = {
	.name		= DRV_NAME,
	.id_table	= cswarp_zorro_tbl,
	.probe		= cswarp_probe,
	.remove		= cswarp_remove,
};

static int __init cswarp_init(void)
{
	return zorro_register_driver(&cswarp_driver);
}
module_init(cswarp_init);

static void __exit cswarp_exit(void)
{
	zorro_unregister_driver(&cswarp_driver);
}
module_exit(cswarp_exit);

MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("CS-Lab Warp turbo board core (dual-port RAM channel, INT2)");
MODULE_LICENSE("GPL");
