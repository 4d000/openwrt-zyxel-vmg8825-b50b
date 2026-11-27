// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom BCM63138 Runner/RDP DSA Conduit Network Driver
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <net/page_pool/helpers.h>

typedef void *bdmf_sysb;
#include "access_macros.h"
#include "data_path_init.h"
#include "rdd.h"
#include "rdp_cpu_ring_defs.h"
#include "bcm63138_flow_offload.h"

extern struct device *rdp_dummy_dev;
extern void f_basic_bpm_sp_enable(void);
extern void f_basic_sbpm_sp_enable(void);
extern int rdd_cpu_tx_abs_init(void);
extern void rdd_cpu_tx_reclaim_abs(struct device *dev);
extern BL_LILAC_RDD_ERROR_DTE rdd_cpu_tx_write_eth_abs(struct device *dev,
						       struct sk_buff *skb,
						       BL_LILAC_RDD_EMAC_ID_DTE xi_emac_id,
						       BL_LILAC_RDD_QUEUE_ID_DTE xi_queue_id);

#define DRV_NAME		"bcm63138-conduit"
#define RUNNER_RX_RING_ENTRIES	512
#define RUNNER_RX_RING_ID	0
#define RX_BUF_SIZE		2048
#define RX_FRAG_SIZE		2048
#define RX_HEADROOM		(NET_SKB_PAD + NET_IP_ALIGN)
#define RX_DMA_LEN		(RX_FRAG_SIZE - RX_HEADROOM - SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))

struct bcm63138_conduit_priv {
	struct net_device *netdev;
	struct device *dev;
	struct napi_struct napi;
	struct hrtimer poll_timer;
	struct page_pool *page_pool;
	bool use_irq;
	bool irq_seen;

	void __iomem *rdp_base;
	void *rsv_virt;
	phys_addr_t rsv_phys;
	size_t rsv_size;

	volatile CPU_RX_DESCRIPTOR *rx_ring;
	dma_addr_t rx_ring_dma;
	struct page *rx_pages[RUNNER_RX_RING_ENTRIES];
	u32 rx_offsets[RUNNER_RX_RING_ENTRIES];
	u32 rx_head;

	struct bcm63138_flow_offload *flow_offload;
};

static int bcm63138_refill_rx(struct bcm63138_conduit_priv *priv, int idx)
{
	volatile CPU_RX_DESCRIPTOR *desc = &priv->rx_ring[idx];
	struct page *page;
	u32 offset;
	dma_addr_t dma;

	page = page_pool_dev_alloc_frag(priv->page_pool, &offset, RX_FRAG_SIZE);
	if (unlikely(!page))
		return -ENOMEM;

	dma = page_pool_get_dma_addr(page) + offset + RX_HEADROOM;
	priv->rx_pages[idx] = page;
	priv->rx_offsets[idx] = offset;

	desc->word0 = 0;
	desc->word1 = 0;
	desc->word3 = 0;
	dma_wmb();
	/* Big-endian descriptor word2: bit 31 = 0 (RUNNER ownership) */
	WRITE_ONCE(desc->word2, cpu_to_be32((u32)dma & 0x7fffffffU));

	return 0;
}

static int bcm63138_conduit_poll(struct napi_struct *napi, int budget)
{
	struct bcm63138_conduit_priv *priv =
		container_of(napi, struct bcm63138_conduit_priv, napi);
	LIST_HEAD(rx_list);
	int work_done = 0;
	u32 idx = priv->rx_head;

	while (work_done < budget) {
		volatile CPU_RX_DESCRIPTOR *desc = &priv->rx_ring[idx];
		struct page *page, *new_page;
		struct sk_buff *skb;
		u32 offset, new_offset;
		dma_addr_t dma, new_dma;
		u32 w2_raw, w0, len, next_idx;
		void *va;

		w2_raw = READ_ONCE(desc->word2);
		/* Runner sets big-endian bit 31 = LE 0x00000080 for HOST ownership */
		if (!(w2_raw & 0x80U))
			break;

		next_idx = (idx + 1) & (RUNNER_RX_RING_ENTRIES - 1);

		dma_rmb();
		w0 = be32_to_cpu(READ_ONCE(desc->word0));
		len = w0 & 0x3fffU;

		page = priv->rx_pages[idx];
		offset = priv->rx_offsets[idx];
		dma = page_pool_get_dma_addr(page) + offset + RX_HEADROOM;

		if (unlikely(!page || len == 0 || len > RX_DMA_LEN)) {
			desc->word0 = 0;
			desc->word1 = 0;
			desc->word3 = 0;
			dma_wmb();
			WRITE_ONCE(desc->word2,
				   cpu_to_be32((u32)dma & 0x7fffffffU));
			dev_core_stats_rx_dropped_inc(priv->netdev);
			idx = next_idx;
			work_done++;
			continue;
		}

		new_page = page_pool_dev_alloc_frag(priv->page_pool,
						    &new_offset, RX_FRAG_SIZE);
		if (unlikely(!new_page)) {
			desc->word0 = 0;
			desc->word1 = 0;
			desc->word3 = 0;
			dma_wmb();
			WRITE_ONCE(desc->word2,
				   cpu_to_be32((u32)dma & 0x7fffffffU));
			dev_core_stats_rx_dropped_inc(priv->netdev);
			idx = next_idx;
			work_done++;
			continue;
		}

		dma_sync_single_for_cpu(priv->dev, dma, len, DMA_FROM_DEVICE);
		va = page_address(page) + offset;
		prefetch(va + RX_HEADROOM);

		new_dma = page_pool_get_dma_addr(new_page) + new_offset + RX_HEADROOM;
		/* Only header/headroom bytes can ever be dirtied by CPU (DSA/NAT/DHDHDR) */
		dma_sync_single_for_device(priv->dev, new_dma - 16, 160, DMA_FROM_DEVICE);
		priv->rx_pages[idx] = new_page;
		priv->rx_offsets[idx] = new_offset;
		desc->word0 = 0;
		desc->word1 = 0;
		desc->word3 = 0;
		dma_wmb();
		WRITE_ONCE(desc->word2, cpu_to_be32((u32)new_dma & 0x7fffffffU));

		skb = napi_build_skb(va, RX_FRAG_SIZE);
		if (unlikely(!skb)) {
			page_pool_recycle_direct(priv->page_pool, page);
			dev_core_stats_rx_dropped_inc(priv->netdev);
			idx = next_idx;
			work_done++;
			continue;
		}

		skb_mark_for_recycle(skb);
		skb_reserve(skb, RX_HEADROOM);
		skb_put(skb, len);
		if (w0 & BIT(19))
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		skb->protocol = eth_type_trans(skb, priv->netdev);
		list_add_tail(&skb->list, &rx_list);

		dev_sw_netstats_rx_add(priv->netdev, len);

		idx = next_idx;
		work_done++;
	}

	priv->rx_head = idx;

	if (!list_empty(&rx_list))
		netif_receive_skb_list(&rx_list);

	/* Reclaim TX skbs inside NAPI on CPU1 for lockless napi_skb_cache & page_pool recycling */
	rdd_cpu_tx_reclaim_abs(priv->dev);

	if (work_done < budget) {
		if (READ_ONCE(priv->rx_ring[priv->rx_head].word2) & 0x80U)
			return budget;
		if (napi_complete_done(napi, work_done) && priv->use_irq)
			rdd_interrupt_unmask(0, RUNNER_RX_RING_ID);
	}

	return work_done;
}

static enum hrtimer_restart bcm63138_timer_poll(struct hrtimer *timer)
{
	struct bcm63138_conduit_priv *priv =
		container_of(timer, struct bcm63138_conduit_priv, poll_timer);
	volatile CPU_RX_DESCRIPTOR *desc = &priv->rx_ring[priv->rx_head];

	if (READ_ONCE(priv->irq_seen))
		return HRTIMER_NORESTART;

	if (READ_ONCE(desc->word2) & 0x80U)
		napi_schedule(&priv->napi);

	hrtimer_forward_now(timer, ns_to_ktime(100000)); /* 100 us fallback */
	return HRTIMER_RESTART;
}

static irqreturn_t bcm63138_conduit_isr(int irq, void *dev_id)
{
	struct bcm63138_conduit_priv *priv = dev_id;

	rdd_interrupt_mask(0, RUNNER_RX_RING_ID);
	rdd_interrupt_clear(0, RUNNER_RX_RING_ID);
	WRITE_ONCE(priv->irq_seen, true);
	napi_schedule(&priv->napi);
	return IRQ_HANDLED;
}

static netdev_tx_t bcm63138_conduit_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct bcm63138_conduit_priv *priv = netdev_priv(dev);
	BL_LILAC_RDD_ERROR_DTE rc;
	unsigned int len;

	if (unlikely(skb_linearize(skb))) {
		dev_core_stats_tx_dropped_inc(dev);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (unlikely(skb->len < ETH_ZLEN + 4 && skb_put_padto(skb, ETH_ZLEN + 4))) {
		dev_core_stats_tx_dropped_inc(dev);
		return NETDEV_TX_OK;
	}

	len = skb->len;

	/* Zero-copy Runner DMA TX via ABSOLUTE_ADDRESS_PACKET */
	rc = rdd_cpu_tx_write_eth_abs(priv->dev, skb, BL_LILAC_RDD_EMAC_ID_1, 0);

	if (likely(rc == BL_LILAC_RDD_OK)) {
		dev_sw_netstats_tx_add(dev, 1, len);
	} else {
		dev_core_stats_tx_dropped_inc(dev);
		dev_kfree_skb_any(skb);
	}

	return NETDEV_TX_OK;
}

static int bcm63138_conduit_open(struct net_device *dev)
{
	struct bcm63138_conduit_priv *priv = netdev_priv(dev);

	WRITE_ONCE(priv->irq_seen, false);
	napi_enable(&priv->napi);
	if (priv->use_irq) {
		rdd_interrupt_clear(0, RUNNER_RX_RING_ID);
		rdd_interrupt_unmask(0, RUNNER_RX_RING_ID);
	}
	hrtimer_start(&priv->poll_timer, ns_to_ktime(100000), HRTIMER_MODE_REL);

	netif_carrier_on(dev);
	netif_start_queue(dev);
	return 0;
}

static int bcm63138_conduit_stop(struct net_device *dev)
{
	struct bcm63138_conduit_priv *priv = netdev_priv(dev);

	netif_stop_queue(dev);
	netif_carrier_off(dev);

	if (priv->use_irq) {
		rdd_interrupt_mask(0, RUNNER_RX_RING_ID);
		rdd_interrupt_clear(0, RUNNER_RX_RING_ID);
	}
	hrtimer_cancel(&priv->poll_timer);
	napi_disable(&priv->napi);

	return 0;
}

static int bcm63138_conduit_setup_tc(struct net_device *dev,
				     enum tc_setup_type type, void *type_data)
{
	struct bcm63138_conduit_priv *priv = netdev_priv(dev);

	return bcm63138_flow_offload_setup_tc(priv->flow_offload, type,
					      type_data);
}

static const struct net_device_ops bcm63138_conduit_netdev_ops = {
	.ndo_open = bcm63138_conduit_open,
	.ndo_stop = bcm63138_conduit_stop,
	.ndo_start_xmit = bcm63138_conduit_xmit,
	.ndo_get_stats64 = dev_get_tstats64,
	.ndo_setup_tc = bcm63138_conduit_setup_tc,
};

static int bcm63138_rdp_power_and_ubus_init(struct bcm63138_conduit_priv *priv)
{
	struct device *dev = priv->dev;
	void __iomem *pmb, *ubus;
	int timeout;

	/* 0. Power up and deassert reset on RDP (PMBM[1], Dev 7) */
	pmb = ioremap(0x80480000, 0x100);
	if (!pmb)
		return -ENOMEM;

	timeout = 100000;
	writel(0x00000000, pmb + 0xe4);
	writel(0x81107008, pmb + 0xe0);
	while ((readl(pmb + 0xe0) & 0x80000000) && --timeout)
		cpu_relax();
	dev_info(dev, "PMB RDP pwd_control=0 timeout_left=%d ctrl=0x%08x\n",
		 timeout, readl(pmb + 0xe0));

	timeout = 100000;
	writel(0x00ffffff, pmb + 0xe4);
	writel(0x8110700a, pmb + 0xe0);
	while ((readl(pmb + 0xe0) & 0x80000000) && --timeout)
		cpu_relax();
	dev_info(dev, "PMB RDP sr_control=0x00ffffff timeout_left=%d ctrl=0x%08x\n",
		 timeout, readl(pmb + 0xe0));
	udelay(100);
	iounmap(pmb);

	/* 1. Enable UBUS Masters */
	ubus = ioremap(0x802d2000, 0x1000);
	if (!ubus)
		return -ENOMEM;

	writel(0x01000000, ubus + 0x000); /* UBUS Master 1 */
	writel(0x01000000, ubus + 0x400); /* UBUS Master 2 */
	writel(0x01000000, ubus + 0x800); /* UBUS Master 3 */
	writel(0x010ef000, ubus + 0x80c); /* High priority forwarding */
	iounmap(ubus);

	return 0;
}

static void bcm63138_unimac_init(struct bcm63138_conduit_priv *priv)
{
	void __iomem *unimac_top, *unimac;

	/* UNIMAC TOP MISC: GMII direct mode to SF2 IMP Port 8 */
	unimac_top = ioremap(0x802db800, 0x800);
	if (unimac_top) {
		writel(0x01000000, unimac_top + 0x400);
		iounmap(unimac_top);
	}

	/* UNIMAC 1 (EMAC 1): 1000M full-duplex, promis_en, crc_fwd, pause_fwd, no_lgth_check */
	unimac = ioremap(0x802d5000, 0x100);
	if (unimac) {
		writel(cpu_to_be32(1536), unimac + 0x14); /* FRM_LEN */
		writel(cpu_to_be32(0x010020d8), unimac + 0x08); /* sw_reset=1 */
		udelay(10);
		writel(cpu_to_be32(0x010000d8), unimac + 0x08); /* sw_reset=0 */
		udelay(10);
		writel(cpu_to_be32(0x010000db), unimac + 0x08); /* tx_ena=1, rx_ena=1 */
		iounmap(unimac);
	}
}

static int bcm63138_rdp_sdk_init(struct bcm63138_conduit_priv *priv)
{
	struct device *dev = priv->dev;
	struct device_node *rsv_np;
	struct resource rsv_res;
	S_DPI_CFG dpi_cfg = {};
	uint32_t rc;
	int i;

	priv->rdp_base = devm_ioremap(dev, 0x80200000, 0x100000);
	if (!priv->rdp_base)
		return -ENOMEM;

	soc_base_address = (uint8_t __force *)priv->rdp_base;
	rdp_dummy_dev = dev;

	rsv_np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!rsv_np)
		rsv_np = of_find_node_by_name(NULL, "runner-reserved");
	if (!rsv_np) {
		dev_err(dev, "Missing runner-reserved memory node\n");
		return -ENODEV;
	}

	if (of_address_to_resource(rsv_np, 0, &rsv_res)) {
		of_node_put(rsv_np);
		return -EINVAL;
	}
	of_node_put(rsv_np);

	priv->rsv_phys = rsv_res.start;
	priv->rsv_size = resource_size(&rsv_res);
	if (priv->rsv_size < (20 << 20)) {
		dev_err(dev, "runner-reserved size %zu < 20MB\n", priv->rsv_size);
		return -EINVAL;
	}

	priv->rsv_virt = devm_memremap(dev, priv->rsv_phys, priv->rsv_size,
				       MEMREMAP_WC);
	if (IS_ERR(priv->rsv_virt)) {
		dev_err(dev, "devm_memremap runner-reserved failed: %ld\n",
			PTR_ERR(priv->rsv_virt));
		return PTR_ERR(priv->rsv_virt);
	}

	{
		u32 tm_mb = (priv->rsv_size >= (28 << 20)) ? 24 : 16;

		dpi_cfg.mtu_size = 1536;
		/* 40B DDR headroom + 18B DDR_PACKET_PAYLOAD_OFFSET = 58B (RDPA_CMD_LIST_HEADROOM) */
		dpi_cfg.headroom_size = 40;
		dpi_cfg.runner_freq = 800;
		dpi_cfg.runner_tm_base_addr = (uint32_t)(uintptr_t)priv->rsv_virt;
		dpi_cfg.runner_tm_base_addr_phys = (uint32_t)priv->rsv_phys;
		dpi_cfg.runner_tm_size = tm_mb;
		dpi_cfg.runner_mc_base_addr = (uint32_t)((uintptr_t)priv->rsv_virt + (tm_mb << 20));
		dpi_cfg.runner_mc_base_addr_phys = (uint32_t)(priv->rsv_phys + (tm_mb << 20));
		dpi_cfg.runner_mc_size = 4;
		dpi_cfg.runner_lp = 0;
	}

	dev_info(dev, "Calling data_path_init: TM=0x%08x (%uMB) MC=0x%08x (%uMB) headroom=%uB\n",
		 dpi_cfg.runner_tm_base_addr_phys, dpi_cfg.runner_tm_size,
		 dpi_cfg.runner_mc_base_addr_phys, dpi_cfg.runner_mc_size,
		 dpi_cfg.headroom_size);

	rc = data_path_init(&dpi_cfg);
	if (rc != DPI_RC_OK) {
		dev_err(dev, "data_path_init failed: %u\n", rc);
		return -EIO;
	}

	/* Full-RDD pre-enable setup matching Broadcom RDPA system_data_path_init */
	for (i = 0; i <= 16; i++) {
		rdd_wan_channel_set(i, 0, 0);
		rdd_wan_channel_rate_limiter_config(i, 0, 0);
	}
	rdd_us_wan_flow_config(0, 0, 0, 0, 1, 0, 0, 0);

	for (i = 0; i < rdpa_cpu_reason__num_of; i++) {
		rdd_cpu_reason_to_cpu_rx_queue(i, 0, rdpa_dir_us, 0);
		rdd_cpu_reason_to_cpu_rx_queue(i, 0, rdpa_dir_ds, 0);
		rdd_cpu_reason_to_cpu_rx_queue(i, 0, rdpa_dir_ds, 1); /* ethwan2 */
	}

	/* Map Broadcom switch ports 0..7 to Runner LAN bridge ports + lookup ports */
	for (i = 0; i < 8; i++)
		rdd_broadcom_switch_ports_mapping_table_config(
			BL_LILAC_RDD_LAN0_BRIDGE_PORT + i, i);
	rdd_broadcom_switch_ports_mapping_table_config(BL_LILAC_RDD_WAN_ROUTER_PORT, 8);

	for (i = 0; i <= 8; i++)
		rdd_lookup_ports_mapping_table_init(i, i);
	rdd_lookup_ports_mapping_table_init(10, 10);
	rdd_lookup_ports_mapping_table_init(11, 11);
	rdd_lookup_ports_mapping_table_init(13, 13);

	/* Allocate and populate Runner CPU RX Ring 0 with page_pool */
	{
		struct page_pool_params pp_params = {
			.flags = PP_FLAG_DMA_MAP,
			.order = 0,
			.pool_size = RUNNER_RX_RING_ENTRIES * 2,
			.nid = NUMA_NO_NODE,
			.dev = dev,
			.napi = &priv->napi,
			.dma_dir = DMA_FROM_DEVICE,
		};

		priv->page_pool = page_pool_create(&pp_params);
		if (IS_ERR(priv->page_pool)) {
			int err = PTR_ERR(priv->page_pool);

			priv->page_pool = NULL;
			dev_err(dev, "page_pool_create failed: %d\n", err);
			return err;
		}
	}

	priv->rx_ring = dmam_alloc_coherent(dev,
					    RUNNER_RX_RING_ENTRIES * sizeof(CPU_RX_DESCRIPTOR),
					    &priv->rx_ring_dma, GFP_KERNEL);
	if (!priv->rx_ring)
		return -ENOMEM;

	memset((void *)priv->rx_ring, 0,
	       RUNNER_RX_RING_ENTRIES * sizeof(CPU_RX_DESCRIPTOR));
	for (i = 0; i < RUNNER_RX_RING_ENTRIES; i++) {
		int err = bcm63138_refill_rx(priv, i);

		if (err)
			return err;
	}
	priv->rx_head = 0;
	wmb();

	rc = rdd_ring_init(RUNNER_RX_RING_ID, 0,
			   (rdd_phys_addr_t)priv->rx_ring_dma,
			   RUNNER_RX_RING_ENTRIES, sizeof(CPU_RX_DESCRIPTOR),
			   0, 0, 0, 0);
	if (rc != BL_LILAC_RDD_OK) {
		dev_err(dev, "rdd_ring_init failed: %u\n", rc);
		return -EIO;
	}

	rc = data_path_go();
	if (rc != DPI_RC_OK) {
		dev_err(dev, "data_path_go failed: %u\n", rc);
		return -EIO;
	}

	/* Post-enable Full-RDD BPM/SBPM, headroom, TX queues, and FC global cfg */
	rdd_ddr_headroom_size_config(dpi_cfg.headroom_size);
	f_basic_bpm_sp_enable();
	f_basic_sbpm_sp_enable();
	{
		int emac, q;
		RDD_FC_GLOBAL_CFG_ENTRY_DTS fc_cfg = {
			.fc_accel_mode = 1,
			.fc_tcp_ack_mflows = 0,
		};

		for (emac = BL_LILAC_RDD_EMAC_ID_0; emac < BL_LILAC_RDD_EMAC_ID_COUNT; emac++) {
			for (q = 0; q < 8; q++) {
				rdd_eth_tx_queue_config(emac, q, 256,
							rdd_queue_profile_disabled,
							INVALID_COUNTER_ID);
			}
		}
		rdd_fc_global_cfg_set(&fc_cfg);
	}

	if (rdd_cpu_tx_abs_init()) {
		dev_err(dev, "rdd_cpu_tx_abs_init failed\n");
		return -ENOMEM;
	}

	/* Hardware RX IRQ coalescing: max 120 us timeout or 64 packets per IRQ */
	rc = rdd_cpu_rx_interrupt_coalescing_config(RUNNER_RX_RING_ID, 120, 64);
	if (rc != BL_LILAC_RDD_OK)
		dev_warn(dev, "RX IRQ coalescing config failed: %u\n", rc);

	bcm63138_unimac_init(priv);

	dev_info(dev, "SDK RDP init complete (RX ring dma=%pad, RNR0 ctrl=0x%08x)\n",
		 &priv->rx_ring_dma, readl(priv->rdp_base + 0x99000));
	return 0;
}

static void bcm63138_free_rx_skbs(struct bcm63138_conduit_priv *priv)
{
	int i;

	if (!priv->page_pool)
		return;

	for (i = 0; i < RUNNER_RX_RING_ENTRIES; i++) {
		if (priv->rx_pages[i]) {
			page_pool_put_full_page(priv->page_pool,
						priv->rx_pages[i], false);
			priv->rx_pages[i] = NULL;
		}
	}
	page_pool_destroy(priv->page_pool);
	priv->page_pool = NULL;
}

static int bcm63138_conduit_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct bcm63138_conduit_priv *priv;
	int irq, ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "Failed to set 32-bit DMA mask: %d\n", ret);
		return ret;
	}

	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	priv = netdev_priv(ndev);
	priv->netdev = ndev;
	priv->dev = dev;

	ret = of_get_ethdev_address(dev->of_node, ndev);
	if (ret)
		eth_hw_addr_random(ndev);

	ndev->netdev_ops = &bcm63138_conduit_netdev_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = 1536;
	/* DSA conduit uses internal Runner TX lock; bypass qdisc & HARD_TX_LOCK */
	ndev->priv_flags |= IFF_NO_QUEUE;
	ndev->lltx = true;
	ndev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	ndev->features = NETIF_F_RXCSUM | NETIF_F_HW_TC;
	ndev->hw_features = NETIF_F_RXCSUM | NETIF_F_HW_TC;

	ret = bcm63138_rdp_power_and_ubus_init(priv);
	if (ret)
		goto err_free_netdev;

	netif_napi_add_weight(ndev, &priv->napi, bcm63138_conduit_poll, 128);
	hrtimer_setup(&priv->poll_timer, bcm63138_timer_poll,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	ret = bcm63138_rdp_sdk_init(priv);
	if (ret)
		goto err_free_rx;

	priv->flow_offload = bcm63138_flow_offload_init(ndev);
	if (IS_ERR(priv->flow_offload)) {
		dev_warn(dev, "Flow offload init failed (%ld), continuing with CPU conduit\n",
			 PTR_ERR(priv->flow_offload));
		priv->flow_offload = NULL;
	}

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		rdd_interrupt_mask(0, RUNNER_RX_RING_ID);
		rdd_interrupt_clear(0, RUNNER_RX_RING_ID);
		ret = devm_request_irq(dev, irq, bcm63138_conduit_isr, 0,
				       ndev->name, priv);
		if (ret) {
			dev_warn(dev, "IRQ %d request failed (%d), using hrtimer polling\n",
				 irq, ret);
		} else {
			priv->use_irq = true;
			dev_info(dev, "Using hardware RX IRQ %d (with hrtimer fallback)\n", irq);
		}
	}

	platform_set_drvdata(pdev, ndev);

	ret = register_netdev(ndev);
	if (ret) {
		dev_err(dev, "Failed to register netdevice: %d\n", ret);
		goto err_free_offload;
	}

	netif_carrier_off(ndev);
	dev_info(dev, "BCM63138 DSA conduit %s registered\n", ndev->name);
	return 0;

err_free_offload:
	bcm63138_flow_offload_exit(priv->flow_offload);
err_free_rx:
	bcm63138_free_rx_skbs(priv);
err_free_netdev:
	free_netdev(ndev);
	return ret;
}

static void bcm63138_conduit_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct bcm63138_conduit_priv *priv = netdev_priv(ndev);

	unregister_netdev(ndev);
	bcm63138_flow_offload_exit(priv->flow_offload);
	bcm63138_free_rx_skbs(priv);
	free_netdev(ndev);
}

static const struct of_device_id bcm63138_conduit_of_match[] = {
	{ .compatible = "brcm,bcm63138-conduit" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, bcm63138_conduit_of_match);

static struct platform_driver bcm63138_conduit_driver = {
	.probe = bcm63138_conduit_probe,
	.remove = bcm63138_conduit_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = bcm63138_conduit_of_match,
	},
};

module_platform_driver(bcm63138_conduit_driver);

MODULE_AUTHOR("OpenWrt");
MODULE_DESCRIPTION("Broadcom BCM63138 Runner/RDP DSA Conduit Driver");
MODULE_LICENSE("GPL");
