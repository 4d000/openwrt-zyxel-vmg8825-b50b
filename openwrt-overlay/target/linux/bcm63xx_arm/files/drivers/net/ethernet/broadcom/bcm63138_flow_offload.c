// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux Netfilter Flowtable Hardware Offload (TC_SETUP_FT) for
 * Broadcom BCM63138 Runner/RDP DSA Conduit
 *
 * Bridges Linux nf_flow_table hardware offload rules across Starfighter 2 DSA
 * user ports into BCM63138 Runner B Flow Cache + GPE command lists, while
 * returning -EOPNOTSUPP for Wi-Fi or non-SF2 flows so software fastpath
 * (FLOW_OFFLOAD_XMIT_DIRECT) remains active for hybrid traffic.
 */

#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_pppox.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/rhashtable.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/netfilter/nf_flow_table.h>
#include <net/pkt_cls.h>

typedef void *bdmf_sysb;
#include "access_macros.h"
#include "data_path_init.h"
#include "rdd.h"
#include "rdp_drv_ih.h"

#include "bcm63138_rdp/bcm63138_cmdlist.h"
#include "bcm63138_flow_offload.h"

/* IH DA filter indices 0..2 are reserved by data_path_init (IPTV/broadcast).
 * Slots 3..7 (5 hardware filters) + 16 Runner FW MAC slots = 21 router MACs.
 */
#define BCM63138_IH_DA_FILT_BASE	3
#define BCM63138_IH_DA_FILT_COUNT	5
#define BCM63138_FW_DA_FILT_COUNT	16
#define BCM63138_MAX_HOST_MACS		(BCM63138_IH_DA_FILT_BASE == 3 ? \
					 (BCM63138_IH_DA_FILT_COUNT + BCM63138_FW_DA_FILT_COUNT) : 0)

struct bcm63138_hw_flow {
	struct rhash_head node;
	struct list_head list;
	unsigned long cookie;

	u32 ctx_idx;
	u32 hw_pkts;
	u32 hw_bytes;
	u64 total_pkts;
	u64 total_bytes;
	unsigned long last_used;

	u8 in_sf2_port;
	u8 out_sf2_port;
	u8 ip_proto;
	bool is_ipv6;
	u8 cmdlist_len64;
	u16 mtu;

	__be16 orig_sport;
	__be16 orig_dport;
	__be16 new_sport;
	__be16 new_dport;
	union {
		struct {
			__be32 orig_saddr;
			__be32 orig_daddr;
			__be32 new_saddr;
			__be32 new_daddr;
		} v4;
		struct {
			struct in6_addr orig_saddr;
			struct in6_addr orig_daddr;
		} v6;
	};
};

struct bcm63138_flow_offload {
	struct net_device *ndev;
	struct rhashtable flow_ht;
	struct list_head flow_list;
	struct list_head block_cb_list;
	struct mutex lock;

	struct notifier_block netdev_nb;
	u8 host_macs[BCM63138_MAX_HOST_MACS][ETH_ALEN];
	u32 host_mac_count;

	/* Telemetry counters */
	u32 active_flows;
	u64 total_added;
	u64 total_deleted;
	u64 rejected_non_sf2;
	u64 rejected_unsupported;
	u64 hw_insert_errors;

	struct dentry *dbg_dir;
};

static const struct rhashtable_params bcm63138_flow_ht_params = {
	.head_offset = offsetof(struct bcm63138_hw_flow, node),
	.key_offset = offsetof(struct bcm63138_hw_flow, cookie),
	.key_len = sizeof(unsigned long),
	.automatic_shrinking = true,
};

static bool bcm63138_is_sf2_dsa_dev(const struct net_device *conduit,
				    struct net_device *dev, u8 *out_port)
{
#if IS_ENABLED(CONFIG_NET_DSA)
	struct dsa_port *dp;

	if (!dev)
		return false;

	dp = dsa_port_from_netdev(dev);
	if (IS_ERR(dp))
		return false;

	if (dsa_port_to_conduit(dp) != conduit)
		return false;

	if (dp->index >= 8)
		return false;

	if (out_port)
		*out_port = (u8)dp->index;
	return true;
#else
	return false;
#endif
}

static bool bcm63138_mac_in_list(const u8 list[][ETH_ALEN], u32 count,
				 const u8 *mac)
{
	u32 i;

	for (i = 0; i < count; i++) {
		if (ether_addr_equal(list[i], mac))
			return true;
	}
	return false;
}

/* Synchronize active router unicast Ethernet MACs into IH DA filters 3..7
 * and Runner FW MAC DA filter slots 0..15 so Runner B classifies routed
 * packets destined to br-lan, wan, or VLAN subinterfaces into Flow Cache.
 * Must be called with offload->lock held; caller must hold RTNL or RCU when
 * walking init_net devices.
 */
static void bcm63138_sync_host_macs_locked(struct bcm63138_flow_offload *offload)
{
	u8 new_macs[BCM63138_MAX_HOST_MACS][ETH_ALEN];
	u8 zero_mac[ETH_ALEN] = {};
	struct net_device *d;
	u32 count = 0;
	u32 i;

	memset(new_macs, 0, sizeof(new_macs));

	rcu_read_lock();
	/* Pass 1: Prioritize SF2 DSA ports, conduit, and bridges/VLANs */
	for_each_netdev_rcu(&init_net, d) {
		if (!(d->flags & IFF_UP) || (d->flags & IFF_LOOPBACK))
			continue;
		if (d->type != ARPHRD_ETHER || d->addr_len != ETH_ALEN)
			continue;
		if (!is_valid_ether_addr(d->dev_addr))
			continue;
		if (!bcm63138_is_sf2_dsa_dev(offload->ndev, d, NULL) &&
		    d != offload->ndev &&
		    !netif_is_bridge_master(d) &&
		    !is_vlan_dev(d))
			continue;
		if (bcm63138_mac_in_list(new_macs, count, d->dev_addr))
			continue;
		if (count < BCM63138_MAX_HOST_MACS)
			ether_addr_copy(new_macs[count++], d->dev_addr);
	}

	/* Pass 2: Any remaining active unicast Ethernet interfaces (MACVLAN, etc.) */
	for_each_netdev_rcu(&init_net, d) {
		if (count >= BCM63138_MAX_HOST_MACS)
			break;
		if (!(d->flags & IFF_UP) || (d->flags & IFF_LOOPBACK))
			continue;
		if (d->type != ARPHRD_ETHER || d->addr_len != ETH_ALEN)
			continue;
		if (!is_valid_ether_addr(d->dev_addr))
			continue;
		if (bcm63138_mac_in_list(new_macs, count, d->dev_addr))
			continue;
		ether_addr_copy(new_macs[count++], d->dev_addr);
	}
	rcu_read_unlock();

	for (i = 0; i < BCM63138_IH_DA_FILT_COUNT; i++) {
		u8 filt_idx = BCM63138_IH_DA_FILT_BASE + i;

		if (i < count) {
			fi_bl_drv_ih_set_da_filter_without_mask(filt_idx,
								new_macs[i]);
			fi_bl_drv_ih_enable_da_filter(filt_idx, 1);
		} else {
			fi_bl_drv_ih_enable_da_filter(filt_idx, 0);
			fi_bl_drv_ih_set_da_filter_without_mask(filt_idx,
								zero_mac);
		}
	}

	for (i = 0; i < BCM63138_FW_DA_FILT_COUNT; i++) {
		u32 slot = BCM63138_IH_DA_FILT_COUNT + i;

		if (slot < count)
			rdd_fw_mac_da_filter_table_set(i, new_macs[slot]);
		else
			rdd_fw_mac_da_filter_table_set(i, zero_mac);
	}

	memcpy(offload->host_macs, new_macs, sizeof(offload->host_macs));
	offload->host_mac_count = count;
}

static int bcm63138_netdev_event(struct notifier_block *nb,
				 unsigned long event, void *ptr)
{
	struct bcm63138_flow_offload *offload =
		container_of(nb, struct bcm63138_flow_offload, netdev_nb);

	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGEADDR:
	case NETDEV_REGISTER:
	case NETDEV_UNREGISTER:
		mutex_lock(&offload->lock);
		bcm63138_sync_host_macs_locked(offload);
		mutex_unlock(&offload->lock);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static void bcm63138_mangle_masked32(void *hdr_base, u32 max_len,
				     const struct flow_action_entry *act)
{
	u32 cur;

	if (act->mangle.offset + sizeof(u32) > max_len)
		return;

	memcpy(&cur, (u8 *)hdr_base + act->mangle.offset, sizeof(u32));
	cur = (cur & act->mangle.mask) | (act->mangle.val & ~act->mangle.mask);
	memcpy((u8 *)hdr_base + act->mangle.offset, &cur, sizeof(u32));
}

static bool bcm63138_tuple_has_rx_pppoe(unsigned long cookie)
{
	const struct flow_offload_tuple *t =
		(const struct flow_offload_tuple *)cookie;
	u8 i;

	if (!t || t->encap_num > NF_FLOW_TABLE_ENCAP_MAX)
		return false;

	for (i = 0; i < t->encap_num; i++) {
		if (t->in_vlan_ingress & BIT(i))
			continue;
		if (t->encap[i].proto == htons(ETH_P_PPP_SES))
			return true;
	}
	return false;
}

static u16 bcm63138_calc_tx_mtu(unsigned long cookie,
				const struct net_device *odev,
				bool tx_pppoe)
{
	const struct flow_offload_tuple *t =
		(const struct flow_offload_tuple *)cookie;
	u32 mtu = READ_ONCE(odev->mtu);

	if (tx_pppoe && mtu > PPPOE_SES_HLEN)
		mtu -= PPPOE_SES_HLEN;

	if (t && t->mtu > 0 && t->mtu < mtu)
		mtu = t->mtu;

	if (t && t->xmit_type == FLOW_OFFLOAD_XMIT_DIRECT && t->out.ifidx) {
		struct net_device *upper;

		rcu_read_lock();
		upper = dev_get_by_index_rcu(&init_net, t->out.ifidx);
		if (upper) {
			u32 upper_mtu = READ_ONCE(upper->mtu);

			if (upper_mtu > 0 && upper_mtu < mtu)
				mtu = upper_mtu;
		}
		rcu_read_unlock();
	}

	if (mtu > 2047U)
		mtu = 2047U;
	return (u16)mtu;
}

static int bcm63138_flow_offload_replace(struct bcm63138_flow_offload *offload,
					 struct flow_cls_offload *cls)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	struct net_device *idev = NULL, *odev = NULL;
	struct bcm63138_flow_spec spec = {};
	struct ethhdr eth_hdr = {};
	struct flow_action_entry *act;
	struct bcm63138_hw_flow *entry;
	u8 cmdlist[BCM63138_CMDLIST_MAX_LEN];
	u8 cmdlist_len64 = 0;
	u8 in_sf2_port = 0, out_sf2_port = 0;
	u16 addr_type = 0;
	u16 tx_mtu;
	__be16 ports_pair[2] = {};
	rdpa_ip_flow_key_t lookup_key = {};
	BL_LILAC_RDD_ADD_CONNECTION_DTE rdd_conn = {};
	BL_LILAC_RDD_ERROR_DTE rdd_rc;
	int i, err;

	if (!rule)
		return -EINVAL;

	if (rhashtable_lookup_fast(&offload->flow_ht, &cls->cookie,
				   bcm63138_flow_ht_params))
		return -EEXIST;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		offload->rejected_unsupported++;
		return -EOPNOTSUPP;
	}

	{
		struct flow_match_meta match;

		flow_rule_match_meta(rule, &match);
		rcu_read_lock();
		idev = dev_get_by_index_rcu(&init_net,
					    match.key->ingress_ifindex);
		if (!bcm63138_is_sf2_dsa_dev(offload->ndev, idev, &in_sf2_port)) {
			rcu_read_unlock();
			offload->rejected_non_sf2++;
			return -EOPNOTSUPP;
		}
		rcu_read_unlock();
	}

	{
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
		if (flow_rule_has_control_flags(match.mask->flags,
						cls->common.extack)) {
			offload->rejected_unsupported++;
			return -EOPNOTSUPP;
		}
	}

	{
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		spec.ip_proto = match.key->ip_proto;
	}

	if (spec.ip_proto != IPPROTO_TCP && spec.ip_proto != IPPROTO_UDP) {
		offload->rejected_unsupported++;
		return -EOPNOTSUPP;
	}

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		offload->rejected_unsupported++;
		return -EOPNOTSUPP;
	}

	{
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);
		spec.orig_sport = match.key->src;
		spec.orig_dport = match.key->dst;
		ports_pair[0] = match.key->src;
		ports_pair[1] = match.key->dst;
	}

	switch (addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS: {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);
		spec.is_ipv6 = false;
		spec.v4.orig_saddr = match.key->src;
		spec.v4.orig_daddr = match.key->dst;
		spec.v4.new_saddr = match.key->src;
		spec.v4.new_daddr = match.key->dst;
		break;
	}
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS: {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(rule, &match);
		spec.is_ipv6 = true;
		spec.v6.orig_saddr = match.key->src;
		spec.v6.orig_daddr = match.key->dst;
		spec.v6.new_saddr = match.key->src;
		spec.v6.new_daddr = match.key->dst;
		break;
	}
	default:
		offload->rejected_unsupported++;
		return -EOPNOTSUPP;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		if (match.key->vlan_tpid != htons(ETH_P_8021Q)) {
			offload->rejected_unsupported++;
			return -EOPNOTSUPP;
		}
		spec.rx_vlan_num++;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_cvlan(rule, &match);
		if (match.key->vlan_tpid != htons(ETH_P_8021Q)) {
			offload->rejected_unsupported++;
			return -EOPNOTSUPP;
		}
		spec.rx_vlan_num++;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PPPOE) ||
	    bcm63138_tuple_has_rx_pppoe(cls->cookie))
		spec.rx_pppoe = true;

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				bcm63138_mangle_masked32(&eth_hdr,
							 ETH_ALEN * 2, act);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				if (spec.is_ipv6)
					return -EINVAL;
				if (act->mangle.offset == offsetof(struct iphdr, saddr)) {
					struct flow_action_entry sub = *act;

					sub.mangle.offset = 0;
					bcm63138_mangle_masked32(&spec.v4.new_saddr,
								 sizeof(__be32), &sub);
				} else if (act->mangle.offset == offsetof(struct iphdr, daddr)) {
					struct flow_action_entry sub = *act;

					sub.mangle.offset = 0;
					bcm63138_mangle_masked32(&spec.v4.new_daddr,
								 sizeof(__be32), &sub);
				} else {
					offload->rejected_unsupported++;
					return -EOPNOTSUPP;
				}
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP6:
				if (!spec.is_ipv6)
					return -EINVAL;
				if (act->mangle.offset >= offsetof(struct ipv6hdr, saddr) &&
				    act->mangle.offset + 4 <= offsetof(struct ipv6hdr, saddr) + 16) {
					struct flow_action_entry sub = *act;

					sub.mangle.offset -= offsetof(struct ipv6hdr, saddr);
					bcm63138_mangle_masked32(&spec.v6.new_saddr,
								 16, &sub);
				} else if (act->mangle.offset >= offsetof(struct ipv6hdr, daddr) &&
					   act->mangle.offset + 4 <= offsetof(struct ipv6hdr, daddr) + 16) {
					struct flow_action_entry sub = *act;

					sub.mangle.offset -= offsetof(struct ipv6hdr, daddr);
					bcm63138_mangle_masked32(&spec.v6.new_daddr,
								 16, &sub);
				} else {
					offload->rejected_unsupported++;
					return -EOPNOTSUPP;
				}
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
				if (act->mangle.offset + 4 <= sizeof(ports_pair))
					bcm63138_mangle_masked32(ports_pair,
								 sizeof(ports_pair), act);
				else {
					offload->rejected_unsupported++;
					return -EOPNOTSUPP;
				}
				break;
			default:
				offload->rejected_unsupported++;
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_REDIRECT:
			odev = act->dev;
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_PRIORITY:
			spec.traffic_class = (u8)(act->priority & 0x7U);
			break;
		case FLOW_ACTION_VLAN_POP:
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (spec.tx_vlan_num >= 2 ||
			    act->vlan.proto != htons(ETH_P_8021Q)) {
				offload->rejected_unsupported++;
				return -EOPNOTSUPP;
			}
			spec.tx_vlan_tci[spec.tx_vlan_num] =
				((u16)(act->vlan.prio & 0x7U) << VLAN_PRIO_SHIFT) |
				(act->vlan.vid & VLAN_VID_MASK);
			spec.tx_vlan_proto[spec.tx_vlan_num] =
				ntohs(act->vlan.proto);
			spec.tx_vlan_num++;
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			if (spec.tx_pppoe || spec.tx_vlan_num >= 2) {
				offload->rejected_unsupported++;
				return -EOPNOTSUPP;
			}
			spec.tx_pppoe = true;
			spec.tx_pppoe_sid = act->pppoe.sid;
			break;
		default:
			offload->rejected_unsupported++;
			return -EOPNOTSUPP;
		}
	}

	if (!bcm63138_is_sf2_dsa_dev(offload->ndev, odev, &out_sf2_port)) {
		offload->rejected_non_sf2++;
		return -EOPNOTSUPP;
	}

	spec.out_sf2_port = out_sf2_port;
	spec.new_sport = ports_pair[0];
	spec.new_dport = ports_pair[1];
	ether_addr_copy(spec.h_dest, eth_hdr.h_dest);
	ether_addr_copy(spec.h_source, eth_hdr.h_source);

	if (!is_valid_ether_addr(spec.h_dest) ||
	    !is_valid_ether_addr(spec.h_source)) {
		offload->rejected_unsupported++;
		return -EINVAL;
	}

	/* Ensure outgoing router MAC is also present in Host MAC DA filter */
	if (!bcm63138_mac_in_list(offload->host_macs, offload->host_mac_count,
				  spec.h_source))
		bcm63138_sync_host_macs_locked(offload);

	err = bcm63138_cmdlist_build_ucast(&spec, cmdlist, &cmdlist_len64);
	if (err) {
		offload->rejected_unsupported++;
		return err;
	}

	tx_mtu = bcm63138_calc_tx_mtu(cls->cookie, odev, spec.tx_pppoe);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	/* Populate Runner B UsConnectionTable 5-tuple + ingress SF2 port key */
	lookup_key.prot = spec.ip_proto;
	lookup_key.tcp_pure_ack = 0;
	lookup_key.lookup_port = BL_LILAC_RDD_LAN0_BRIDGE_PORT + in_sf2_port;
	lookup_key.src_port = ntohs(spec.orig_sport);
	lookup_key.dst_port = ntohs(spec.orig_dport);
	lookup_key.dir = rdpa_dir_us;

	if (spec.is_ipv6) {
		lookup_key.src_ip.family = bdmf_ip_family_ipv6;
		lookup_key.dst_ip.family = bdmf_ip_family_ipv6;
		memcpy(lookup_key.src_ip.addr.ipv6.data,
		       &spec.v6.orig_saddr, 16);
		memcpy(lookup_key.dst_ip.addr.ipv6.data,
		       &spec.v6.orig_daddr, 16);
	} else {
		lookup_key.src_ip.family = bdmf_ip_family_ipv4;
		lookup_key.dst_ip.family = bdmf_ip_family_ipv4;
		lookup_key.src_ip.addr.ipv4 = ntohl(spec.v4.orig_saddr);
		lookup_key.dst_ip.addr.ipv4 = ntohl(spec.v4.orig_daddr);
	}

	rdd_conn.lookup_entry = &lookup_key;
	rdd_conn.context_entry.fc_ucast_flow_context_entry.multicast_flag = 0;
	rdd_conn.context_entry.fc_ucast_flow_context_entry.is_routed = 1;
	rdd_conn.context_entry.fc_ucast_flow_context_entry.is_l2_accel = 0;
	rdd_conn.context_entry.fc_ucast_flow_context_entry.egress_phy =
		rdd_egress_phy_eth_lan;
	rdd_conn.context_entry.fc_ucast_flow_context_entry.connection_direction =
		rdpa_dir_us;
	rdd_conn.context_entry.fc_ucast_flow_context_entry.mtu = tx_mtu;
	rdd_conn.context_entry.fc_ucast_flow_context_entry.ip_addresses_table_index =
		RDD_FC_FLOW_IP_ADDRESSES_TABLE_SIZE;
	rdd_conn.context_entry.fc_ucast_flow_context_entry.command_list_length_64 =
		cmdlist_len64;
	memcpy(rdd_conn.context_entry.fc_ucast_flow_context_entry.command_list,
	       cmdlist, BCM63138_CMDLIST_MAX_LEN);

	rdd_conn.context_entry.fc_ucast_flow_context_eth_xtm_entry.egress_info = 0;
	rdd_conn.context_entry.fc_ucast_flow_context_eth_xtm_entry.egress_port =
		BL_LILAC_RDD_EMAC_ID_0 + out_sf2_port;
	rdd_conn.context_entry.fc_ucast_flow_context_eth_xtm_entry.traffic_class =
		spec.traffic_class & 0x7U;
	rdd_conn.context_entry.fc_ucast_flow_context_eth_xtm_entry.rate_controller = 0;

	rdd_rc = rdd_connection_entry_add(&rdd_conn, rdpa_dir_us);
	if (rdd_rc != BL_LILAC_RDD_OK) {
		offload->hw_insert_errors++;
		kfree(entry);
		return -ENOSPC;
	}

	entry->cookie = cls->cookie;
	entry->ctx_idx = rdd_conn.xo_entry_index;
	entry->last_used = jiffies;
	entry->in_sf2_port = in_sf2_port;
	entry->out_sf2_port = out_sf2_port;
	entry->ip_proto = spec.ip_proto;
	entry->is_ipv6 = spec.is_ipv6;
	entry->cmdlist_len64 = cmdlist_len64;
	entry->mtu = tx_mtu;
	entry->orig_sport = spec.orig_sport;
	entry->orig_dport = spec.orig_dport;
	entry->new_sport = spec.new_sport;
	entry->new_dport = spec.new_dport;
	if (spec.is_ipv6) {
		entry->v6.orig_saddr = spec.v6.orig_saddr;
		entry->v6.orig_daddr = spec.v6.orig_daddr;
	} else {
		entry->v4.orig_saddr = spec.v4.orig_saddr;
		entry->v4.orig_daddr = spec.v4.orig_daddr;
		entry->v4.new_saddr = spec.v4.new_saddr;
		entry->v4.new_daddr = spec.v4.new_daddr;
	}

	err = rhashtable_insert_fast(&offload->flow_ht, &entry->node,
				     bcm63138_flow_ht_params);
	if (err) {
		rdd_connection_entry_delete(entry->ctx_idx);
		kfree(entry);
		return err;
	}

	list_add_tail(&entry->list, &offload->flow_list);
	offload->active_flows++;
	offload->total_added++;
	return 0;
}

static void bcm63138_flow_sync_hw_stats(struct bcm63138_hw_flow *entry,
					u64 *out_delta_pkts,
					u64 *out_delta_bytes)
{
	RDD_CONTEXT_ENTRY_UNION_DTS ctx = {};
	u32 cur_pkts, cur_bytes;
	u32 d_pkts = 0, d_bytes = 0;

	if (rdd_context_entry_flwstat_get(entry->ctx_idx, &ctx) == BL_LILAC_RDD_OK) {
		cur_pkts = ctx.fc_ucast_flow_context_entry.flow_hits;
		cur_bytes = ctx.fc_ucast_flow_context_entry.flow_bytes;

		d_pkts = cur_pkts - entry->hw_pkts;
		d_bytes = cur_bytes - entry->hw_bytes;

		entry->hw_pkts = cur_pkts;
		entry->hw_bytes = cur_bytes;
		entry->total_pkts += d_pkts;
		entry->total_bytes += d_bytes;
		if (d_pkts > 0)
			entry->last_used = jiffies;
	}

	if (out_delta_pkts)
		*out_delta_pkts = d_pkts;
	if (out_delta_bytes)
		*out_delta_bytes = d_bytes;
}

static int bcm63138_flow_offload_stats(struct bcm63138_flow_offload *offload,
				       struct flow_cls_offload *cls)
{
	struct bcm63138_hw_flow *entry;
	u64 d_pkts = 0, d_bytes = 0;

	entry = rhashtable_lookup_fast(&offload->flow_ht, &cls->cookie,
				       bcm63138_flow_ht_params);
	if (!entry)
		return -ENOENT;

	bcm63138_flow_sync_hw_stats(entry, &d_pkts, &d_bytes);

	cls->stats.pkts += d_pkts;
	cls->stats.bytes += d_bytes;
	if (time_after(entry->last_used, (unsigned long)cls->stats.lastused))
		cls->stats.lastused = entry->last_used;

	return 0;
}

static int bcm63138_flow_offload_destroy(struct bcm63138_flow_offload *offload,
					 struct flow_cls_offload *cls)
{
	struct bcm63138_hw_flow *entry;

	entry = rhashtable_lookup_fast(&offload->flow_ht, &cls->cookie,
				       bcm63138_flow_ht_params);
	if (!entry)
		return -ENOENT;

	rdd_connection_entry_delete(entry->ctx_idx);
	rhashtable_remove_fast(&offload->flow_ht, &entry->node,
			       bcm63138_flow_ht_params);
	list_del(&entry->list);
	if (offload->active_flows > 0)
		offload->active_flows--;
	offload->total_deleted++;
	kfree(entry);

	return 0;
}

static int bcm63138_flow_offload_block_cb(enum tc_setup_type type,
					  void *type_data, void *cb_priv)
{
	struct bcm63138_flow_offload *offload = cb_priv;
	struct flow_cls_offload *cls = type_data;
	int err;

	if (!tc_can_offload(offload->ndev))
		return -EOPNOTSUPP;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	mutex_lock(&offload->lock);
	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		err = bcm63138_flow_offload_replace(offload, cls);
		break;
	case FLOW_CLS_DESTROY:
		err = bcm63138_flow_offload_destroy(offload, cls);
		break;
	case FLOW_CLS_STATS:
		err = bcm63138_flow_offload_stats(offload, cls);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&offload->lock);

	return err;
}

int bcm63138_flow_offload_setup_tc(struct bcm63138_flow_offload *offload,
				   enum tc_setup_type type, void *type_data)
{
	struct flow_block_offload *f = type_data;
	struct flow_block_cb *block_cb;

	if (!offload || type != TC_SETUP_FT)
		return -EOPNOTSUPP;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &offload->block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block,
						bcm63138_flow_offload_block_cb,
						offload);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		block_cb = flow_block_cb_alloc(bcm63138_flow_offload_block_cb,
					       offload, offload, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &offload->block_cb_list);
		return 0;

	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block,
						bcm63138_flow_offload_block_cb,
						offload);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

/* Indirect TC_SETUP_FT block binding for non-DSA interfaces (Wi-Fi APs, PPPoE,
 * VLANs) so OpenWrt firewall4's nft_try_hw_offload() keeps hardware offload
 * enabled on the shared flowtable when wireless/virtual interfaces are bridged
 * or routed alongside Starfighter 2 ports. Non-SF2 flows return -EOPNOTSUPP at
 * rule-install time and remain on nf_flow_table's software XMIT_DIRECT path.
 */
static int bcm63138_indr_noop_block_cb(enum tc_setup_type type,
				       void *type_data, void *cb_priv)
{
	return -EOPNOTSUPP;
}

static int bcm63138_indr_setup_tc_cb(struct net_device *netdev,
				     struct Qdisc *sch, void *cb_priv,
				     enum tc_setup_type type, void *type_data,
				     void *data,
				     void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct bcm63138_flow_offload *offload = cb_priv;
	struct flow_block_offload *f = type_data;
	struct flow_block_cb *block_cb;
	flow_setup_cb_t *cb;

	if (!netdev || type != TC_SETUP_FT)
		return -EOPNOTSUPP;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	/* If the flowtable only contains indirect devices (e.g. VLAN/PPPoE),
	 * attach the real hardware handler once; otherwise attach no-op so
	 * conduit's block_cb is not invoked twice per flow.
	 */
	if (flow_block_cb_lookup(f->block, bcm63138_flow_offload_block_cb,
				 offload))
		cb = bcm63138_indr_noop_block_cb;
	else
		cb = bcm63138_flow_offload_block_cb;

	f->driver_block_list = &offload->block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_indr_block_cb_alloc(cb, netdev, offload, NULL,
						    f, netdev, sch, data,
						    offload, cleanup);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &offload->block_cb_list);
		return 0;

	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block,
						bcm63138_indr_noop_block_cb,
						netdev);
		if (!block_cb)
			block_cb = flow_block_cb_lookup(f->block,
							bcm63138_flow_offload_block_cb,
							netdev);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			flow_indr_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int bcm63138_hwnat_debugfs_show(struct seq_file *s, void *unused)
{
	struct bcm63138_flow_offload *offload = s->private;
	struct bcm63138_hw_flow *e;
	u32 i;

	mutex_lock(&offload->lock);

	seq_printf(s, "BCM63138 Runner Hardware NAT Offload (TC_SETUP_FT)\n");
	seq_printf(s, "conduit: %s (hw_tc=%d)\n", offload->ndev->name,
		   !!tc_can_offload(offload->ndev));
	seq_printf(s, "active_flows: %u  added: %llu  deleted: %llu\n",
		   offload->active_flows, offload->total_added,
		   offload->total_deleted);
	seq_printf(s, "rejected_non_sf2: %llu  rejected_unsupported: %llu  hw_errors: %llu\n",
		   offload->rejected_non_sf2, offload->rejected_unsupported,
		   offload->hw_insert_errors);

	seq_printf(s, "host_macs (%u):\n", offload->host_mac_count);
	for (i = 0; i < offload->host_mac_count; i++) {
		seq_printf(s, "  [%2u] %pM (%s)\n", i, offload->host_macs[i],
			   (i < BCM63138_IH_DA_FILT_COUNT) ? "IH_HW" : "RNR_FW");
	}

	seq_puts(s, "\nactive_entries:\n");
	list_for_each_entry(e, &offload->flow_list, list) {
		bcm63138_flow_sync_hw_stats(e, NULL, NULL);
		if (e->is_ipv6) {
			seq_printf(s,
				   "  ctx=%-4u sf2=%u->%u %-3s [%pI6c]:%u -> [%pI6c]:%u mtu=%u cmd64=%u pkts=%llu bytes=%llu idle_ms=%u\n",
				   e->ctx_idx, e->in_sf2_port, e->out_sf2_port,
				   e->ip_proto == IPPROTO_TCP ? "tcp" : "udp",
				   &e->v6.orig_saddr, ntohs(e->orig_sport),
				   &e->v6.orig_daddr, ntohs(e->orig_dport),
				   e->mtu, e->cmdlist_len64,
				   e->total_pkts, e->total_bytes,
				   jiffies_to_msecs(jiffies - e->last_used));
		} else {
			seq_printf(s,
				   "  ctx=%-4u sf2=%u->%u %-3s %pI4:%u -> %pI4:%u (xlate %pI4:%u -> %pI4:%u) mtu=%u cmd64=%u pkts=%llu bytes=%llu idle_ms=%u\n",
				   e->ctx_idx, e->in_sf2_port, e->out_sf2_port,
				   e->ip_proto == IPPROTO_TCP ? "tcp" : "udp",
				   &e->v4.orig_saddr, ntohs(e->orig_sport),
				   &e->v4.orig_daddr, ntohs(e->orig_dport),
				   &e->v4.new_saddr, ntohs(e->new_sport),
				   &e->v4.new_daddr, ntohs(e->new_dport),
				   e->mtu, e->cmdlist_len64,
				   e->total_pkts, e->total_bytes,
				   jiffies_to_msecs(jiffies - e->last_used));
		}
	}

	mutex_unlock(&offload->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bcm63138_hwnat_debugfs);
#endif

struct bcm63138_flow_offload *bcm63138_flow_offload_init(struct net_device *ndev)
{
	struct bcm63138_flow_offload *offload;
	int err;

	offload = kzalloc(sizeof(*offload), GFP_KERNEL);
	if (!offload)
		return ERR_PTR(-ENOMEM);

	offload->ndev = ndev;
	mutex_init(&offload->lock);
	INIT_LIST_HEAD(&offload->flow_list);
	INIT_LIST_HEAD(&offload->block_cb_list);

	err = rhashtable_init(&offload->flow_ht, &bcm63138_flow_ht_params);
	if (err) {
		kfree(offload);
		return ERR_PTR(err);
	}

	offload->netdev_nb.notifier_call = bcm63138_netdev_event;
	err = register_netdevice_notifier(&offload->netdev_nb);
	if (err) {
		rhashtable_destroy(&offload->flow_ht);
		kfree(offload);
		return ERR_PTR(err);
	}

	err = flow_indr_dev_register(bcm63138_indr_setup_tc_cb, offload);
	if (err) {
		unregister_netdevice_notifier(&offload->netdev_nb);
		rhashtable_destroy(&offload->flow_ht);
		kfree(offload);
		return ERR_PTR(err);
	}

	mutex_lock(&offload->lock);
	bcm63138_sync_host_macs_locked(offload);
	mutex_unlock(&offload->lock);

#if IS_ENABLED(CONFIG_DEBUG_FS)
	offload->dbg_dir = debugfs_create_dir("bcm63138_rdp", NULL);
	if (!IS_ERR_OR_NULL(offload->dbg_dir))
		debugfs_create_file("hwnat", 0444, offload->dbg_dir, offload,
				    &bcm63138_hwnat_debugfs_fops);
#endif

	if (ndev->dev.parent)
		dev_info(ndev->dev.parent,
			 "Runner hardware NAT offload (TC_SETUP_FT) initialized\n");
	return offload;
}

void bcm63138_flow_offload_exit(struct bcm63138_flow_offload *offload)
{
	struct bcm63138_hw_flow *entry, *tmp;

	if (!offload)
		return;

#if IS_ENABLED(CONFIG_DEBUG_FS)
	debugfs_remove_recursive(offload->dbg_dir);
#endif

	flow_indr_dev_unregister(bcm63138_indr_setup_tc_cb, offload, NULL);
	unregister_netdevice_notifier(&offload->netdev_nb);

	mutex_lock(&offload->lock);
	list_for_each_entry_safe(entry, tmp, &offload->flow_list, list) {
		rdd_connection_entry_delete(entry->ctx_idx);
		rhashtable_remove_fast(&offload->flow_ht, &entry->node,
				       bcm63138_flow_ht_params);
		list_del(&entry->list);
		kfree(entry);
	}
	offload->active_flows = 0;
	mutex_unlock(&offload->lock);

	rhashtable_destroy(&offload->flow_ht);
	kfree(offload);
}
