/*
 *  linux/drivers/net/ethernet/amiwarpnet.c -- Amiga / csWarp network driver
 *
 *      Copyright (C) 2024 Andrzej Rogozynski
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/zorro.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/jiffies.h>

#include <asm/amigaints.h>
#include <asm/cswarpdefs.h>
#include <asm/cswarpamicommdata.h>

#define DRV_NAME	"amiwarpnet"
#define DRV_VERSION	"2024-06-12"

/* The maximum time waited (in jiffies) before assuming a Tx failed. */
#define TX_TIMEOUT (2 * HZ)
#define TMR_POLL_INTERVAL (100 * HZ / 1000)

typedef struct {
    void __iomem *ctrlBase;
	struct timer_list pollTimer;
	spinlock_t dpram_lock;

	struct napi_struct napi;
	struct net_device *ndev;
	bool promisc;
	u32 msg_enable;
} WarpNetPriv;

static const char bcast_addr[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// ############################################################################
// Warp HW functions
// ############################################################################
static void cleanupIrqAndFlags(WarpNetPriv *priv)
{
	volatile u32 __iomem *dp_reg_cr = 
		(volatile u32*)((u32)priv->ctrlBase | WARP_OFFSET_DPREG_CR);

	// clear all IRQs and flags (ARM <-> 68k comm)
	*dp_reg_cr = DPREG_CR_CLR | 
				 DPREG_CR_MP_68K | DPREG_CR_MP_ARM |
				 DPREG_CR_MR_68K | DPREG_CR_MR_ARM |
				 DPREG_CR_IE_68K | 
				 DPREG_CR_IE_ETHRX | 
				 DPREG_CR_IE_ETHST | 
				 DPREG_CR_IE_ETHTX |
				 DPREG_CR_IF_ETHRX | 
				 DPREG_CR_IF_ETHST | 
				 DPREG_CR_IF_ETHTX;
}

// irq handler
static irqreturn_t warpnet_irq(int irq, void *ndev_instance)
{
	struct net_device *ndev = ndev_instance;
	WarpNetPriv *priv = netdev_priv(ndev);
	volatile u32 __iomem *dp_reg_cr = 
  		(volatile u32*)((u32)priv->ctrlBase | WARP_OFFSET_DPREG_CR);
	irqreturn_t res = IRQ_NONE;

	if(*dp_reg_cr & DPREG_CR_IF_ETHRX) {
		*dp_reg_cr = DPREG_CR_CLR | DPREG_CR_IF_ETHRX;
		// eth frame received
		if (napi_schedule_prep(&priv->napi)) {
			__napi_schedule(&priv->napi);
		}		
		res = IRQ_HANDLED;
	}

	return res;
}

/**
 * @brief send IRQ to ARM, wait for
 *        message processing and optional for reply.
 * 	      (It is assumed that frame is already in dpRAM)
 * @param priv WarpNetPriv data
 * @param waitForReply
 * @return WAC_OK if successfull
 */
static WarpAmiCommStatus sendMsgToArm(WarpNetPriv *priv, bool waitForReply)
{
  volatile u32 __iomem *dp_reg_cr = 
  		(volatile u32*)((u32)priv->ctrlBase | WARP_OFFSET_DPREG_CR);
  
  // send irq to ARM
  *dp_reg_cr = DPREG_CR_SET | DPREG_CR_MP_ARM | DPREG_CR_IE_ARM;

  // wait for ARM has processed the message
  while((*dp_reg_cr & DPREG_CR_MR_ARM) == 0);
  // clear flag
  *dp_reg_cr = DPREG_CR_CLR | DPREG_CR_MR_ARM | DPREG_CR_IE_ARM;

  if(waitForReply) {
    // wait for ARM reply
	while((*dp_reg_cr & DPREG_CR_MP_68K) == 0);
	// clear flag
	*dp_reg_cr = DPREG_CR_CLR | DPREG_CR_MP_68K;
  }

  return wacOK;
}

static WarpAmiCommStatus ethGetMacAddress(WarpNetPriv *priv, char *mac)
{
	volatile DprCmdFrame __iomem *cmd = 
		(volatile DprCmdFrame*)((u32)priv->ctrlBase | WARP_OFFSET_DPRAM);
	volatile DprRplFrame __iomem *rpl = 
		(volatile DprRplFrame*)cmd;
	ulong irqFlags;

	spin_lock_irqsave(&priv->dpram_lock, irqFlags);

	cmd->header.cmd = dpcmdEthGetMACAddr;
	sendMsgToArm(priv, true);

	if(rpl->header.rpl != dprplEthMACAddr) {
		spin_unlock_irqrestore(&priv->dpram_lock, irqFlags);
		return wacCOMERR;
	}
	memcpy(mac, (void*)rpl->ethMAC.mac, ETH_ALEN);
	spin_unlock_irqrestore(&priv->dpram_lock, irqFlags);

	return wacOK;
}

// ############################################################################
// ethtool functions
// ############################################################################

static void warpnet_get_drvinfo(struct net_device *ndev,
			      struct ethtool_drvinfo *info)
{
	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	strscpy(info->version, DRV_VERSION, sizeof(info->version));
	strscpy(info->bus_info, dev_name(ndev->dev.parent),
		sizeof(info->bus_info));
}

static u32 warpnet_get_msglevel(struct net_device *ndev)
{
	WarpNetPriv *priv = netdev_priv(ndev);

	return priv->msg_enable;
}

static void warpnet_set_msglevel(struct net_device *ndev, u32 value)
{
	WarpNetPriv *priv = netdev_priv(ndev);

	priv->msg_enable = value;
}

static u32 warpnet_get_link(struct net_device *ndev)
{
	return 1;
}


// ############################################################################
// netdev functions
// ############################################################################

static int warpnet_open(struct net_device *ndev)
{
    WarpNetPriv *priv = netdev_priv(ndev);
  	volatile u32 __iomem *dp_reg_cr = 
  		(volatile u32*)((u32)priv->ctrlBase | WARP_OFFSET_DPREG_CR);

	netif_info(priv, ifup, ndev, "enabling\n");
	cleanupIrqAndFlags(priv);

	if (ndev->watchdog_timeo <= 0)
		ndev->watchdog_timeo = TX_TIMEOUT;

	napi_enable(&priv->napi);
	netif_start_queue(ndev);
    netif_carrier_on(ndev);

	// enable eth rx irq
	*dp_reg_cr = DPREG_CR_SET | DPREG_CR_IE_ETHRX;

	mod_timer(&priv->pollTimer, jiffies + TMR_POLL_INTERVAL);

	return 0;
}

static int warpnet_close(struct net_device *ndev)
{
	WarpNetPriv *priv = netdev_priv(ndev);

	del_timer_sync(&priv->pollTimer);

	cleanupIrqAndFlags(priv);

	netif_info(priv, ifdown, ndev, "shutting down\n");
	netif_carrier_off(ndev);
	netif_stop_queue(ndev);
	napi_disable(&priv->napi);
	return 0;
}

static netdev_tx_t warpnet_start_xmit(struct sk_buff *skb,
				   struct net_device *ndev)
{
    WarpNetPriv *priv = netdev_priv(ndev);
	volatile DprCmdFrame __iomem *cmd = 
		(volatile DprCmdFrame*)((u32)priv->ctrlBase | WARP_OFFSET_DPRAM);
	uint tx_len = skb->len;
	ulong irqFlags;

	spin_lock_irqsave(&priv->dpram_lock, irqFlags);

	cmd->header.cmd = dpcmdEthTransmit;
	cmd->ethSend.pktSize = tx_len;
	memcpy((void*)cmd->ethSend.packet, skb->data, tx_len);
	sendMsgToArm(priv, false);	
	spin_unlock_irqrestore(&priv->dpram_lock, irqFlags);

	dev_kfree_skb(skb);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += tx_len;

    return NETDEV_TX_OK;
}

static void warpnet_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	netdev_err(ndev, "TX timeout\n");
	netif_stop_queue(ndev);
	ndev->stats.tx_errors++;
	netif_trans_update(ndev);
	netif_wake_queue(ndev);
}

static struct net_device_stats *warpnet_get_stats(struct net_device *ndev)
{
	return &ndev->stats;
}

static void warpnet_set_rx_mode(struct net_device *ndev)
{
	WarpNetPriv *priv = netdev_priv(ndev);
	bool set_promisc = (ndev->flags & IFF_PROMISC) != 0;

	if (priv->promisc != set_promisc) {
		priv->promisc = set_promisc;
	}
}

static int warpnet_set_macaddr(struct net_device *ndev, void *addr)
{
	netdev_warn(ndev, "MAC setting is not supported!\n");
	return -EADDRNOTAVAIL;
}

static int warpnet_napi_poll(struct napi_struct *napi, int budget)
{
    WarpNetPriv *priv = container_of(napi, WarpNetPriv, napi);
	struct net_device *ndev = priv->ndev;
	volatile DprCmdFrame __iomem *cmd = 
		(volatile DprCmdFrame*)((u32)priv->ctrlBase | WARP_OFFSET_DPRAM);
	volatile DprRplFrame __iomem *rpl = 
		(volatile DprRplFrame*)cmd;
	int rx_count;
	ulong irqFlags;
	struct sk_buff *skb;

	for(rx_count = 0; rx_count < budget; rx_count++)
	{
		spin_lock_irqsave(&priv->dpram_lock, irqFlags);
		
		cmd->header.cmd = dpcmdEthReceive;
		sendMsgToArm(priv, true);

		if(unlikely(rpl->header.rpl != dprplEthReceive)) 
		{
			spin_unlock_irqrestore(&priv->dpram_lock, irqFlags);
			netdev_err(ndev, "warpnet_napi_poll: error, wrong reply header!\n");
			break;
		}
		uint16_t rx_len = rpl->ethRecv.pktSize;
		if(rx_len == 0) {
			spin_unlock_irqrestore(&priv->dpram_lock, irqFlags);
			break;
		}
		if(!priv->promisc && 
			(memcmp((void*)rpl->ethRecv.packet, ndev->dev_addr, ETH_ALEN) != 0) &&
			(memcmp((void*)rpl->ethRecv.packet, bcast_addr, ETH_ALEN) != 0))
		{
			// not promiciuous mode and dst MAC is not ours
			spin_unlock_irqrestore(&priv->dpram_lock, irqFlags);
			break;			
		}
		skb = netdev_alloc_skb(ndev, rx_len);
		skb_put_data(skb, (void*)rpl->ethRecv.packet, rx_len);
		spin_unlock_irqrestore(&priv->dpram_lock, irqFlags);
		skb->protocol = eth_type_trans(skb, ndev);
		netif_receive_skb(skb);

		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += rx_len;
	}

	if(rx_count < budget) {
		napi_complete_done(napi, rx_count);
	}
    return rx_count;
}

static const struct ethtool_ops warpnet_ethtool_ops = {
	.get_drvinfo		= warpnet_get_drvinfo,
	.get_msglevel		= warpnet_get_msglevel,
	.set_msglevel		= warpnet_set_msglevel,
	.get_link		    = warpnet_get_link,
};

const struct net_device_ops warpnet_netdev_ops = {
	.ndo_open		    = warpnet_open,
	.ndo_stop		    = warpnet_close,
	.ndo_start_xmit		= warpnet_start_xmit,
	.ndo_tx_timeout		= warpnet_tx_timeout,
	.ndo_get_stats		= warpnet_get_stats,
	.ndo_set_rx_mode	= warpnet_set_rx_mode,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address = warpnet_set_macaddr,
};

static void pollTimerCallback(struct timer_list *t)
{
	WarpNetPriv *priv = container_of(t, WarpNetPriv, pollTimer);

	napi_schedule(&priv->napi);
	mod_timer(&priv->pollTimer, jiffies + TMR_POLL_INTERVAL);
}


/**
 * @brief warpnet_probe
 */
static int warpnet_probe(struct zorro_dev *z, const struct zorro_device_id *id)
{
    int retval;
    struct net_device *ndev;

    ndev = alloc_etherdev(sizeof(WarpNetPriv));
   	if (!ndev)
        return -ENOMEM;
    // 
	ether_setup(ndev);
    SET_NETDEV_DEV(ndev, &z->dev);

    zorro_set_drvdata(z, ndev);
    WarpNetPriv *priv = netdev_priv(ndev);

	ndev->irq = IRQ_AMIGA_PORTS;
    ndev->netdev_ops = &warpnet_netdev_ops;
    ndev->ethtool_ops = &warpnet_ethtool_ops;
    ndev->watchdog_timeo = TX_TIMEOUT;

    // disable VLANs
    ndev->features |= NETIF_F_VLAN_CHALLENGED;
	// no checksum offloading
	ndev->features &= ~(NETIF_F_HW_CSUM | NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM | NETIF_F_RXCSUM);

	// Find Warp-CTRL Zorro device (card control registers)
	struct zorro_dev *zWarpCtrl = NULL;
	zWarpCtrl = zorro_find_device(ZORRO_PROD_CSLAB_WARP_CTRL, zWarpCtrl);
	if(zWarpCtrl == NULL) {
		dev_err(&z->dev, "amiwarpnet: Can't find Warp-CTRL zorro card! \n");
		retval = -ENODEV;
		goto err1;
	}

    priv->ctrlBase = (void*)zorro_resource_start(zWarpCtrl);
    priv->ndev = ndev;
	priv->promisc = false;

    netif_napi_add_weight(ndev, &priv->napi, warpnet_napi_poll, 8);
    register_netdev(ndev);

	// clear all IRQs and flags (ARM <-> 68k comm)
	cleanupIrqAndFlags(priv);

	// init dpram access critial section
	spin_lock_init(&priv->dpram_lock);

	int ri = request_irq(ndev->irq, warpnet_irq, IRQF_SHARED, DRV_NAME, ndev);
	if(ri) {
		netdev_err(ndev, "Can't allocate IRQ! (return val: %d)\n", ri);
		retval = -EIO;
		goto err1;
	}
	netdev_info(ndev, "irq %d allocated\n", ndev->irq);

	WarpAmiCommStatus warpStat;
	u8 warp_mac[ETH_ALEN];
	warpStat = ethGetMacAddress(priv, (char*)warp_mac);
	if(warpStat == wacOK) {
		netdev_info(ndev, "MAC address readed from Warp: %02x:%02x:%02x:%02x:%02x:%02x\n",
			warp_mac[0], warp_mac[1], warp_mac[2], 
			warp_mac[3], warp_mac[4], warp_mac[5]);
		eth_hw_addr_set(ndev, warp_mac);
	} else {
		netdev_err(ndev, "Read MAC from Warp failed!\n");
		eth_hw_addr_random(ndev);
	}

	// setup polling timer
	timer_setup(&priv->pollTimer, pollTimerCallback, 0);

	netdev_info(ndev, "device probe ok");
    return 0;

err1:  
    free_netdev(ndev);
    return retval;
}

static const struct zorro_device_id warpnet_devices[] = {
	// FIXME: Currently, this driver is bound to Warp's XROM card.
	//        A proper implementation would be an MFD driver (USB, SDCard, Network, ATA, etc.)
	//        bound to the 'ZORRO_PROD_CSLAB_WARP_CTRL' Zorro card.
	{ ZORRO_PROD_CSLAB_WARP_XROM }, 
	{ 0 }
};
MODULE_DEVICE_TABLE(zorro, warpnet_devices);

static struct zorro_driver warpnet_driver = {
	.name		= "amiwarpnet",
	.id_table	= warpnet_devices,
	.probe		= warpnet_probe,
};

static int __init warpnet_init(void)
{
	return zorro_register_driver(&warpnet_driver);
}

module_init(warpnet_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("CSWarp Turbo Board Ethernet driver");
