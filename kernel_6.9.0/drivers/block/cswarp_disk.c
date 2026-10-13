// SPDX-License-Identifier: GPL-2.0
/*
 * CS-Lab Warp turbo board - SD card and USB mass-storage disks
 *
 * Copyright (c) 2026 CS-Lab s.c.
 *		http://www.cs-lab.eu
 *
 * The board's ARM owns the SD card slot and the USB host ports; it offers
 * each medium to the 68k as an async unit (AUNIT_SD, AUNIT_USB0/1), the same
 * interface warpSD.device and warpUSBDisk.device use through cswarp.library
 * (WarpAsync.c).  One operation per unit:
 *
 *  - read:  SUBMIT UOP_READ for the whole request.  The board reads into one
 *    half of the unit's staging buffer while we fetch the other: chunk k is
 *    announced as UNIT_STATUS READY k with its size in UNIT_COUNT, fetched
 *    through the frame in pieces of at most UNIT_INLINE_MAX bytes, and the
 *    last fetch RELEASEs it.  A device error is announced as DONE instead.
 *  - write: SUBMIT UOP_WRITE carries chunk 0 (UNIT_WR_INLINE_CHUNK bytes),
 *    UOP_WDATA submits carry chunk k >= 1; chunk k >= 2 waits for READY k
 *    (chunk k-2 is on the device).  The board ends it with DONE.
 *
 * The unit's IRQ_IF bit is the doorbell for every status change; a waiter
 * also re-reads the status every 100 ms in case one is missed.  The frame is
 * held only for one exchange at a time, never while a device works.
 *
 * A request larger than one frame goes over the QSDMA engine (dma=1, the
 * default), a whole chunk per fetch or write submit, as cswarp.library does
 * it; smaller ones, and all of them with dma=0, move through the frame (PIO).
 * The engine wants one physically contiguous run of Warp RAM per transfer
 * and a request is a list of pages: a read chunk goes straight into the
 * pages when they form a few such runs, anything else is staged in a
 * per-unit bounce buffer of two staging halves (dpcmdUnitCaps).  Every DMA
 * target must lie in Warp RAM - the DDR3 Zorro board's 256 MB area, or the
 * CPU slot area at 0x08000000 (7060) - and own whole L2 lines.  Around every
 * transfer the CPU caches are pushed or invalidated and the board's L2 cache
 * (which the engine bypasses) is written back and invalidated.
 *
 * Media come and go: a worker polls dpcmdSDGetInfo and dpcmdUSBDiskGetUnits
 * once a second and adds a disk when a medium appears and deletes it when
 * the medium goes (or a different one is found in the slot).  Disks are
 * warpsd, warpusb0 and warpusb1; partitions (MBR, RDB, ...) follow the usual
 * rules (warpsd1, warpusb0p1).
 */

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/mfd/cswarp.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/zorro.h>

#include <asm/cacheflush.h>
#include <asm/setup.h>

#define DRV_NAME	"cswarp-disk"

static unsigned int poll_ms = 1000;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Media change polling interval in ms (0: scan only at probe)");

static unsigned int io_timeout_ms = 30000;
module_param(io_timeout_ms, uint, 0644);
MODULE_PARM_DESC(io_timeout_ms, "Give up on a unit that announces nothing for this long (ms)");

static bool dma = true;
module_param(dma, bool, 0644);
MODULE_PARM_DESC(dma, "Move requests larger than one frame over the QSDMA engine (default on; 0: all through the frame)");

/*
 * Long requests let the board read the next chunk while the 68k fetches this
 * one; between requests the device idles.  The readahead window follows it.
 */
/*
 * A DMA read chunk goes straight into the request's pages when they form few
 * enough physically contiguous runs (each costs a fetch exchange, ~0.2 ms);
 * otherwise through the bounce buffer, at one CPU copy.  Page cache reads
 * usually come in runs of 100 KB and more, O_DIRECT user buffers in pages.
 */
static unsigned int zc_min_kb = 32;
module_param(zc_min_kb, uint, 0644);
MODULE_PARM_DESC(zc_min_kb, "DMA reads skip the bounce buffer when the pages average runs of this many KB (0: never)");

static unsigned int max_kb = 1024;
module_param(max_kb, uint, 0444);
MODULE_PARM_DESC(max_kb, "Largest request in KB (also the readahead window)");

#define CWD_MINORS		16
#define CWD_BUSY_RETRY_MS	20	/* a unit still finishing a cancelled operation */
#define CWD_BUSY_RETRIES	3000	/* ... for up to a minute, as cswarp.library */
#define CWD_STATUS_POLL_MS	100
#define CWD_QSDMA_TC_POLLS	100000	/* reads of the CSR before giving up on TC */

enum { CWD_SLOT_SD, CWD_SLOT_USB0, CWD_SLOT_USB1, CWD_SLOTS };

struct cwd_stats {
	unsigned long reads;
	unsigned long writes;
	u64 bytes_read;
	u64 bytes_written;
	unsigned long errors;
	unsigned long busy_retries;
	unsigned long timeouts;
	unsigned long cancels;
	unsigned long media_changes;
	unsigned long dma_reads;	/* chunks fetched over QSDMA */
	unsigned long dma_writes;	/* chunks sent over QSDMA */
	unsigned long dma_errors;
	/* where a request's time goes, in us (reset by writing disk_stats) */
	u64 us_submit;			/* submit exchanges */
	u64 us_wait;			/* waiting for the board to announce */
	u64 us_xfer;			/* fetch exchanges (PIO: with the copy) */
	u64 us_tc;			/* waiting for QSDMA TC */
	u64 us_cache;			/* CPU cache and L2 maintenance */
	u64 us_copy;			/* bounce buffer copies */
	u64 us_total;			/* whole requests */
	unsigned long zc_chunks;	/* DMA read chunks fetched straight into the request */
	unsigned long zc_runs;		/* ... in this many fetches */
	unsigned long zc_frag;		/* bounced: too many runs */
	unsigned long zc_align;		/* bounced: a run off L2 lines or out of reach */
};

/* Add the time since *t to bucket, and restart *t */
static inline void cwd_tick(u64 *bucket, ktime_t *t)
{
	ktime_t now = ktime_get();

	*bucket += ktime_us_delta(now, *t);
	*t = now;
}

struct cwd_slot {
	struct cswarp_disk *cd;
	unsigned int idx;		/* CWD_SLOT_* */
	unsigned int unit;		/* AUNIT_* */
	char name[DISK_NAME_LEN];
	bool avail;			/* the firmware implements the unit */
	u32 chunk_bytes;		/* staging half, dpcmdUnitCaps */

	/*
	 * QSDMA bounce buffer, two halves of chunk_bytes, allocated on first use.
	 * A read fetches chunk k into half k & 1 while chunk k - 1 is copied out
	 * of the other; a write stages every chunk in half 0.
	 */
	void *bounce;
	unsigned long bounce_phys;
	bool bounce_failed;		/* not allocatable / not in Warp RAM: stay PIO */

	struct blk_mq_tag_set tag_set;
	bool tag_set_ok;
	bool unit_registered;

	struct mutex io_lock;		/* one operation per unit */
	wait_queue_head_t wq;		/* doorbell */
	u16 seq;

	/* medium, as last seen by the poll worker (which alone changes these) */
	struct gendisk *disk;
	u64 sectors;
	u32 change_cnt;
	bool warned_bsize;

	struct cwd_stats stats;
};

struct cswarp_disk {
	struct device *dev;
	struct cswarp_core *wc;
	int major;
	unsigned long ddr3_start;	/* the Warp DDR3 Zorro board, 0 if none */
	unsigned long ddr3_end;		/* inclusive */
	struct cwd_slot slots[CWD_SLOTS];
	struct delayed_work poll_work;
};

/* ------------------------------------------------------------------------ */
/* Unit protocol                                                            */
/* ------------------------------------------------------------------------ */

static u16 cwd_next_seq(struct cwd_slot *s)
{
	if (++s->seq == 0)
		++s->seq;
	return s->seq;
}

/*
 * RAM to RAM copy for the bounce buffer.  On the 040/060 move16 moves whole
 * lines without the data cache (the line in the cache, if any, is pushed or
 * invalidated), about twice the speed of memcpy on Warp RAM.
 */
static void cwd_copy(void *dst, const void *src, unsigned int len)
{
	unsigned int lines;

	if (!CPU_IS_040_OR_060 || (((unsigned long)dst | (unsigned long)src | len) & 15) || !len) {
		memcpy(dst, src, len);
		return;
	}
	lines = len / 16;
	__asm__ __volatile__(".chip 68040\n"
			     "1:\tmove16 %1@+,%0@+\n\t"
			     "subql #1,%2\n\t"
			     "jne 1b\n\t"
			     ".chip 68k"
			     : "+a" (dst), "+a" (src), "+d" (lines)
			     : : "memory", "cc");
}

/*
 * Copy len bytes between the request's data at byte offset pos and io (the
 * frame), or buf when io is NULL (the bounce buffer).
 */
static void cwd_rq_copy(struct request *rq, unsigned int pos, void __iomem *io, void *buf,
			unsigned int len, bool to_dev)
{
	struct req_iterator iter;
	struct bio_vec bv;
	unsigned int off = 0;

	rq_for_each_segment(bv, rq, iter) {
		unsigned int skip, n;
		void *p;

		if (off + bv.bv_len <= pos) {
			off += bv.bv_len;
			continue;
		}
		skip = pos - off;
		n = min(bv.bv_len - skip, len);
		p = bvec_kmap_local(&bv);
		if (to_dev) {
			if (io)
				z_memcpy_toio(io, p + skip, n);
			else
				cwd_copy(buf, p + skip, n);
		} else {
			if (io)
				z_memcpy_fromio(p + skip, io, n);
			else
				cwd_copy(p + skip, buf, n);
			/* the I-cache does not see what the CPU wrote */
			flush_dcache_page(bv.bv_page);
		}
		kunmap_local(p);
		if (io)
			io += n;
		else
			buf += n;
		pos += n;
		len -= n;
		off += bv.bv_len;
		if (!len)
			break;
	}
	WARN_ON_ONCE(len);
}

/* ------------------------------------------------------------------------ */
/* QSDMA                                                                    */
/* ------------------------------------------------------------------------ */

/* L2 line size; a core that never got the diag reply leaves it 0 */
static unsigned long cwd_l2_line(struct cswarp_disk *cd)
{
	return cd->wc->l2_line ? cd->wc->l2_line : 128;
}

/* May the engine reach [phys, phys + len)?  Same rule as isItWarpRAMAddr() */
static bool cwd_warp_ram(struct cswarp_disk *cd, unsigned long phys, unsigned long len)
{
	unsigned long last = phys + len - 1;

	if ((phys & 0xf8000000) == 0x08000000 && (last & 0xf8000000) == 0x08000000)
		return true;
	return cd->ddr3_start && phys >= cd->ddr3_start && last <= cd->ddr3_end &&
	       (phys & 0xf0000000) == (last & 0xf0000000);
}

/* The slot's bounce buffer, allocated on first use; NULL means stay PIO */
static void *cwd_bounce(struct cwd_slot *s)
{
	struct cswarp_disk *cd = s->cd;
	void *buf;

	if (s->bounce || s->bounce_failed)
		return s->bounce;
	s->bounce_failed = true;
	if (!s->chunk_bytes || s->chunk_bytes % 16)
		return NULL;
	/* in the I/O path: reclaim must not write back to this very disk */
	buf = kmalloc(2 * s->chunk_bytes, GFP_NOIO | __GFP_NOWARN);
	if (!buf) {
		dev_warn(cd->dev, "%s: no %u-byte DMA buffer, staying with PIO\n", s->name, 2 * s->chunk_bytes);
		return NULL;
	}
	/* whole L2 lines, as for the requests' own pages (cwd_zc_map) */
	if (((unsigned long)buf & (cwd_l2_line(cd) - 1)) ||
	    !cwd_warp_ram(cd, virt_to_phys(buf), 2 * s->chunk_bytes)) {
		dev_warn(cd->dev, "%s: DMA buffer at 0x%08lx is not in Warp RAM, staying with PIO\n",
			 s->name, (unsigned long)virt_to_phys(buf));
		kfree(buf);
		return NULL;
	}
	s->bounce = buf;
	s->bounce_phys = virt_to_phys(buf);
	s->bounce_failed = false;
	cswarp_dbg(CSWARP_DBG_DISK, cd->dev, "%s: DMA buffer %u bytes at 0x%08lx\n",
		   s->name, 2 * s->chunk_bytes, s->bounce_phys);
	return buf;
}

/* Use QSDMA for this request? */
static bool cwd_use_dma(struct cwd_slot *s, u32 total)
{
	return READ_ONCE(dma) && total > UNIT_INLINE_MAX && cwd_bounce(s);
}

/*
 * Cache maintenance before the engine touches len bytes of the bounce
 * buffer: dirty CPU lines go to memory (through the L2), then the L2 is
 * written back and invalidated, so nothing stale or dirty sits above the
 * DDR3 for the engine to miss or for a later write-back to overwrite.
 */
static void cwd_dma_pre(struct cwd_slot *s, unsigned long phys, u32 len)
{
	ktime_t t = ktime_get();

	cache_push(phys, len);
	cswarp_l2_flush(s->cd->wc);
	cwd_tick(&s->stats.us_cache, &t);
}

/* After the engine wrote into the bounce buffer: drop what the caches hold */
static void cwd_dma_post_read(struct cwd_slot *s, unsigned long phys, u32 len)
{
	ktime_t t = ktime_get();

	cache_clear(phys, len);
	cswarp_l2_flush(s->cd->wc);
	cwd_tick(&s->stats.us_cache, &t);
}

/*
 * The board's reply only says it has pushed the last byte into the FPGA; the
 * engine may still be writing its FIFO out.  TC says it is done.
 */
static bool cwd_qsdma_wait_tc(struct cwd_slot *s)
{
	struct cswarp_core *wc = s->cd->wc;
	ktime_t t = ktime_get();
	unsigned int i;
	u32 csr = 0;

	for (i = 0; i < CWD_QSDMA_TC_POLLS; i++) {
		csr = cswarp_qsdma_csr(wc);
		if (csr & QSDMA_CSR_TC)
			break;
	}
	cwd_tick(&s->stats.us_tc, &t);
	if ((csr & QSDMA_CSR_TC) && !(csr & QSDMA_CSR_ERR_MASK))
		return true;
	s->stats.dma_errors++;
	dev_err_ratelimited(s->cd->dev, "%s: QSDMA transfer did not complete (csr 0x%08x)\n",
			    s->name, csr);
	return false;
}

/*
 * Wait until the unit reports on operation seq: DONE, or READY for chunk
 * (exactly, or with or_later also a later one, modulo 256 - at most two
 * chunks are ever outstanding).  chunk < 0 waits for DONE only.
 */
static int cwd_await(struct cwd_slot *s, u16 seq, int chunk, bool or_later, u32 *status)
{
	struct cswarp_core *wc = s->cd->wc;
	unsigned long deadline = jiffies + msecs_to_jiffies(READ_ONCE(io_timeout_ms));
	ktime_t t = ktime_get();

	for (;;) {
		u32 st = cswarp_unit_status(wc, s->unit);

		if (USTAT_SEQ(st) == seq) {
			u32 ph = USTAT_PHASE(st);

			if (ph == UPHASE_DONE) {
				*status = st;
				cwd_tick(&s->stats.us_wait, &t);
				return 0;
			}
			if (ph == UPHASE_READY && chunk >= 0) {
				u32 d = (USTAT_CHUNK(st) - (u32)chunk) & 0xff;

				if (d == 0 || (or_later && d < 0x80)) {
					*status = st;
					cwd_tick(&s->stats.us_wait, &t);
					return 0;
				}
			}
		}
		if (time_after(jiffies, deadline)) {
			s->stats.timeouts++;
			dev_err_ratelimited(s->cd->dev, "%s: no answer from the board for seq %u chunk %d (status 0x%08x)\n",
					    s->name, seq, chunk, st);
			return -ETIMEDOUT;
		}
		wait_event_timeout(s->wq, cswarp_unit_status(wc, s->unit) != st,
				   max_t(unsigned long, msecs_to_jiffies(CWD_STATUS_POLL_MS), 2));
	}
}

/*
 * One submit exchange.  dlen bytes of the request's data from dpos go in the
 * frame.  *res is the ack's URES_*, *chunk the stream's chunk size.  A unit
 * still busy with an earlier (cancelled) operation is retried.
 */
static int cwd_submit(struct cwd_slot *s, u8 op, u16 seq, u32 addr, u32 total,
		      struct request *rq, unsigned int dpos, unsigned int dlen, bool use_dma,
		      u8 *res, u32 *chunk)
{
	struct cswarp_core *wc = s->cd->wc;
	unsigned int tries;
	ktime_t t = ktime_get();

	/*
	 * QSDMA payload: staged in the bounce buffer, which is 16-byte aligned,
	 * so there is no unaligned head or tail to carry in data[].  The board
	 * pulls it before it acks.
	 */
	if (use_dma && dlen) {
		cwd_rq_copy(rq, dpos, NULL, s->bounce, dlen, true);
		cwd_tick(&s->stats.us_copy, &t);
		cwd_dma_pre(s, s->bounce_phys, dlen);
		t = ktime_get();
	}

	for (tries = 0;; tries++) {
		void __iomem *f = cswarp_frame_lock(wc);
		bool armed = use_dma && dlen;
		int err;

		if (IS_ERR(f))
			return PTR_ERR(f);
		if (armed)
			cswarp_qsdma_arm(wc, (s->bounce_phys & 0x0fffffff) >> 4, dlen >> 4, true);
		z_writel(dpcmdUnitSubmit, f + offsetof(DprCmdUnitSubmit, header));
		z_writeb(s->unit, f + offsetof(DprCmdUnitSubmit, unit));
		z_writeb(op, f + offsetof(DprCmdUnitSubmit, op));
		z_writeb(armed ? 1 : 0, f + offsetof(DprCmdUnitSubmit, dmaEnable));
		z_writeb(0, f + offsetof(DprCmdUnitSubmit, dmaAlgn));
		z_writew(seq, f + offsetof(DprCmdUnitSubmit, seq));
		z_writew(DISK_BLOCKSIZE, f + offsetof(DprCmdUnitSubmit, blockSize));
		z_writel(addr, f + offsetof(DprCmdUnitSubmit, blockAddr));
		z_writel(total, f + offsetof(DprCmdUnitSubmit, totalBytes));
		z_writel(armed ? 0 : dlen, f + offsetof(DprCmdUnitSubmit, dataLen));
		if (dlen && !armed)
			cwd_rq_copy(rq, dpos, f + offsetof(DprCmdUnitSubmit, data), NULL, dlen, true);

		err = cswarp_frame_exchange(wc, true, dprplUnitAck);
		if (!err && armed) {
			u32 csr = cswarp_qsdma_csr(wc);

			/* a board that refused never pulled; anything else should be done */
			if (z_readb(f + offsetof(DprRplUnitAck, result)) != URES_OK) {
				/* nothing moved */
			} else if (!(csr & QSDMA_CSR_TC) || (csr & QSDMA_CSR_ERR_MASK)) {
				s->stats.dma_errors++;
				dev_warn_ratelimited(s->cd->dev, "%s: QSDMA csr 0x%08x after a write chunk\n",
						     s->name, csr);
			} else {
				s->stats.dma_writes++;
			}
		}
		if (!err) {
			*res = z_readb(f + offsetof(DprRplUnitAck, result));
			*chunk = z_readl(f + offsetof(DprRplUnitAck, chunkBytes));
			if (z_readw(f + offsetof(DprRplUnitAck, seq)) != seq)
				*res = URES_BADPARAM;
		}
		cswarp_frame_unlock(wc);
		cwd_tick(&s->stats.us_submit, &t);
		if (err)
			return err;

		if (*res != URES_BUSY || op == UOP_WDATA || tries >= CWD_BUSY_RETRIES)
			return 0;
		s->stats.busy_retries++;
		msleep(CWD_BUSY_RETRY_MS);
		t = ktime_get();
	}
}

/* Fetch len bytes of chunk at offset into the request at rpos */
static int cwd_fetch(struct cwd_slot *s, u16 seq, u32 chunk, u32 offset, u32 len,
		     struct request *rq, unsigned int rpos, bool release)
{
	struct cswarp_core *wc = s->cd->wc;
	ktime_t t = ktime_get();
	void __iomem *f;
	u8 res;
	int err;

	f = cswarp_frame_lock(wc);
	if (IS_ERR(f))
		return PTR_ERR(f);
	z_writel(dpcmdUnitFetch, f + offsetof(DprCmdUnitFetch, header));
	z_writeb(s->unit, f + offsetof(DprCmdUnitFetch, unit));
	z_writeb(release ? UFETCH_RELEASE : 0, f + offsetof(DprCmdUnitFetch, flags));
	z_writeb(0, f + offsetof(DprCmdUnitFetch, dmaAlgn));
	z_writeb(0, f + offsetof(DprCmdUnitFetch, reserved));
	z_writew(seq, f + offsetof(DprCmdUnitFetch, seq));
	z_writew(chunk, f + offsetof(DprCmdUnitFetch, chunk));
	z_writel(offset, f + offsetof(DprCmdUnitFetch, offset));
	z_writel(len, f + offsetof(DprCmdUnitFetch, len));

	err = cswarp_frame_exchange(wc, true, dprplUnitData);
	if (!err) {
		res = z_readb(f + offsetof(DprRplUnitData, result));
		if (res != URES_OK || z_readw(f + offsetof(DprRplUnitData, seq)) != seq ||
		    z_readl(f + offsetof(DprRplUnitData, len)) != len) {
			dev_err_ratelimited(s->cd->dev, "%s: fetch of chunk %u +%u refused (result %u)\n",
					    s->name, chunk, offset, res);
			err = -EIO;
		} else {
			cwd_rq_copy(rq, rpos, f + offsetof(DprRplUnitData, data), NULL, len, false);
		}
	}
	cswarp_frame_unlock(wc);
	cwd_tick(&s->stats.us_xfer, &t);
	return err;
}

/* A chunk fetched into a bounce half, still to be copied into the request */
struct cwd_pending {
	void *buf;
	u32 len;
	unsigned int rpos;
};

static void cwd_pending_copy(struct cwd_slot *s, struct request *rq, struct cwd_pending *p)
{
	ktime_t t;

	if (!p->len)
		return;
	t = ktime_get();
	cwd_rq_copy(rq, p->rpos, NULL, p->buf, p->len, false);
	cwd_tick(&s->stats.us_copy, &t);
	p->len = 0;
}

/*
 * Fetch len bytes at offset of chunk over QSDMA to phys (16-byte aligned, in
 * Warp RAM), releasing the chunk with release.  A pending bounce half is
 * copied into the request while the ARM pushes.  Caches are handled around.
 */
static int cwd_fetch_dma_run(struct cwd_slot *s, u16 seq, u32 chunk, u32 offset, u32 len,
			     unsigned long phys, bool release, struct request *rq,
			     struct cwd_pending *p)
{
	struct cswarp_core *wc = s->cd->wc;
	void __iomem *f;
	u8 res = URES_OK;
	ktime_t t;
	int err;

	cwd_dma_pre(s, phys, len);
	t = ktime_get();
	f = cswarp_frame_lock(wc);
	if (IS_ERR(f))
		return PTR_ERR(f);
	cswarp_qsdma_arm(wc, (phys & 0x0fffffff) >> 4, len >> 4, false);
	z_writel(dpcmdUnitFetch, f + offsetof(DprCmdUnitFetch, header));
	z_writeb(s->unit, f + offsetof(DprCmdUnitFetch, unit));
	z_writeb(UFETCH_DMA | (release ? UFETCH_RELEASE : 0), f + offsetof(DprCmdUnitFetch, flags));
	z_writeb(0, f + offsetof(DprCmdUnitFetch, dmaAlgn));
	z_writeb(0, f + offsetof(DprCmdUnitFetch, reserved));
	z_writew(seq, f + offsetof(DprCmdUnitFetch, seq));
	z_writew(chunk, f + offsetof(DprCmdUnitFetch, chunk));
	z_writel(offset, f + offsetof(DprCmdUnitFetch, offset));
	z_writel(len, f + offsetof(DprCmdUnitFetch, len));

	err = cswarp_frame_exchange_start(wc);
	if (!err) {
		/* the engine writes elsewhere meanwhile */
		cwd_tick(&s->stats.us_xfer, &t);
		cwd_pending_copy(s, rq, p);
		t = ktime_get();
		err = cswarp_frame_exchange_finish(wc, true, dprplUnitData);
	}
	if (!err) {
		/* aligned buffer: no head and tail bytes come back in data[] */
		res = z_readb(f + offsetof(DprRplUnitData, result));
		if (res != URES_OK || z_readw(f + offsetof(DprRplUnitData, seq)) != seq ||
		    z_readl(f + offsetof(DprRplUnitData, len)) != 0) {
			dev_err_ratelimited(s->cd->dev, "%s: DMA fetch of chunk %u +%u refused (result %u)\n",
					    s->name, chunk, offset, res);
			err = -EIO;
		} else {
			cwd_tick(&s->stats.us_xfer, &t);
			if (!cwd_qsdma_wait_tc(s))
				err = -EIO;
		}
	}
	cswarp_frame_unlock(wc);
	if (err)
		cwd_tick(&s->stats.us_xfer, &t);

	/* also after a failure: the engine may have written part of it */
	cwd_dma_post_read(s, phys, len);
	return err;
}

#define CWD_ZC_MAX_RUNS	16

struct cwd_run {
	unsigned long phys;
	u32 len;
};

/*
 * The request's bytes [pos, pos + len) as physically contiguous runs the
 * engine can write, at most max of them.  0 when they do not qualify.
 */
static int cwd_zc_map(struct cwd_slot *s, struct request *rq, unsigned int pos, unsigned int len,
		      struct cwd_run *runs, int max)
{
	struct req_iterator iter;
	struct bio_vec bv;
	unsigned int off = 0;
	int n = 0, i;

	rq_for_each_segment(bv, rq, iter) {
		unsigned long phys;
		unsigned int skip, k;

		if (off + bv.bv_len <= pos) {
			off += bv.bv_len;
			continue;
		}
		skip = pos - off;
		k = min(bv.bv_len - skip, len);
		phys = page_to_phys(bv.bv_page) + bv.bv_offset + skip;
		if (n && runs[n - 1].phys + runs[n - 1].len == phys) {
			runs[n - 1].len += k;
		} else {
			if (n == max) {
				s->stats.zc_frag++;
				return 0;
			}
			runs[n].phys = phys;
			runs[n].len = k;
			n++;
		}
		pos += k;
		len -= k;
		off += bv.bv_len;
		if (!len)
			break;
	}
	if (len)
		return 0;
	/*
	 * Whole L2 lines only: a line shared with other memory can be dirtied
	 * by a CPU write there during the transfer, and the flush after it
	 * would write the line's stale copy of our bytes over the DMA data.
	 */
	for (i = 0; i < n; i++)
		if (((runs[i].phys | runs[i].len) & (cwd_l2_line(s->cd) - 1)) ||
		    !cwd_warp_ram(s->cd, runs[i].phys, runs[i].len)) {
			s->stats.zc_align++;
			return 0;
		}
	return n;
}

/*
 * Fetch read chunk k (len bytes, request offset rpos) over QSDMA, and release
 * it: straight into the request when its pages allow, else into bounce half
 * k & 1, left in *p for the next fetch (or the caller) to copy out.
 */
static int cwd_fetch_dma(struct cwd_slot *s, u16 seq, u32 chunk, u32 len,
			 struct request *rq, unsigned int rpos, struct cwd_pending *p)
{
	unsigned int half = (chunk & 1) * s->chunk_bytes;
	unsigned int zc_kb = READ_ONCE(zc_min_kb);
	struct cwd_run runs[CWD_ZC_MAX_RUNS];
	int n = 0, i, err;
	u32 off = 0;

	if (zc_kb)
		n = cwd_zc_map(s, rq, rpos, len, runs,
			       clamp_t(u32, len / (zc_kb * 1024), 1, CWD_ZC_MAX_RUNS));
	if (n) {
		for (i = 0; i < n; i++) {
			err = cwd_fetch_dma_run(s, seq, chunk, off, runs[i].len, runs[i].phys,
						i == n - 1, rq, p);
			if (err)
				return err;
			off += runs[i].len;
		}
		s->stats.zc_chunks++;
		s->stats.zc_runs += n;
		s->stats.dma_reads++;
		return 0;
	}

	err = cwd_fetch_dma_run(s, seq, chunk, 0, len, s->bounce_phys + half, true, rq, p);
	if (err)
		return err;
	p->buf = s->bounce + half;
	p->len = len;
	p->rpos = rpos;
	s->stats.dma_reads++;
	return 0;
}

/* Abandon operation seq so the unit is free again */
static void cwd_cancel(struct cwd_slot *s, u16 seq)
{
	DprCmdUnitCancel cmd = {
		.header.cmd = dpcmdUnitCancel,
		.unit = s->unit,
		.seq = seq,
	};
	DprRplUnitAck ack;

	s->stats.cancels++;
	cswarp_xfer(s->cd->wc, &cmd, sizeof(cmd), &ack, sizeof(ack), dprplUnitAck);
}

static void cwd_report(struct cwd_slot *s, const char *what, struct request *rq,
		       int err, unsigned int res)
{
	s->stats.errors++;
	if (err)
		dev_err_ratelimited(s->cd->dev, "%s: %s of %u sectors at %llu failed (%d)\n",
				    s->name, what, blk_rq_sectors(rq),
				    (unsigned long long)blk_rq_pos(rq), err);
	else
		dev_err_ratelimited(s->cd->dev, "%s: %s of %u sectors at %llu failed (board result %u)\n",
				    s->name, what, blk_rq_sectors(rq),
				    (unsigned long long)blk_rq_pos(rq), res);
}

static blk_status_t cwd_read(struct cwd_slot *s, struct request *rq)
{
	struct cswarp_core *wc = s->cd->wc;
	u32 total = blk_rq_bytes(rq);
	u32 got = 0, chunk = 0, k;
	bool use_dma = cwd_use_dma(s, total);
	struct cwd_pending pend = { .len = 0 };
	u16 seq = cwd_next_seq(s);
	u8 res = URES_OK;
	int err;

	err = cwd_submit(s, UOP_READ, seq, blk_rq_pos(rq), total, NULL, 0, 0, false, &res, &chunk);
	if (err)
		goto fail;
	if (res != URES_OK || chunk == 0) {
		if (res == URES_OK)
			res = URES_BADPARAM;
		/* a garbled ack may still be an accepted job: leave nothing running */
		goto cancel;
	}

	for (k = 0; got < total; k++) {
		u32 st, bytes, off;

		err = cwd_await(s, seq, k, false, &st);
		if (err)
			goto cancel;
		if (USTAT_PHASE(st) == UPHASE_DONE) {
			/* the device failed part way; the unit is idle again */
			res = USTAT_RESULT(st);
			if (res == URES_OK)
				res = URES_IOERR;
			goto fail;
		}
		bytes = cswarp_unit_count(wc, s->unit);
		if (bytes != min(chunk, total - got)) {
			dev_err_ratelimited(s->cd->dev, "%s: read chunk %u is %u bytes, expected %u\n",
					    s->name, k, bytes, min(chunk, total - got));
			err = -EPROTO;
			goto cancel;
		}
		if (use_dma && bytes > UNIT_INLINE_MAX && bytes <= s->chunk_bytes && !(bytes % 16)) {
			err = cwd_fetch_dma(s, seq, k, bytes, rq, got, &pend);
			if (err)
				goto cancel;
		} else {
			cwd_pending_copy(s, rq, &pend);
			for (off = 0; off < bytes;) {
				u32 n = min_t(u32, UNIT_INLINE_MAX, bytes - off);

				err = cwd_fetch(s, seq, k, off, n, rq, got + off, off + n == bytes);
				if (err)
					goto cancel;
				off += n;
			}
		}
		got += bytes;
	}
	cwd_pending_copy(s, rq, &pend);

	s->stats.reads++;
	s->stats.bytes_read += total;
	cswarp_dbg(CSWARP_DBG_DISKIO, s->cd->dev, "%s: read %u at %llu, chunk %u%s\n",
		   s->name, total, (unsigned long long)blk_rq_pos(rq), chunk,
		   use_dma ? " (dma)" : "");
	return BLK_STS_OK;

cancel:
	cwd_cancel(s, seq);
fail:
	cwd_report(s, "read", rq, err, res);
	return BLK_STS_IOERR;
}

static blk_status_t cwd_write(struct cwd_slot *s, struct request *rq)
{
	struct cswarp_core *wc = s->cd->wc;
	u32 total = blk_rq_bytes(rq);
	bool use_dma = cwd_use_dma(s, total);
	/* the chunk both sides compute; the ack echoes it */
	u32 w = use_dma ? UNIT_STREAM_CHUNK(total, s->chunk_bytes, DISK_BLOCKSIZE)
			: UNIT_WR_INLINE_CHUNK(DISK_BLOCKSIZE);
	u32 nchunks = DIV_ROUND_UP(total, w);
	u32 ack_chunk = 0, st = 0, bytes, k;
	u16 seq = cwd_next_seq(s);
	u8 res = URES_OK;
	int err;

	err = cwd_submit(s, UOP_WRITE, seq, blk_rq_pos(rq), total, rq, 0, min(w, total), use_dma,
			 &res, &ack_chunk);
	if (err)
		goto fail;
	if (res != URES_OK || (nchunks > 1 && ack_chunk != w)) {
		if (res == URES_OK)
			res = URES_BADPARAM;
		if (res != URES_BUSY)
			goto cancel;
		goto fail;
	}

	for (k = 1; k < nchunks; k++) {
		u32 off = k * w, n = min(w, total - off), dummy;

		if (k >= 2) {
			/* chunk k-2 must be on the device before its half is reused */
			err = cwd_await(s, seq, k, true, &st);
			if (err)
				goto cancel;
			if (USTAT_PHASE(st) == UPHASE_DONE)
				goto ended;	/* the device failed part way */
		}
		err = cwd_submit(s, UOP_WDATA, seq, k, n, rq, off, n, use_dma, &res, &dummy);
		if (err)
			goto cancel;
		if (res == URES_BADSEQ)
			break;		/* the job already ended: its DONE says why */
		if (res != URES_OK)
			goto cancel;
	}

	err = cwd_await(s, seq, -1, false, &st);
	if (err)
		goto cancel;
ended:
	res = USTAT_RESULT(st);
	bytes = cswarp_unit_count(wc, s->unit);
	if (res != URES_OK || bytes != total) {
		if (res == URES_OK)
			res = URES_IOERR;
		if (res == URES_WRPROT && s->disk && !get_disk_ro(s->disk)) {
			dev_warn(s->cd->dev, "%s: medium is write protected\n", s->name);
			set_disk_ro(s->disk, true);
		}
		goto fail;
	}

	s->stats.writes++;
	s->stats.bytes_written += total;
	cswarp_dbg(CSWARP_DBG_DISKIO, s->cd->dev, "%s: wrote %u at %llu in %u chunks%s\n",
		   s->name, total, (unsigned long long)blk_rq_pos(rq), nchunks,
		   use_dma ? " (dma)" : "");
	return BLK_STS_OK;

cancel:
	cwd_cancel(s, seq);
fail:
	cwd_report(s, "write", rq, err, res);
	return BLK_STS_IOERR;
}

/* ------------------------------------------------------------------------ */
/* Block layer                                                              */
/* ------------------------------------------------------------------------ */

static blk_status_t cwd_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	struct cwd_slot *s = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	blk_status_t sts;
	ktime_t t;

	blk_mq_start_request(rq);

	switch (req_op(rq)) {
	case REQ_OP_READ:
		mutex_lock(&s->io_lock);
		t = ktime_get();
		sts = cwd_read(s, rq);
		cwd_tick(&s->stats.us_total, &t);
		mutex_unlock(&s->io_lock);
		break;
	case REQ_OP_WRITE:
		mutex_lock(&s->io_lock);
		t = ktime_get();
		sts = cwd_write(s, rq);
		cwd_tick(&s->stats.us_total, &t);
		mutex_unlock(&s->io_lock);
		break;
	default:
		sts = BLK_STS_NOTSUPP;
		break;
	}

	blk_mq_end_request(rq, sts);
	return BLK_STS_OK;
}

static const struct blk_mq_ops cwd_mq_ops = {
	.queue_rq	= cwd_queue_rq,
};

static const struct block_device_operations cwd_fops = {
	.owner		= THIS_MODULE,
};

static void cwd_del_disk(struct cwd_slot *s, const char *why)
{
	if (!s->disk)
		return;
	dev_info(s->cd->dev, "%s: %s\n", s->name, why);
	del_gendisk(s->disk);
	put_disk(s->disk);
	s->disk = NULL;
	s->sectors = 0;
	s->stats.media_changes++;
}

static int cwd_add_disk(struct cwd_slot *s, u64 sectors, u32 change_cnt, const char *ident)
{
	struct queue_limits lim = {
		.logical_block_size	= DISK_BLOCKSIZE,
		.max_hw_sectors		= max(max_kb, 4U) * 2,
		.max_segments		= max(max_kb, 4U) / 4 + 1,
		.io_opt			= max(max_kb, 4U) * 512,	/* readahead = 2 x io_opt */
	};
	struct gendisk *disk;
	int err;

	disk = blk_mq_alloc_disk(&s->tag_set, &lim, s);
	if (IS_ERR(disk))
		return PTR_ERR(disk);

	disk->major = s->cd->major;
	disk->first_minor = s->idx * CWD_MINORS;
	disk->minors = CWD_MINORS;
	disk->fops = &cwd_fops;
	disk->private_data = s;
	disk->flags |= GENHD_FL_REMOVABLE;
	strscpy(disk->disk_name, s->name, sizeof(disk->disk_name));
	set_capacity(disk, sectors);

	s->disk = disk;
	s->sectors = sectors;
	s->change_cnt = change_cnt;

	dev_info(s->cd->dev, "%s: %s%s%llu sectors (%llu MB)\n", s->name,
		 ident ? ident : "", ident ? ", " : "",
		 (unsigned long long)sectors, (unsigned long long)(sectors >> 11));

	err = device_add_disk(s->cd->dev, disk, NULL);
	if (err) {
		dev_err(s->cd->dev, "%s: cannot add the disk (%d)\n", s->name, err);
		put_disk(disk);
		s->disk = NULL;
		s->sectors = 0;
	}
	return err;
}

/* Bring the slot in line with what the board reports */
static void cwd_update(struct cwd_slot *s, bool present, u64 sectors, u32 bsize,
		       u32 change_cnt, const char *ident)
{
	if (present && bsize != DISK_BLOCKSIZE) {
		if (!s->warned_bsize)
			dev_warn(s->cd->dev, "%s: %u-byte blocks are not supported\n", s->name, bsize);
		s->warned_bsize = true;
		present = false;
	} else if (!present) {
		s->warned_bsize = false;
	}
	if (!sectors)
		present = false;

	if (s->disk) {
		if (present && sectors == s->sectors && change_cnt == s->change_cnt)
			return;
		cwd_del_disk(s, present ? "medium changed" : "medium removed");
	}
	if (present)
		cwd_add_disk(s, sectors, change_cnt, ident);
}

/* Copy a space-padded INQUIRY field as a trimmed, printable string */
static void cwd_inquiry_str(char *dst, const char *src, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		dst[i] = (src[i] >= 0x20 && src[i] <= 0x7e) ? src[i] : ' ';
	dst[len] = 0;
	strim(dst);
}

static void cwd_scan_sd(struct cswarp_disk *cd)
{
	struct cwd_slot *s = &cd->slots[CWD_SLOT_SD];
	DprCmdHeader cmd = { .cmd = dpcmdSDGetInfo };
	DprRplSDGetInfoHead info;
	int err;

	if (!s->avail)
		return;
	err = cswarp_xfer(cd->wc, &cmd, sizeof(cmd), &info, sizeof(info), dprplSDGetInfo);
	if (err) {
		dev_warn_ratelimited(cd->dev, "SD card query failed (%d)\n", err);
		return;
	}
	cswarp_dbg(CSWARP_DBG_DISKIO, cd->dev, "sd: init %u state %u type %u blocks %u size %u\n",
		   info.cardInitialized, info.state, info.type, info.blockNbr, info.blockSize);
	cwd_update(s, info.cardInitialized != 0, info.blockNbr, info.blockSize, 0, "SD card");
}

static void cwd_scan_usb(struct cswarp_disk *cd)
{
	DprCmdHeader cmd = { .cmd = dpcmdUSBDiskGetUnits };
	DprRplUSBDiskUnits *rpl;
	unsigned int n, cnt;
	int err;

	if (!cd->slots[CWD_SLOT_USB0].avail && !cd->slots[CWD_SLOT_USB1].avail)
		return;
	rpl = kmalloc(sizeof(*rpl), GFP_KERNEL);
	if (!rpl)
		return;
	err = cswarp_xfer(cd->wc, &cmd, sizeof(cmd), rpl, sizeof(*rpl), dprplUSBDiskUnits);
	if (err) {
		dev_warn_ratelimited(cd->dev, "USB disk query failed (%d)\n", err);
		goto out;
	}
	cnt = min_t(u32, rpl->unitCount, WARP_USB_DISK_MAX_UNITS);
	for (n = 0; n < WARP_USB_DISK_MAX_UNITS; n++) {
		struct cwd_slot *s = &cd->slots[CWD_SLOT_USB0 + n];
		DprUSBDiskUnit *u = &rpl->units[n];
		char vendor[9], product[17], rev[5], ident[64];

		if (!s->avail)
			continue;
		if (n >= cnt) {
			cwd_update(s, false, 0, 0, 0, NULL);
			continue;
		}
		cswarp_dbg(CSWARP_DBG_DISKIO, cd->dev, "usb%u: present %u blocks %u size %u change %u id %08x\n",
			   n, u->present, u->blockNbr, u->blockSize, u->changeCnt, u->vidPid);
		if (u->identValid) {
			cwd_inquiry_str(vendor, u->vendor, sizeof(u->vendor));
			cwd_inquiry_str(product, u->product, sizeof(u->product));
			cwd_inquiry_str(rev, u->revision, sizeof(u->revision));
			snprintf(ident, sizeof(ident), "%s %s %s [%04x:%04x]", vendor, product, rev,
				 u->vidPid >> 16, u->vidPid & 0xffff);
		} else {
			snprintf(ident, sizeof(ident), "USB disk [%04x:%04x]",
				 u->vidPid >> 16, u->vidPid & 0xffff);
		}
		cwd_update(s, u->present != 0, u->blockNbr, u->blockSize, u->changeCnt, ident);
	}
out:
	kfree(rpl);
}

static void cwd_poll(struct work_struct *work)
{
	struct cswarp_disk *cd = container_of(to_delayed_work(work), struct cswarp_disk, poll_work);
	unsigned int ms = READ_ONCE(poll_ms);

	cwd_scan_sd(cd);
	cwd_scan_usb(cd);
	if (ms)
		schedule_delayed_work(&cd->poll_work, msecs_to_jiffies(ms));
}

/* ------------------------------------------------------------------------ */
/* sysfs                                                                    */
/* ------------------------------------------------------------------------ */

static ssize_t disk_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_disk *cd = dev_get_drvdata(dev);
	int len = 0, i;

	for (i = 0; i < CWD_SLOTS; i++) {
		struct cwd_slot *s = &cd->slots[i];
		struct cwd_stats *st = &s->stats;

		if (!s->avail)
			continue;
		len += sysfs_emit_at(buf, len,
				     "%s: unit %u chunk %u %s sectors %llu\n"
				     "  reads %lu (%llu bytes) writes %lu (%llu bytes)\n"
				     "  errors %lu timeouts %lu cancels %lu busy_retries %lu media_changes %lu\n"
				     "  dma %s, dma_reads %lu dma_writes %lu dma_errors %lu\n"
				     "  us: total %llu submit %llu wait %llu xfer %llu tc %llu cache %llu copy %llu\n"
				     "  zc_chunks %lu zc_runs %lu zc_frag %lu zc_align %lu\n",
				     s->name, s->unit, s->chunk_bytes,
				     s->disk ? "present" : "empty", (unsigned long long)s->sectors,
				     st->reads, st->bytes_read, st->writes, st->bytes_written,
				     st->errors, st->timeouts, st->cancels, st->busy_retries,
				     st->media_changes,
				     s->bounce ? "ready" : s->bounce_failed ? "unusable" : "unused",
				     st->dma_reads, st->dma_writes, st->dma_errors,
				     st->us_total, st->us_submit, st->us_wait, st->us_xfer,
				     st->us_tc, st->us_cache, st->us_copy,
				     st->zc_chunks, st->zc_runs, st->zc_frag, st->zc_align);
	}
	return len;
}
/* Any write zeroes the counters (not the medium state) */
static ssize_t disk_stats_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct cswarp_disk *cd = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < CWD_SLOTS; i++) {
		struct cwd_slot *s = &cd->slots[i];

		mutex_lock(&s->io_lock);
		memset(&s->stats, 0, sizeof(s->stats));
		mutex_unlock(&s->io_lock);
	}
	return count;
}
static DEVICE_ATTR_RW(disk_stats);

static struct attribute *cwd_attrs[] = {
	&dev_attr_disk_stats.attr,
	NULL
};
ATTRIBUTE_GROUPS(cwd);

/* ------------------------------------------------------------------------ */
/* Probe                                                                    */
/* ------------------------------------------------------------------------ */

static void cwd_doorbell(void *ctx)
{
	struct cwd_slot *s = ctx;

	wake_up(&s->wq);
}

static int cwd_read_caps(struct cswarp_disk *cd, DprRplUnitCaps *caps)
{
	DprCmdHeader cmd = { .cmd = dpcmdUnitCaps };
	int err;

	err = cswarp_xfer(cd->wc, &cmd, sizeof(cmd), caps, sizeof(*caps), dprplUnitCaps);
	if (err) {
		dev_err(cd->dev, "dpcmdUnitCaps failed (%d)\n", err);
		return err;
	}
	if (caps->version != UNITCAPS_VERSION) {
		dev_err(cd->dev, "unit protocol version %u, this driver speaks %u\n",
			caps->version, UNITCAPS_VERSION);
		return -ENODEV;
	}
	return 0;
}

static void cwd_teardown(struct cswarp_disk *cd)
{
	int i;

	cancel_delayed_work_sync(&cd->poll_work);
	for (i = 0; i < CWD_SLOTS; i++) {
		struct cwd_slot *s = &cd->slots[i];

		cwd_del_disk(s, "detached");
		if (s->unit_registered)
			cswarp_unit_unregister(cd->wc, s->unit);
		s->unit_registered = false;
		if (s->tag_set_ok)
			blk_mq_free_tag_set(&s->tag_set);
		s->tag_set_ok = false;
		kfree(s->bounce);
		s->bounce = NULL;
	}
	if (cd->major > 0)
		unregister_blkdev(cd->major, "warpdisk");
	cd->major = 0;
}

static int cwd_probe(struct platform_device *pdev)
{
	static const char *const names[CWD_SLOTS] = { "warpsd", "warpusb0", "warpusb1" };
	static const unsigned int units[CWD_SLOTS] = { AUNIT_SD, AUNIT_USB(0), AUNIT_USB(1) };
	struct cswarp_core *wc = cswarp_core_get(pdev);
	struct cswarp_disk *cd;
	DprRplUnitCaps *caps;
	int err, i, navail = 0;

	cd = devm_kzalloc(&pdev->dev, sizeof(*cd), GFP_KERNEL);
	caps = devm_kzalloc(&pdev->dev, sizeof(*caps), GFP_KERNEL);
	if (!cd || !caps)
		return -ENOMEM;
	cd->dev = &pdev->dev;
	cd->wc = wc;
	INIT_DELAYED_WORK(&cd->poll_work, cwd_poll);
	platform_set_drvdata(pdev, cd);

	err = cwd_read_caps(cd, caps);
	if (err)
		return err;

	{
		struct zorro_dev *z = zorro_find_device(ZORRO_PROD_CSLAB_WARP_DDR3, NULL);

		if (z) {
			cd->ddr3_start = zorro_resource_start(z);
			cd->ddr3_end = zorro_resource_end(z);
		}
	}
	cswarp_dbg(CSWARP_DBG_DISK, cd->dev, "units 0x%08x, chunk SD %u USB %u/%u\n",
		   caps->unitMask, caps->chunkBytes[AUNIT_SD], caps->chunkBytes[AUNIT_USB0],
		   caps->chunkBytes[AUNIT_USB1]);

	err = register_blkdev(0, "warpdisk");
	if (err < 0)
		return err;
	cd->major = err;

	for (i = 0; i < CWD_SLOTS; i++) {
		struct cwd_slot *s = &cd->slots[i];

		s->cd = cd;
		s->idx = i;
		s->unit = units[i];
		strscpy(s->name, names[i], sizeof(s->name));
		mutex_init(&s->io_lock);
		init_waitqueue_head(&s->wq);
		s->seq = get_random_u16();
		if (!(caps->unitMask & BIT(s->unit)))
			continue;
		s->chunk_bytes = caps->chunkBytes[s->unit];

		err = blk_mq_alloc_sq_tag_set(&s->tag_set, &cwd_mq_ops, 2,
					      BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_BLOCKING);
		if (err)
			goto fail;
		s->tag_set_ok = true;

		err = cswarp_unit_register(wc, s->unit, cwd_doorbell, s);
		if (err) {
			dev_err(cd->dev, "%s: unit %u doorbell taken (%d)\n", s->name, s->unit, err);
			goto fail;
		}
		s->unit_registered = true;
		s->avail = true;
		navail++;

		/*
		 * Without compaction (it is off: the hole between the Warp's
		 * RAM blocks breaks it) a 256 KB buffer gets hard to find once
		 * memory is fragmented; with dma=1 from boot take it now.
		 */
		if (READ_ONCE(dma))
			cwd_bounce(s);
	}
	if (!navail) {
		dev_info(cd->dev, "the firmware offers no SD or USB disk units\n");
		err = -ENODEV;
		goto fail;
	}

	/* first scan now, so a medium present at boot is there before rootwait looks */
	cwd_scan_sd(cd);
	cwd_scan_usb(cd);
	if (poll_ms)
		schedule_delayed_work(&cd->poll_work, msecs_to_jiffies(poll_ms));
	dev_info(cd->dev, "SD card %s, USB disk units %s/%s, media polled every %u ms, QSDMA %s\n",
		 cd->slots[CWD_SLOT_SD].avail ? "yes" : "no",
		 cd->slots[CWD_SLOT_USB0].avail ? "yes" : "no",
		 cd->slots[CWD_SLOT_USB1].avail ? "yes" : "no", poll_ms, dma ? "on" : "off");
	return 0;

fail:
	cwd_teardown(cd);
	return err;
}

static void cwd_remove(struct platform_device *pdev)
{
	cwd_teardown(platform_get_drvdata(pdev));
}

static struct platform_driver cwd_driver = {
	.probe		= cwd_probe,
	.remove_new	= cwd_remove,
	.driver		= {
		.name		= DRV_NAME,
		.dev_groups	= cwd_groups,
	},
};
module_platform_driver(cwd_driver);

MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("CS-Lab Warp SD card and USB disks");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
