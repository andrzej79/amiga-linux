// SPDX-License-Identifier: GPL-2.0
/*
 * CS-Lab Warp turbo board - native IDE (PATA) driver
 *
 * Copyright (c) 2024-2026 CS-Lab s.c.
 *		http://www.cs-lab.eu
 *
 * Based on pata_gayle.c, pata_buddha.c and warpATA.device.
 * Created 2 Jun 2024 by Andrzej Rogozynski.
 *
 * The FPGA decodes the IDE port at CTRL + 0x6000 only in native IDE mode (the
 * factory default; "WarpTool IDEMode=native").  In Gayle emulation mode the
 * window is not decoded at all, so the driver refuses to bind instead of
 * touching it.  Registers have a 4-byte stride and A12 selects CS1.  There is
 * no interrupt in native mode: libata polls.
 *
 * Data transfers are 16 bit by default.  The Warp560/1240/1260 FPGA splits a
 * 32-bit access into two 16-bit IDE cycles (warpATA.device uses that); the
 * 1260X/7060 IDE block has no such split.  pio32=1 enables 32-bit transfers
 * on the boards that support them.
 */

#include <linux/ata.h>
#include <linux/kernel.h>
#include <linux/libata.h>
#include <linux/mfd/cswarp.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <scsi/scsi_host.h>

#define DRV_NAME	"pata_cswarp"
#define DRV_VERSION	"3.0"

static bool pio32;
module_param(pio32, bool, 0444);
MODULE_PARM_DESC(pio32, "Use 32-bit PIO data transfers where the board supports them (Warp560/1240/1260)");

static const struct scsi_host_template pata_cswarp_sht = {
	ATA_PIO_SHT(DRV_NAME),
};

static bool board_has_pio32(u8 type)
{
	switch (type) {
	case WARP_BOARD_560:
	case WARP_BOARD_1240:
	case WARP_BOARD_1260:
		return true;
	default:
		return false;
	}
}

static unsigned int pata_cswarp_data_xfer(struct ata_queued_cmd *qc,
					  unsigned char *buf,
					  unsigned int buflen, int rw)
{
	struct ata_port *ap = qc->dev->link->ap;
	void __iomem *data_addr = ap->ioaddr.data_addr;
	bool use32 = ap->private_data != NULL;
	unsigned int done = 0;

	if (cswarp_dbg_on(CSWARP_DBG_ATAIO))
		ata_dev_info(qc->dev, "dbg: %s %u bytes (%s), cmd 0x%02x\n",
			     rw == READ ? "read" : "write", buflen,
			     use32 ? "32 bit" : "16 bit", qc->tf.command);

	if (use32 && buflen >= 4) {
		unsigned int longs = buflen >> 2;

		if (rw == READ)
			raw_insl((u32 __iomem *)data_addr, (u32 *)buf, longs);
		else
			raw_outsl((u32 __iomem *)data_addr, (u32 *)buf, longs);
		done = longs << 2;
	}

	if (buflen - done >= 2) {
		unsigned int words = (buflen - done) >> 1;

		if (rw == READ)
			raw_insw((u16 __iomem *)data_addr, (u16 *)(buf + done), words);
		else
			raw_outsw((u16 __iomem *)data_addr, (u16 *)(buf + done), words);
		done += words << 1;
	}

	/* trailing byte, if any */
	if (unlikely(done < buflen)) {
		unsigned char pad[2] = { };

		if (rw == READ) {
			raw_insw((u16 __iomem *)data_addr, (u16 *)pad, 1);
			buf[done] = pad[0];
		} else {
			pad[0] = buf[done];
			raw_outsw((u16 __iomem *)data_addr, (u16 *)pad, 1);
		}
		done += 2;
	}
	return done;
}

/*
 * The FPGA sets the IDE timing (the board firmware's IDE speed setting); the
 * drive is left in the mode it powered up in.
 */
static int pata_cswarp_set_mode(struct ata_link *link, struct ata_device **unused)
{
	struct ata_device *dev;

	ata_for_each_dev(dev, link, ENABLED) {
		dev->pio_mode = dev->xfer_mode = XFER_PIO_0;
		dev->xfer_shift = ATA_SHIFT_PIO;
		dev->flags |= ATA_DFLAG_PIO;
		ata_dev_info(dev, "configured for PIO (timing set by the Warp firmware)\n");
	}
	return 0;
}

static struct ata_port_operations pata_cswarp_ops = {
	.inherits	= &ata_sff_port_ops,
	.sff_data_xfer	= pata_cswarp_data_xfer,
	.cable_detect	= ata_cable_unknown,
	.set_mode	= pata_cswarp_set_mode,
};

static int pata_cswarp_probe(struct platform_device *pdev)
{
	struct cswarp_core *wc = cswarp_core_get(pdev);
	struct ata_host *host;
	struct ata_port *ap;
	void __iomem *base;
	bool use32;
	int err;

	if (!wc)
		return -ENODEV;

	if (!wc->native_ide) {
		dev_warn(&pdev->dev, "Warp IDE is in Gayle emulation mode, native IDE port not decoded\n");
		dev_warn(&pdev->dev, "switch with \"WarpTool IDEMode=native\" under AmigaOS to use it from Linux\n");
		return -ENODEV;
	}

	use32 = pio32 && board_has_pio32(wc->board_type);
	if (pio32 && !use32)
		dev_info(&pdev->dev, "pio32 requested, but %s has no 32-bit IDE access: using 16 bit\n",
			 cswarp_board_type_name(wc->board_type));

	base = wc->base + WARP_OFFSET_ATA;
	cswarp_dbg(CSWARP_DBG_ATA, &pdev->dev,
		   "native IDE at %p (phys 0x%08lx), ctl %p, IDE timing IOR %u/%u IOW %u/%u ACK %u, %s PIO\n",
		   base, wc->phys + WARP_OFFSET_ATA, base + WARP_ATA_CTL_OFFSET,
		   wc->ide_timing[0], wc->ide_timing[1], wc->ide_timing[2],
		   wc->ide_timing[3], wc->ide_timing[4], use32 ? "32-bit" : "16-bit");

	host = ata_host_alloc(&pdev->dev, 1);
	if (!host)
		return -ENOMEM;

	ap = host->ports[0];
	ap->ops = &pata_cswarp_ops;
	ap->pio_mask = ATA_PIO4;
	ap->flags |= ATA_FLAG_SLAVE_POSS | ATA_FLAG_NO_IORDY | ATA_FLAG_PIO_POLLING;
	ap->private_data = use32 ? (void *)1 : NULL;

	ap->ioaddr.data_addr		= base;
	ap->ioaddr.error_addr		= base + 1 * 4;
	ap->ioaddr.feature_addr		= base + 1 * 4;
	ap->ioaddr.nsect_addr		= base + 2 * 4;
	ap->ioaddr.lbal_addr		= base + 3 * 4;
	ap->ioaddr.lbam_addr		= base + 4 * 4;
	ap->ioaddr.lbah_addr		= base + 5 * 4;
	ap->ioaddr.device_addr		= base + 6 * 4;
	ap->ioaddr.status_addr		= base + 7 * 4;
	ap->ioaddr.command_addr		= base + 7 * 4;
	ap->ioaddr.altstatus_addr	= base + WARP_ATA_CTL_OFFSET;
	ap->ioaddr.ctl_addr		= base + WARP_ATA_CTL_OFFSET;

	ata_port_desc(ap, "Warp native IDE cmd 0x%lx ctl 0x%lx",
		      wc->phys + WARP_OFFSET_ATA,
		      wc->phys + WARP_OFFSET_ATA + WARP_ATA_CTL_OFFSET);

	cswarp_dbg(CSWARP_DBG_ATA, &pdev->dev, "status 0x%02x altstatus 0x%02x before activate\n",
		   z_readb(ap->ioaddr.status_addr), z_readb(ap->ioaddr.altstatus_addr));

	/* irq 0: no interrupt, libata polls */
	err = ata_host_activate(host, 0, NULL, 0, &pata_cswarp_sht);
	if (err) {
		dev_err(&pdev->dev, "ata_host_activate failed (%d)\n", err);
		return err;
	}
	cswarp_dbg(CSWARP_DBG_ATA, &pdev->dev, "port registered\n");
	return 0;
}

static void pata_cswarp_remove(struct platform_device *pdev)
{
	struct ata_host *host = platform_get_drvdata(pdev);

	ata_host_detach(host);
}

static struct platform_driver pata_cswarp_driver = {
	.probe		= pata_cswarp_probe,
	.remove_new	= pata_cswarp_remove,
	.driver		= {
		.name	= "cswarp-ata",
	},
};
module_platform_driver(pata_cswarp_driver);

MODULE_ALIAS("platform:cswarp-ata");
MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("low-level driver for the CS-Lab Warp native IDE port");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
