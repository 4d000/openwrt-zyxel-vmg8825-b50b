/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Linux Netfilter Flowtable Hardware Offload (TC_SETUP_FT) for
 * Broadcom BCM63138 Runner/RDP DSA Conduit
 */

#ifndef _BCM63138_FLOW_OFFLOAD_H
#define _BCM63138_FLOW_OFFLOAD_H

#include <linux/netdevice.h>
#include <net/pkt_cls.h>

struct bcm63138_flow_offload;

struct bcm63138_flow_offload *bcm63138_flow_offload_init(struct net_device *ndev);
void bcm63138_flow_offload_exit(struct bcm63138_flow_offload *offload);
int bcm63138_flow_offload_setup_tc(struct bcm63138_flow_offload *offload,
				   enum tc_setup_type type, void *type_data);

#endif /* _BCM63138_FLOW_OFFLOAD_H */
