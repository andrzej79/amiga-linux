// SPDX-License-Identifier: GPL-2.0
/*
 * CS-Lab Warp turbo board - network driver
 *
 * Copyright (C) 2024-2026 Andrzej Rogozynski, CS-Lab s.c.
 *
 * The board's ARM side does the networking; frames travel one at a time
 * through the dual-port RAM frame owned by cswarp-core.  Two interfaces, both
 * registered as ethN (fixed map, shared with warpNET.device):
 *   ETH_IF_WIFI (0) - the ESP32 WiFi station
 *   ETH_IF_USB  (1) - a USB ethernet adapter, if one is plugged in
 *
 * A frame exchange sleeps, while ndo_start_xmit and NAPI cannot, so every
 * board access runs from one ordered workqueue: TX frames are queued per
 * interface and sent round-robin, the RX doorbells (AUNIT_ETH0/1) make the
 * worker drain dpcmdEthIfReceive(ETH_IF_ANY), and a slow timer covers a missed
 * doorbell.  Link state and MAC come from dpcmdEthIfGetInfo, polled once a
 * second while an interface is up.
 */

#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/kernel.h>
#include <linux/mfd/cswarp.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>

#define DRV_NAME	"amiwarpnet"
#define DRV_VERSION	"3.0"

#define TX_TIMEOUT		(5 * HZ)
#define TXQ_STOP_LEN		64	/* stop the netdev queue at this backlog */
#define TXQ_WAKE_LEN		16	/* and wake it again below this */
#define RX_BURST		32	/* frames per drain round (as warpNET.device) */
#define IO_ROUNDS		64	/* work rounds before yielding the workqueue */
#define RX_POLL_MS		200	/* fallback for a missed doorbell */
#define LINK_POLL_MS		1000

#define NET_RX_PENDING		0	/* drain wanted (doorbell or timer) */
#define NET_RX_DB		1	/* ... and a doorbell asked for it, at db_time */

/* latency accumulator, microseconds */
struct warpnet_lat {
	u64 total;
	u32 max;
	unsigned long n;
};

static void lat_add(struct warpnet_lat *l, s64 us)
{
	if (us < 0)
		us = 0;
	l->total += us;
	if (us > l->max)
		l->max = us;
	l->n++;
}

struct warpnet;

struct warpnet_if {
	struct warpnet *wn;
	struct net_device *ndev;
	u8 ifnr;
	struct sk_buff_head txq;
	/* last dpcmdEthIfGetInfo */
	bool present;
	bool link;
	bool full_duplex;
	u32 bps;
	u32 msg_enable;
};

struct warpnet {
	struct cswarp_core *wc;
	struct device *dev;
	struct warpnet_if *nif[ETH_IF_COUNT];
	struct workqueue_struct *wq;
	struct work_struct io_work;
	struct delayed_work link_work;
	struct timer_list rx_timer;
	unsigned long flags;
	unsigned int open_count;	/* under rtnl */
	/* counters for the dbg dump on close */
	unsigned long rx_drops_down;
	unsigned long rx_bad;
	/* latency diagnostics (sysfs "latency") */
	ktime_t db_time;
	struct warpnet_lat lat_db;	/* RX doorbell irq -> drain starts */
	struct warpnet_lat lat_tx;	/* ndo_start_xmit -> frame taken by the board */
	unsigned long rx_frames_db;	/* frames found by doorbell-triggered drains */
	unsigned long rx_frames_timer;	/* frames found by the fallback timer */
	unsigned long io_runs;
};

static const char *const if_names[ETH_IF_COUNT] = { "WiFi", "USB-ethernet" };

/* ------------------------------------------------------------------------ */
/* Board access (workqueue context only)                                    */
/* ------------------------------------------------------------------------ */

static int warpnet_get_info(struct warpnet *wn, u8 ifnr, DprRplEthIfInfo *info)
{
	DprCmdEthIf cmd = {
		.header.cmd = dpcmdEthIfGetInfo,
		.ifNr = ifnr,
	};

	return cswarp_xfer(wn->wc, &cmd, sizeof(cmd), info, sizeof(*info), dprplEthIfInfo);
}

static void warpnet_tx_one(struct warpnet_if *nif, struct sk_buff *skb)
{
	struct warpnet *wn = nif->wn;
	struct net_device *ndev = nif->ndev;
	void __iomem *frame;
	unsigned int len = skb->len;
	int err;

	frame = cswarp_frame_lock(wn->wc);
	if (IS_ERR(frame)) {
		ndev->stats.tx_errors++;
		ndev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return;
	}
	z_writel(dpcmdEthIfTransmit, frame + offsetof(DprCmdEthIfSend, header));
	z_writeb(nif->ifnr, frame + offsetof(DprCmdEthIfSend, ifNr));
	z_writeb(0, frame + offsetof(DprCmdEthIfSend, reserved));
	z_writew(len, frame + offsetof(DprCmdEthIfSend, pktSize));
	z_memcpy_toio(frame + offsetof(DprCmdEthIfSend, packet), skb->data, len);
	err = cswarp_frame_exchange(wn->wc, false, CSWARP_RPL_ANY);
	cswarp_frame_unlock(wn->wc);
	lat_add(&wn->lat_tx, ktime_us_delta(ktime_get(), *(ktime_t *)skb->cb));

	if (err) {
		ndev->stats.tx_errors++;
		netdev_err_once(ndev, "TX exchange failed (%d)\n", err);
	} else {
		ndev->stats.tx_packets++;
		ndev->stats.tx_bytes += len;
		cswarp_dbg(CSWARP_DBG_NETPKT, &ndev->dev, "TX if%u len %u proto 0x%04x\n",
			   nif->ifnr, len, ntohs(skb->protocol));
	}
	dev_kfree_skb(skb);
}

/* Drain up to RX_BURST frames.  Returns the number of exchanges that carried a frame. */
static int warpnet_rx_drain(struct warpnet *wn)
{
	int n;

	for (n = 0; n < RX_BURST; n++) {
		struct warpnet_if *nif;
		struct net_device *ndev;
		struct sk_buff *skb;
		void __iomem *frame;
		u16 len;
		u8 ifnr;
		int err;

		frame = cswarp_frame_lock(wn->wc);
		if (IS_ERR(frame))
			return n;
		z_writel(dpcmdEthIfReceive, frame + offsetof(DprCmdEthIf, header));
		z_writeb(ETH_IF_ANY, frame + offsetof(DprCmdEthIf, ifNr));
		z_writeb(0, frame + offsetof(DprCmdEthIf, reserved));
		err = cswarp_frame_exchange(wn->wc, true, dprplEthIfReceive);
		if (err) {
			cswarp_frame_unlock(wn->wc);
			dev_err_ratelimited(wn->dev, "RX exchange failed (%d)\n", err);
			return n;
		}

		len = z_readw(frame + offsetof(DprRplEthIfRecv, pktSize));
		if (len == 0) {
			/* every interface drained */
			cswarp_frame_unlock(wn->wc);
			return n;
		}
		ifnr = z_readb(frame + offsetof(DprRplEthIfRecv, ifNr));

		nif = ifnr < ETH_IF_COUNT ? wn->nif[ifnr] : NULL;
		if (!nif) {
			/* a board with more interfaces than we know: drop, keep draining */
			cswarp_frame_unlock(wn->wc);
			wn->rx_bad++;
			dev_warn_ratelimited(wn->dev, "RX frame for unknown interface %u\n", ifnr);
			continue;
		}
		ndev = nif->ndev;

		if (len < ETH_HLEN || len > ETH_MTU_AND_HDR_SIZE) {
			cswarp_frame_unlock(wn->wc);
			ndev->stats.rx_length_errors++;
			ndev->stats.rx_errors++;
			netdev_warn_once(ndev, "RX bad frame size %u\n", len);
			continue;
		}
		if (!netif_running(ndev)) {
			cswarp_frame_unlock(wn->wc);
			wn->rx_drops_down++;
			continue;
		}

		skb = netdev_alloc_skb_ip_align(ndev, len);
		if (!skb) {
			cswarp_frame_unlock(wn->wc);
			ndev->stats.rx_dropped++;
			continue;
		}
		z_memcpy_fromio(skb_put(skb, len), frame + offsetof(DprRplEthIfRecv, packet), len);
		cswarp_frame_unlock(wn->wc);

		skb->protocol = eth_type_trans(skb, ndev);
		cswarp_dbg(CSWARP_DBG_NETPKT, &ndev->dev, "RX if%u len %u proto 0x%04x type %u\n",
			   ifnr, len, ntohs(skb->protocol), skb->pkt_type);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		netif_rx(skb);
	}
	return n;
}

static void warpnet_io_work(struct work_struct *work)
{
	struct warpnet *wn = container_of(work, struct warpnet, io_work);
	int round;

	wn->io_runs++;
	for (round = 0; round < IO_ROUNDS; round++) {
		int did = 0, i;

		/* one frame per interface per round: neither starves the other */
		for (i = 0; i < ETH_IF_COUNT; i++) {
			struct warpnet_if *nif = wn->nif[i];
			struct sk_buff *skb;

			if (!nif)
				continue;
			skb = skb_dequeue(&nif->txq);
			if (!skb)
				continue;
			warpnet_tx_one(nif, skb);
			did++;
			if (netif_queue_stopped(nif->ndev) &&
			    skb_queue_len(&nif->txq) < TXQ_WAKE_LEN &&
			    netif_running(nif->ndev)) {
				cswarp_dbg(CSWARP_DBG_NET, &nif->ndev->dev, "TX queue woken\n");
				netif_wake_queue(nif->ndev);
			}
		}

		if (test_and_clear_bit(NET_RX_PENDING, &wn->flags)) {
			bool db = test_and_clear_bit(NET_RX_DB, &wn->flags);
			int n;

			if (db)
				lat_add(&wn->lat_db, ktime_us_delta(ktime_get(), wn->db_time));
			n = warpnet_rx_drain(wn);
			if (db)
				wn->rx_frames_db += n;
			else
				wn->rx_frames_timer += n;

			if (n == RX_BURST)
				set_bit(NET_RX_PENDING, &wn->flags);
			did += n;
		}

		if (!did)
			return;
	}
	/* still busy: let other work run, then continue */
	queue_work(wn->wq, &wn->io_work);
}

static void warpnet_update_info(struct warpnet_if *nif, const DprRplEthIfInfo *info)
{
	struct net_device *ndev = nif->ndev;
	bool present = info->present, link = info->present && info->linkUp;

	if (present != nif->present)
		netdev_info(ndev, "%s interface %s\n", if_names[nif->ifnr],
			    present ? "present" : "gone");

	/*
	 * ndo_stop runs under RTNL and flushes this workqueue, so only try the
	 * lock here; a missed update is retried on the next poll.
	 */
	if (present && is_valid_ether_addr(info->mac) &&
	    !ether_addr_equal(ndev->dev_addr, info->mac) && rtnl_trylock()) {
		netdev_info(ndev, "MAC address %pM (was %pM)\n", info->mac, ndev->dev_addr);
		eth_hw_addr_set(ndev, info->mac);
		call_netdevice_notifiers(NETDEV_CHANGEADDR, ndev);
		rtnl_unlock();
	}

	if (link != nif->link || info->bps != nif->bps) {
		if (link)
			netdev_info(ndev, "link up, %u Mbit/s, %s duplex\n",
				    info->bps / 1000000, info->fullDuplex ? "full" : "half");
		else if (nif->link)
			netdev_info(ndev, "link down\n");
	}

	nif->present = present;
	nif->link = link;
	nif->full_duplex = info->fullDuplex;
	nif->bps = info->bps;

	if (link && !netif_carrier_ok(ndev))
		netif_carrier_on(ndev);
	else if (!link && netif_carrier_ok(ndev))
		netif_carrier_off(ndev);
}

static void warpnet_link_work(struct work_struct *work)
{
	struct warpnet *wn = container_of(to_delayed_work(work), struct warpnet, link_work);
	DprRplEthIfInfo info;
	int i;

	for (i = 0; i < ETH_IF_COUNT; i++) {
		struct warpnet_if *nif = wn->nif[i];

		if (!nif || !netif_running(nif->ndev))
			continue;
		if (warpnet_get_info(wn, nif->ifnr, &info)) {
			netdev_warn_once(nif->ndev, "dpcmdEthIfGetInfo failed\n");
			continue;
		}
		warpnet_update_info(nif, &info);
	}
	queue_delayed_work(wn->wq, &wn->link_work, msecs_to_jiffies(LINK_POLL_MS));
}

/* AUNIT_ETH0/1 doorbell: hard-irq context */
static void warpnet_doorbell(void *ctx)
{
	struct warpnet *wn = ctx;

	if (!test_and_set_bit(NET_RX_DB, &wn->flags))
		wn->db_time = ktime_get();
	set_bit(NET_RX_PENDING, &wn->flags);
	queue_work(wn->wq, &wn->io_work);
}

static void warpnet_rx_timer(struct timer_list *t)
{
	struct warpnet *wn = from_timer(wn, t, rx_timer);

	set_bit(NET_RX_PENDING, &wn->flags);
	queue_work(wn->wq, &wn->io_work);
	mod_timer(&wn->rx_timer, jiffies + msecs_to_jiffies(RX_POLL_MS));
}

/* ------------------------------------------------------------------------ */
/* netdev                                                                   */
/* ------------------------------------------------------------------------ */

static int warpnet_open(struct net_device *ndev)
{
	struct warpnet_if *nif = netdev_priv(ndev);
	struct warpnet *wn = nif->wn;
	int err;

	ASSERT_RTNL();
	cswarp_dbg(CSWARP_DBG_NET, &ndev->dev, "open (%s, if%u), %u already open\n",
		   if_names[nif->ifnr], nif->ifnr, wn->open_count);

	netif_carrier_off(ndev);
	nif->link = false;

	if (wn->open_count++ == 0) {
		err = cswarp_unit_register(wn->wc, AUNIT_ETH0, warpnet_doorbell, wn);
		if (!err) {
			err = cswarp_unit_register(wn->wc, AUNIT_ETH1, warpnet_doorbell, wn);
			if (err)
				cswarp_unit_unregister(wn->wc, AUNIT_ETH0);
		}
		if (err) {
			wn->open_count--;
			netdev_err(ndev, "cannot register the RX doorbells (%d)\n", err);
			return err;
		}
		mod_timer(&wn->rx_timer, jiffies + msecs_to_jiffies(RX_POLL_MS));
		/* drain whatever queued up while we were closed */
		set_bit(NET_RX_PENDING, &wn->flags);
		queue_work(wn->wq, &wn->io_work);
	}
	mod_delayed_work(wn->wq, &wn->link_work, 0);
	netif_start_queue(ndev);
	return 0;
}

static int warpnet_close(struct net_device *ndev)
{
	struct warpnet_if *nif = netdev_priv(ndev);
	struct warpnet *wn = nif->wn;

	ASSERT_RTNL();
	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	nif->link = false;

	if (--wn->open_count == 0) {
		cswarp_unit_unregister(wn->wc, AUNIT_ETH0);
		cswarp_unit_unregister(wn->wc, AUNIT_ETH1);
		del_timer_sync(&wn->rx_timer);
		cancel_delayed_work_sync(&wn->link_work);
		flush_workqueue(wn->wq);
	}
	skb_queue_purge(&nif->txq);

	cswarp_dbg(CSWARP_DBG_NET, &ndev->dev,
		   "closed, %u still open; rx dropped while down %lu, rx bad %lu\n",
		   wn->open_count, wn->rx_drops_down, wn->rx_bad);
	return 0;
}

static netdev_tx_t warpnet_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct warpnet_if *nif = netdev_priv(ndev);
	struct warpnet *wn = nif->wn;

	if (unlikely(skb->len > ETH_MTU_AND_HDR_SIZE)) {
		netdev_warn_once(ndev, "TX frame too long (%u), dropped\n", skb->len);
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	/* frees the skb on failure */
	if (skb_put_padto(skb, ETH_ZLEN)) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	*(ktime_t *)skb->cb = ktime_get();
	skb_queue_tail(&nif->txq, skb);
	if (skb_queue_len(&nif->txq) >= TXQ_STOP_LEN) {
		cswarp_dbg(CSWARP_DBG_NET, &ndev->dev, "TX queue full (%u), stopped\n",
			   skb_queue_len(&nif->txq));
		netif_stop_queue(ndev);
	}
	queue_work(wn->wq, &wn->io_work);
	return NETDEV_TX_OK;
}

static void warpnet_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct warpnet_if *nif = netdev_priv(ndev);

	netdev_err(ndev, "TX timeout, %u frames queued\n", skb_queue_len(&nif->txq));
	ndev->stats.tx_errors++;
	queue_work(nif->wn->wq, &nif->wn->io_work);
	netif_wake_queue(ndev);
}

static int warpnet_set_mac_address(struct net_device *ndev, void *addr)
{
	/* the board owns the MAC (WiFi module / USB adapter) */
	return -EOPNOTSUPP;
}

static void warpnet_set_rx_mode(struct net_device *ndev)
{
	/* the board delivers every frame it has; the stack filters */
}

static const struct net_device_ops warpnet_netdev_ops = {
	.ndo_open		= warpnet_open,
	.ndo_stop		= warpnet_close,
	.ndo_start_xmit		= warpnet_start_xmit,
	.ndo_tx_timeout		= warpnet_tx_timeout,
	.ndo_set_rx_mode	= warpnet_set_rx_mode,
	.ndo_set_mac_address	= warpnet_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ------------------------------------------------------------------------ */
/* ethtool                                                                  */
/* ------------------------------------------------------------------------ */

static void warpnet_get_drvinfo(struct net_device *ndev, struct ethtool_drvinfo *info)
{
	struct warpnet_if *nif = netdev_priv(ndev);

	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	strscpy(info->version, DRV_VERSION, sizeof(info->version));
	snprintf(info->bus_info, sizeof(info->bus_info), "warp-if%u (%s)",
		 nif->ifnr, if_names[nif->ifnr]);
}

static int warpnet_get_link_ksettings(struct net_device *ndev,
				      struct ethtool_link_ksettings *cmd)
{
	struct warpnet_if *nif = netdev_priv(ndev);

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	cmd->base.port = PORT_OTHER;
	cmd->base.autoneg = AUTONEG_DISABLE;
	if (nif->link) {
		cmd->base.speed = nif->bps / 1000000;
		cmd->base.duplex = nif->full_duplex ? DUPLEX_FULL : DUPLEX_HALF;
	} else {
		cmd->base.speed = SPEED_UNKNOWN;
		cmd->base.duplex = DUPLEX_UNKNOWN;
	}
	return 0;
}

static u32 warpnet_get_msglevel(struct net_device *ndev)
{
	struct warpnet_if *nif = netdev_priv(ndev);

	return nif->msg_enable;
}

static void warpnet_set_msglevel(struct net_device *ndev, u32 value)
{
	struct warpnet_if *nif = netdev_priv(ndev);

	nif->msg_enable = value;
}

static const struct ethtool_ops warpnet_ethtool_ops = {
	.get_drvinfo		= warpnet_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= warpnet_get_link_ksettings,
	.get_msglevel		= warpnet_get_msglevel,
	.set_msglevel		= warpnet_set_msglevel,
};

/* ------------------------------------------------------------------------ */
/* Probe                                                                    */
/* ------------------------------------------------------------------------ */

static int warpnet_add_if(struct warpnet *wn, u8 ifnr)
{
	struct net_device *ndev;
	struct warpnet_if *nif;
	DprRplEthIfInfo info;
	int err;

	ndev = alloc_etherdev(sizeof(*nif));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, wn->dev);

	nif = netdev_priv(ndev);
	nif->wn = wn;
	nif->ndev = ndev;
	nif->ifnr = ifnr;
	skb_queue_head_init(&nif->txq);

	ndev->netdev_ops = &warpnet_netdev_ops;
	ndev->ethtool_ops = &warpnet_ethtool_ops;
	ndev->watchdog_timeo = TX_TIMEOUT;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ETH_DATA_LEN;
	ndev->features |= NETIF_F_VLAN_CHALLENGED;

	err = warpnet_get_info(wn, ifnr, &info);
	if (err) {
		dev_err(wn->dev, "if%u: dpcmdEthIfGetInfo failed (%d)\n", ifnr, err);
		goto err_free;
	}
	dev_info(wn->dev, "if%u (%s): present %u, link %u, %u bps, mtu %u, mac %pM, board ifCount %u\n",
		 ifnr, if_names[ifnr], info.present, info.linkUp, info.bps, info.mtu,
		 info.mac, info.ifCount);

	if (info.present && is_valid_ether_addr(info.mac)) {
		eth_hw_addr_set(ndev, info.mac);
	} else {
		/* replaced by the board's MAC once the hardware shows up */
		eth_hw_addr_random(ndev);
		dev_info(wn->dev, "if%u: no hardware MAC yet, using random %pM\n",
			 ifnr, ndev->dev_addr);
	}
	nif->present = info.present;

	netif_carrier_off(ndev);
	err = register_netdev(ndev);
	if (err) {
		dev_err(wn->dev, "if%u: register_netdev failed (%d)\n", ifnr, err);
		goto err_free;
	}
	wn->nif[ifnr] = nif;
	netdev_info(ndev, "Warp %s interface (if%u), MAC %pM\n", if_names[ifnr], ifnr,
		    ndev->dev_addr);
	return 0;

err_free:
	free_netdev(ndev);
	return err;
}

static void warpnet_remove_ifs(struct warpnet *wn)
{
	int i;

	for (i = 0; i < ETH_IF_COUNT; i++) {
		if (!wn->nif[i])
			continue;
		unregister_netdev(wn->nif[i]->ndev);
		skb_queue_purge(&wn->nif[i]->txq);
		free_netdev(wn->nif[i]->ndev);
		wn->nif[i] = NULL;
	}
}

/* ------------------------------------------------------------------------ */
/* sysfs: latency diagnostics                                               */
/* ------------------------------------------------------------------------ */

static ssize_t latency_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct warpnet *wn = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "rx_doorbell_to_drain_us avg %llu max %u n %lu\n"
			  "tx_queue_to_board_us avg %llu max %u n %lu\n"
			  "rx_frames_by_doorbell %lu\nrx_frames_by_timer %lu\nio_work_runs %lu\n",
			  wn->lat_db.n ? div_u64(wn->lat_db.total, wn->lat_db.n) : 0ULL,
			  wn->lat_db.max, wn->lat_db.n,
			  wn->lat_tx.n ? div_u64(wn->lat_tx.total, wn->lat_tx.n) : 0ULL,
			  wn->lat_tx.max, wn->lat_tx.n,
			  wn->rx_frames_db, wn->rx_frames_timer, wn->io_runs);
}

static ssize_t latency_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct warpnet *wn = dev_get_drvdata(dev);

	/* any write resets */
	memset(&wn->lat_db, 0, sizeof(wn->lat_db));
	memset(&wn->lat_tx, 0, sizeof(wn->lat_tx));
	wn->rx_frames_db = 0;
	wn->rx_frames_timer = 0;
	wn->io_runs = 0;
	return count;
}
static DEVICE_ATTR_RW(latency);

/*
 * Board-side network telemetry (dpcmdGetNetTelemetry), named after
 * DprNetTelemetry, schema version 8: 16 header words, 2 x 8 interface words,
 * 49 ASIX words (skipped), 2 x 7 command timing words, 8 ESP words.
 */
#define TLM_IFACE	16
#define TLM_CMDTX	(TLM_IFACE + 2 * 8 + 49)
#define TLM_CMDRX	(TLM_CMDTX + 7)
#define TLM_ESP		(TLM_CMDRX + 7)

static int warpnet_fetch_tlm(struct warpnet *wn, u32 flags, u32 *w)
{
	struct {
		DprCmdHeader header;
		u32 flags;
	} __packed cmd = { .header.cmd = dpcmdGetNetTelemetry, .flags = flags };
	u32 rpl[1 + NETTLM_WORDS];
	int err;

	if (!cswarp_fw_at_least(wn->wc, WARP_FMW_VER_MIN_NETTLM))
		return -EOPNOTSUPP;
	err = cswarp_xfer(wn->wc, &cmd, sizeof(cmd), rpl, sizeof(rpl), dprplNetTelemetry);
	if (!err)
		memcpy(w, &rpl[1], NETTLM_WORDS * 4);
	return err;
}

static int tlm_emit_cmd(char *buf, int len, const char *name, const u32 *c)
{
	u32 n = c[0] ? c[0] : 1;

	return sysfs_emit_at(buf, len,
			     "%s: count %u  turn avg %u max %u  wait avg %u max %u  serve avg %u max %u (us)\n",
			     name, c[0], c[1] / n, c[2], c[3] / n, c[4], c[5] / n, c[6]);
}

static ssize_t board_telemetry_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct warpnet *wn = dev_get_drvdata(dev);
	u32 *w;
	int err, len, i;

	w = kmalloc_array(NETTLM_WORDS, sizeof(u32), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	err = warpnet_fetch_tlm(wn, 0, w);
	if (err) {
		kfree(w);
		return err;
	}

	len = sysfs_emit(buf, "version %u%s  uptime %u ms\n", w[0],
			 w[0] == NETTLM_VERSION ? "" : " (UNEXPECTED, field names may be wrong)", w[12]);
	len += sysfs_emit_at(buf, len, "pool %u blocks, min free %u, queue max %u\n", w[4], w[5], w[6]);
	len += sysfs_emit_at(buf, len, "rx polls %u (empty %u), doorbells %u avg %u max %u us\n",
			     w[7], w[8], w[9], w[9] ? w[10] / w[9] : 0, w[11]);
	len += sysfs_emit_at(buf, len, "esp tx dropped %u, waited %u, wait max %u us\n",
			     w[13], w[14], w[15]);
	for (i = 0; i < 2; i++) {
		const u32 *f = &w[TLM_IFACE + 8 * i];

		len += sysfs_emit_at(buf, len,
				     "if%d: queued %u delivered %u dropQueueFull %u dropPoolEmpty %u dropSendFail %u peak %u present %u link %u\n",
				     i, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
	}
	len += tlm_emit_cmd(buf, len, "cmd tx", &w[TLM_CMDTX]);
	len += tlm_emit_cmd(buf, len, "cmd rx", &w[TLM_CMDRX]);
	len += sysfs_emit_at(buf, len, "esp spi: transactions %u avg %u max %u us\n",
			     w[TLM_ESP], w[TLM_ESP] ? w[TLM_ESP + 1] / w[TLM_ESP] : 0, w[TLM_ESP + 2]);
	len += sysfs_emit_at(buf, len, "esp radio: valid %u txFrames %u txNoMem %u txDropped %u busy %u\n",
			     w[TLM_ESP + 3], w[TLM_ESP + 4], w[TLM_ESP + 5], w[TLM_ESP + 6], w[TLM_ESP + 7]);
	kfree(w);
	return len;
}

/* write anything: reset the board's counters */
static ssize_t board_telemetry_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct warpnet *wn = dev_get_drvdata(dev);
	u32 *w;
	int err;

	w = kmalloc_array(NETTLM_WORDS, sizeof(u32), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	err = warpnet_fetch_tlm(wn, NETTLM_FLAG_RESET, w);
	kfree(w);
	return err ? err : count;
}
static DEVICE_ATTR_RW(board_telemetry);

static int warpnet_probe(struct platform_device *pdev)
{
	struct cswarp_core *wc = cswarp_core_get(pdev);
	struct warpnet *wn;
	int err, i, added = 0;

	if (!wc)
		return -ENODEV;

	if (!cswarp_fw_at_least(wc, WARP_FMW_VER_MIN_ETHIF)) {
		dev_err(&pdev->dev, "firmware 0x%04x has no dpcmdEthIf* commands\n", wc->fw_ver);
		return -ENODEV;
	}

	wn = devm_kzalloc(&pdev->dev, sizeof(*wn), GFP_KERNEL);
	if (!wn)
		return -ENOMEM;
	wn->wc = wc;
	wn->dev = &pdev->dev;
	INIT_WORK(&wn->io_work, warpnet_io_work);
	INIT_DELAYED_WORK(&wn->link_work, warpnet_link_work);
	timer_setup(&wn->rx_timer, warpnet_rx_timer, 0);

	wn->wq = alloc_ordered_workqueue("amiwarpnet", WQ_MEM_RECLAIM);
	if (!wn->wq)
		return -ENOMEM;
	platform_set_drvdata(pdev, wn);

	for (i = 0; i < ETH_IF_COUNT; i++) {
		err = warpnet_add_if(wn, i);
		if (!err)
			added++;
	}
	if (!added) {
		destroy_workqueue(wn->wq);
		return -ENODEV;
	}
	if (device_create_file(&pdev->dev, &dev_attr_latency) ||
	    device_create_file(&pdev->dev, &dev_attr_board_telemetry))
		dev_warn(&pdev->dev, "sysfs diagnostics attributes not created\n");
	cswarp_dbg(CSWARP_DBG_NET, &pdev->dev, "%d of %d interfaces registered\n",
		   added, ETH_IF_COUNT);
	return 0;
}

static void warpnet_remove(struct platform_device *pdev)
{
	struct warpnet *wn = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_latency);
	device_remove_file(&pdev->dev, &dev_attr_board_telemetry);
	/* closes the interfaces, which stops the doorbells, timer and link poll */
	warpnet_remove_ifs(wn);
	del_timer_sync(&wn->rx_timer);
	cancel_delayed_work_sync(&wn->link_work);
	cancel_work_sync(&wn->io_work);
	destroy_workqueue(wn->wq);
}

static struct platform_driver warpnet_driver = {
	.probe		= warpnet_probe,
	.remove_new	= warpnet_remove,
	.driver		= {
		.name	= "cswarp-net",
	},
};
module_platform_driver(warpnet_driver);

MODULE_ALIAS("platform:cswarp-net");
MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("CS-Lab Warp turbo board network driver (WiFi + USB ethernet)");
MODULE_LICENSE("GPL");
