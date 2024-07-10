// SPDX-License-Identifier: GPL-2.0

/*
 * Amiga CS Warp PATA controller driver
 *
 * Copyright (c) 2024 CS-Lab s.c.
 *		http://www.cs-lab.eu
 *
 * Based on pata_gayle.c, pata_buddha.c and warpATA.device:
 *
 *     Created 2 Jun 2024 by Andrzej Rogozynski
 */

#include <linux/ata.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/libata.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/zorro.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_host.h>

#include <asm/amigahw.h>
#include <asm/amigaints.h>
#include <asm/amigayle.h>
#include <asm/setup.h>

#define DRV_NAME "pata_cswarp"
#define DRV_VERSION "0.1.0"

#define WARP_OFFSET_ATA         0x6000
#define REV16(x) ((uint16_t)((x << 8) | (x >> 8)))

static const struct scsi_host_template pata_cswarp_sht = {
	ATA_PIO_SHT(DRV_NAME),
};

/* FIXME: is this needed? */
static unsigned int pata_cswarp_data_xfer(struct ata_queued_cmd *qc,
					 unsigned char *buf,
					 unsigned int buflen, int rw)
{
	struct ata_device *dev = qc->dev;
	struct ata_port *ap = dev->link->ap;
	void __iomem *data_addr = ap->ioaddr.data_addr;
	unsigned int words = buflen >> 1;

	/* Transfer multiple of 2 bytes */
	if (rw == READ)
		raw_insw((u16 *)data_addr, (u16 *)buf, words);
	else
		raw_outsw((u16 *)data_addr, (u16 *)buf, words);

	/* Transfer trailing byte, if any. */
	if (unlikely(buflen & 0x01)) {
		unsigned char pad[2] = { };

		/* Point buf to the tail of buffer */
		buf += buflen - 1;

		if (rw == READ) {
			raw_insw((u16 *)data_addr, (u16 *)pad, 1);
			*buf = pad[0];
		} else {
			pad[0] = *buf;
			raw_outsw((u16 *)data_addr, (u16 *)pad, 1);
		}
		words++;
	}

	return words << 1;
}

/*
 * Provide our own set_mode() as we don't want to change anything that has
 * already been configured..
 */
static int pata_cswarp_set_mode(struct ata_link *link,
			       struct ata_device **unused)
{
	struct ata_device *dev;

	ata_for_each_dev(dev, link, ENABLED) {
		/* We don't really care */
		dev->pio_mode = dev->xfer_mode = XFER_PIO_0;
		dev->xfer_shift = ATA_SHIFT_PIO;
		dev->flags |= ATA_DFLAG_PIO;
		ata_dev_info(dev, "configured for PIO\n");
	}
	return 0;
}

static struct ata_port_operations pata_cswarp_ops = {
	.inherits	= &ata_sff_port_ops,
	.sff_data_xfer	= pata_cswarp_data_xfer,
	.cable_detect	= ata_cable_unknown,
	.set_mode	= pata_cswarp_set_mode,
};

static int pata_cswarp_probe(struct zorro_dev *z,
			     const struct zorro_device_id *ent)
{
	static const char board_name[] = "csWarp";
	struct ata_host *host;
	void __iomem *cswarp_ctrl_board;
	unsigned long board;

	board = z->resource.start;

	dev_info(&z->dev, "%s IDE controller (board: 0x%lx)\n", board_name, board);

	if (!devm_request_mem_region(&z->dev,
						board + WARP_OFFSET_ATA,
						0x1800, DRV_NAME))
	{
		return -ENXIO;
	}

	/* allocate host */
	host = ata_host_alloc(&z->dev, 1);
	if (!host)
		return -ENXIO;

	cswarp_ctrl_board = (void*)board;

	struct ata_port *ap = host->ports[0];
	void __iomem *base = cswarp_ctrl_board + WARP_OFFSET_ATA;

	ap->ops = &pata_cswarp_ops;

	ap->pio_mask = ATA_PIO4;
	ap->flags |= ATA_FLAG_SLAVE_POSS | ATA_FLAG_NO_IORDY | ATA_FLAG_PIO_POLLING;

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

	ap->ioaddr.altstatus_addr	= base + (0x1000 | (6UL << 2));
	ap->ioaddr.ctl_addr			= base + (0x1000 | (6UL << 2));

	ap->private_data = (void *)0;

	ata_port_desc(ap, "  cmd 0x%lx ctl 0x%lx", 
			(unsigned long)base, (unsigned long)ap->ioaddr.ctl_addr);

	ata_host_activate(host, 0, NULL,
			  IRQF_SHARED, &pata_cswarp_sht);

	return 0;
}

static void pata_cswarp_remove(struct zorro_dev *z)
{
	struct ata_host *host = dev_get_drvdata(&z->dev);

	ata_host_detach(host);
}

static const struct zorro_device_id pata_cswarp_zorro_tbl[] = {
	{ ZORRO_PROD_CSLAB_WARP_CTRL, 0},
	{ 0 }
};
MODULE_DEVICE_TABLE(zorro, pata_cswarp_zorro_tbl);

static struct zorro_driver pata_cswarp_driver = {
	.name           = "pata_cswarp",
	.id_table       = pata_cswarp_zorro_tbl,
	.probe          = pata_cswarp_probe,
	.remove         = pata_cswarp_remove,
};

/*
 * We cannot have a modalias for X-Surf boards, as it competes with the
 * zorro8390 network driver. As a stopgap measure until we have proper
 * MFD support for this board, we manually attach to it late after Zorro
 * has enumerated its boards.
 */
static int __init pata_cswarp_late_init(void)
{
	struct zorro_dev *z = NULL;

	/* Auto-bind to regular boards */
	zorro_register_driver(&pata_cswarp_driver);

	/* Manually bind to all boards */
	while ((z = zorro_find_device(ZORRO_PROD_CSLAB_WARP_CTRL, z))) {
		static struct zorro_device_id cswarp_ent = {
			ZORRO_PROD_CSLAB_WARP_CTRL, 0
		};

		pata_cswarp_probe(z, &cswarp_ent);
	}
	return 0;
}
late_initcall(pata_cswarp_late_init);

MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("low-level driver for CSWarp PATA");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);
