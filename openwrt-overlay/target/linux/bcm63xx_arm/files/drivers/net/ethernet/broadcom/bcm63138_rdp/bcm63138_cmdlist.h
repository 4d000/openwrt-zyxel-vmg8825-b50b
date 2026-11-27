/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Clean-room Runner Generic Packet Engine (GPE) Command-List Builder
 * for Broadcom BCM63138 (DSL_63138 / LEGACY_RDP)
 */

#ifndef _BCM63138_CMDLIST_H
#define _BCM63138_CMDLIST_H

#include <linux/if_ether.h>
#include <linux/in6.h>
#include <linux/types.h>

#define BCM63138_CMDLIST_MAX_LEN	104
#define BCM63138_CTX_CMDLIST_SRAM_OFF	24
#define BCM63138_DDR_HEADROOM_SOP_OFF	58 /* 18B base + 40B extra headroom */

struct bcm63138_flow_spec {
	bool is_ipv6;
	u8 ip_proto;		/* IPPROTO_TCP, IPPROTO_UDP, IPPROTO_GRE */

	/* Egress Starfighter 2 port (0..7) and traffic class (0..7) */
	u8 out_sf2_port;
	u8 traffic_class;

	/* Outgoing Ethernet MAC addresses */
	u8 h_dest[ETH_ALEN];
	u8 h_source[ETH_ALEN];

	/* Ingress encapsulation state */
	u8 rx_vlan_num;		/* 0, 1, or 2 802.1Q tags on wire */
	bool rx_pppoe;		/* true if ingress frame carried PPPoE session */

	/* Egress encapsulation state */
	u8 tx_vlan_num;		/* 0, 1, or 2 802.1Q tags on wire */
	u16 tx_vlan_tci[2];	/* host byte order TCI (PCP|DEI|VID) */
	u16 tx_vlan_proto[2];	/* host byte order TPID (e.g. ETH_P_8021Q) */
	bool tx_pppoe;
	u16 tx_pppoe_sid;	/* host byte order PPPoE session ID */

	/* L3 addresses (big-endian network byte order) */
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
			struct in6_addr new_saddr;
			struct in6_addr new_daddr;
		} v6;
	};

	/* L4 ports (big-endian network byte order) */
	__be16 orig_sport;
	__be16 orig_dport;
	__be16 new_sport;
	__be16 new_dport;
};

int bcm63138_cmdlist_build_ucast(const struct bcm63138_flow_spec *spec,
				 u8 out_cmdlist[BCM63138_CMDLIST_MAX_LEN],
				 u8 *out_len64);

#endif /* _BCM63138_CMDLIST_H */
