/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CS-Lab Warp turbo board - core driver interface.
 *
 * cswarp-core owns the Warp CTRL Zorro board: the register window, the one
 * 4 KB dual-port RAM command/reply frame shared with the board's ARM, and the
 * INT2 (IRQ_AMIGA_PORTS) handler.  The function drivers (cswarp-net,
 * cswarp-ata, cswarp-fb, cswarp-disk) are MFD children and reach the hardware only through
 * the helpers below - the Linux counterpart of cswarp.library.
 */
#ifndef _LINUX_MFD_CSWARP_H
#define _LINUX_MFD_CSWARP_H

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/zorro.h>

#include <asm/cswarpamicommdata.h>
#include <asm/cswarpdefs.h>

/*
 * Debug classes for the cswarp_core.dbg bitmask (kernel command line
 * "cswarp_core.dbg=0x..." or /sys/module/cswarp_core/parameters/dbg).
 * Messages print at KERN_INFO so they reach the console during bring-up.
 */
#define CSWARP_DBG_CORE		BIT(0)	/* probe, identity, children, recovery */
#define CSWARP_DBG_MBOX		BIT(1)	/* every frame exchange (noisy with net traffic) */
#define CSWARP_DBG_IRQ		BIT(2)	/* every INT2 entry (very noisy) */
#define CSWARP_DBG_NET		BIT(3)	/* netdev state, link, MAC, queue stops */
#define CSWARP_DBG_NETPKT	BIT(4)	/* every TX/RX frame (very noisy) */
#define CSWARP_DBG_FB		BIT(5)	/* mode set, pixel clock, pan */
#define CSWARP_DBG_FBACC	BIT(6)	/* every blitter operation (very noisy) */
#define CSWARP_DBG_ATA		BIT(7)	/* IDE probe and setup */
#define CSWARP_DBG_ATAIO	BIT(8)	/* every PIO data transfer (very noisy) */
#define CSWARP_DBG_DISK		BIT(9)	/* SD/USB disks: caps, media changes, errors */
#define CSWARP_DBG_DISKIO	BIT(10)	/* every disk request and unit wait (very noisy) */

#define CSWARP_DBG_DEFAULT	(CSWARP_DBG_CORE | CSWARP_DBG_NET | CSWARP_DBG_FB | CSWARP_DBG_ATA | \
				 CSWARP_DBG_DISK)

extern unsigned int cswarp_dbg_mask;

#define cswarp_dbg_on(cls)	unlikely(READ_ONCE(cswarp_dbg_mask) & (cls))
#define cswarp_dbg(cls, dev, fmt, ...)					\
	do {								\
		if (cswarp_dbg_on(cls))					\
			dev_info(dev, "dbg: " fmt, ##__VA_ARGS__);	\
	} while (0)

/* Expected reply value for an exchange that does not check the reply type */
#define CSWARP_RPL_ANY		0xffffffffU

typedef void (*cswarp_unit_fn)(void *ctx);

struct cswarp_unit_slot {
	cswarp_unit_fn fn;
	void *ctx;
};

struct cswarp_stats {
	unsigned long exchanges;	/* frame exchanges started */
	unsigned long timeouts;		/* exchanges that timed out */
	unsigned long bad_replies;	/* unexpected reply type */
	unsigned long irqs;		/* INT2 entries with something for us */
	unsigned long irqs_none;	/* INT2 entries with nothing for us (other PORTS users) */
	unsigned long irq_missed;	/* handshake bit still set after the grace period, taken by polling */
	unsigned long unit_irqs[AUNIT_MAX];
	unsigned long unit_unclaimed;	/* doorbells for units nobody registered */
	/* exchange timing (writing MP_ARM to the end of the wait) */
	u64 xfer_us_total;
	u32 xfer_us_max;
	unsigned long xfer_slept;	/* exchanges that had to sleep for the irq */
};

struct cswarp_core {
	struct device *dev;		/* the CTRL zorro device */
	struct zorro_dev *zdev;
	void __iomem *base;		/* CTRL board, uncached */
	unsigned long phys;
	unsigned long size;

	/* identity, from the FPGA and the dpcmdGetDiag reply */
	u32 syscfg_id;
	u32 fw_ver;
	u32 hw_ver;
	u8 board_type;			/* enum warp_board_type */
	char board_name[AMICOMM_BOARDNAME_LEN + 1];
	bool native_ide;
	u8 wifi_channel;		/* 2.4 GHz channel at probe, 0 = unknown/not connected */
	u8 ide_timing[5];		/* IOR as/ng, IOW as/ng, ACK as */
	u32 l2_line;			/* L2 cache line bytes (power of two) */

	/* the command/reply frame */
	struct mutex frame_lock;
	wait_queue_head_t frame_wq;
	int frame_mr;			/* set by the irq: ARM took the command */
	int frame_mp;			/* set by the irq: reply is in the frame */
	bool broken;			/* an exchange timed out: frame unusable */
	u32 last_cmd;
	unsigned long xfer_deadline;	/* of the exchange in flight */
	ktime_t xfer_t0;

	/* async unit doorbells */
	spinlock_t unit_lock;
	struct cswarp_unit_slot units[AUNIT_MAX];
	u32 unit_ie;			/* shadow of IRQ_IE */

	bool use_irq;
	struct timer_list poll_timer;	/* doorbell polling when use_irq is off */

	struct cswarp_stats stats;
};

static inline struct cswarp_core *cswarp_core_get(struct platform_device *pdev)
{
	return dev_get_drvdata(pdev->dev.parent);
}

static inline u32 cswarp_readl(struct cswarp_core *wc, unsigned long off)
{
	return z_readl(wc->base + off);
}

static inline void cswarp_writel(struct cswarp_core *wc, u32 val, unsigned long off)
{
	z_writel(val, wc->base + off);
}

/* Async unit n's UNIT_STATUS / UNIT_COUNT words (written by the ARM only) */
static inline u32 cswarp_unit_status(struct cswarp_core *wc, unsigned int unit)
{
	return cswarp_readl(wc, WARP_OFFSET_DPREG_CR + 4 * DPREG_IDX_UNIT_STATUS(unit));
}

static inline u32 cswarp_unit_count(struct cswarp_core *wc, unsigned int unit)
{
	return cswarp_readl(wc, WARP_OFFSET_DPREG_CR + 4 * DPREG_IDX_UNIT_COUNT(unit));
}

/*
 * QSDMA engine.  One engine for every user: arm it only while holding the
 * frame, for the exchange that makes the board feed or drain it.
 */
static inline void cswarp_qsdma_arm(struct cswarp_core *wc, u32 word, u32 words, bool to_board)
{
	cswarp_writel(wc, word, WARP_QSDMA_MEMADDR);
	cswarp_writel(wc, words, WARP_QSDMA_TRNUMB);
	cswarp_writel(wc, 0, WARP_QSDMA_MODULO);
	cswarp_writel(wc, 0, WARP_QSDMA_MODINC);
	cswarp_writel(wc, QSDMA_CSR_START | (to_board ? QSDMA_CSR_DIR_MTQ : 0), WARP_QSDMA_CSR);
}

static inline u32 cswarp_qsdma_csr(struct cswarp_core *wc)
{
	return cswarp_readl(wc, WARP_QSDMA_CSR);
}

/* Write back and invalidate the board's L2 cache (QSDMA bypasses it) */
static inline void cswarp_l2_flush(struct cswarp_core *wc)
{
	cswarp_writel(wc, SYSCFG_CCTRL_L2FLUSHINV, WARP_SYSCFG_CCTRL);
}

static inline void __iomem *cswarp_frame(struct cswarp_core *wc)
{
	return wc->base + WARP_OFFSET_DPRAM;
}

static inline bool cswarp_fw_at_least(struct cswarp_core *wc, u32 ver)
{
	return wc->fw_ver == WARP_FMW_VER_DEVBUILD || wc->fw_ver >= ver;
}

/*
 * Frame access.  Sleeps; process context only.
 *
 *   frame = cswarp_frame_lock(wc);       (ERR_PTR(-EIO) once the frame broke)
 *   ... write the command into frame ...
 *   err = cswarp_frame_exchange(wc, want_reply, expect_rpl);
 *   ... read the reply from frame ...
 *   cswarp_frame_unlock(wc);
 *
 * Without want_reply the exchange returns once the ARM has taken the command
 * (MR); the frame is then free again.  With it, it also waits for the reply
 * (MP) and checks its type against expect_rpl (CSWARP_RPL_ANY: no check).
 *
 * An exchange may also be split: cswarp_frame_exchange_start() rings the ARM
 * and returns at once, so the caller can work while the ARM serves it (say,
 * copy out the previous QSDMA buffer), then cswarp_frame_exchange_finish()
 * waits as cswarp_frame_exchange() would.  The frame stays locked meanwhile.
 */
void __iomem *cswarp_frame_lock(struct cswarp_core *wc);
int cswarp_frame_exchange(struct cswarp_core *wc, bool want_reply, u32 expect_rpl);
int cswarp_frame_exchange_start(struct cswarp_core *wc);
int cswarp_frame_exchange_finish(struct cswarp_core *wc, bool want_reply, u32 expect_rpl);
void cswarp_frame_unlock(struct cswarp_core *wc);

/* Copy cmd in, exchange, copy rlen bytes of the reply out (rpl may be NULL). */
int cswarp_xfer(struct cswarp_core *wc, const void *cmd, size_t clen,
		void *rpl, size_t rlen, u32 expect_rpl);

/*
 * Async unit doorbells.  fn runs in hard-irq context (or from the poll timer
 * when the irq is disabled): it may only schedule work.  Registering enables
 * the unit's IRQ_IE bit, unregistering disables it.
 */
int cswarp_unit_register(struct cswarp_core *wc, unsigned int unit,
			 cswarp_unit_fn fn, void *ctx);
void cswarp_unit_unregister(struct cswarp_core *wc, unsigned int unit);

const char *cswarp_board_type_name(u8 type);

#endif /* _LINUX_MFD_CSWARP_H */
