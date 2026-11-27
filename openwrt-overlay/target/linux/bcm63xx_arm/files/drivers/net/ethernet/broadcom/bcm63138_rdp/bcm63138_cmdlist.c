// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean-room Runner Generic Packet Engine (GPE) Command-List Builder
 * for Broadcom BCM63138 (DSL_63138 / LEGACY_RDP)
 *
 * Encodes 32-bit big-endian GPE instructions into the 104-byte command_list[]
 * embedded at byte offset +24 of RDD_FC_UCAST_FLOW_CONTEXT_ENTRY_DTS.
 */

#include <linux/byteorder/generic.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/if_pppox.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kernel.h>
#include <linux/ppp_defs.h>
#include <linux/string.h>
#include <linux/tcp.h>
#include <linux/udp.h>

#include "bcm63138_cmdlist.h"

/* BCM63138 LEGACY_RDP GPE 5-bit opcodes */
enum bcm63138_gpe_opcode {
	GPE_OP_END			= 0,
	GPE_OP_SOP_PUSH_REPLACE		= 1,
	GPE_OP_SOP_PULL_REPLACE		= 5,
	GPE_OP_REPLACE_16		= 12,
	GPE_OP_REPLACE_32		= 13,
	GPE_OP_REPLACE_BITS_16		= 14,
	GPE_OP_COPY_ADD_16_SRAM		= 15,
	GPE_OP_COPY_ADD_16_DDR		= 16,
	GPE_OP_COPY_BITS_16		= 18,
	GPE_OP_INSERT_16		= 19,
	GPE_OP_DELETE_16		= 20,
	GPE_OP_DECREMENT_8		= 21,
	GPE_OP_APPLY_ICSUM_16		= 22,
	GPE_OP_APPLY_ICSUM_NZ_16	= 23,
};

#define GPE_SOP_SRAM_IMM_FLAG		0x0100U

struct bcm63138_gpe_ctx {
	u8 buf[BCM63138_CMDLIST_MAX_LEN];
	u32 sram_base_off;	/* 24 inside 128B context entry */
	u32 byte_len;		/* bytes written so far (0..104) */
	s32 ddr_sop_pos;	/* starts at headroom_sop_off - 2 = 56 */
};

static void gpe_init(struct bcm63138_gpe_ctx *ctx)
{
	memset(ctx->buf, 0xff, sizeof(ctx->buf));
	ctx->sram_base_off = BCM63138_CTX_CMDLIST_SRAM_OFF;
	ctx->byte_len = 0;
	ctx->ddr_sop_pos = BCM63138_DDR_HEADROOM_SOP_OFF - 2;
}

static inline u32 gpe_make_hdr(u8 opcode, u8 arg0, u8 arg1, u16 imm16)
{
	return (((u32)opcode & 0x1fU) << 27) |
	       (((u32)arg0 & 0x3fU) << 21) |
	       (((u32)arg1 & 0x1fU) << 16) |
	       (u32)imm16;
}

static int gpe_check_space(const struct bcm63138_gpe_ctx *ctx, u32 cmd_bytes,
			   u32 arg0, u32 arg1)
{
	/* Leave at least 4 bytes for the terminating GPE_OP_END word */
	if (arg0 > 63 || arg1 > 31)
		return -EINVAL;
	if (ctx->byte_len + cmd_bytes > BCM63138_CMDLIST_MAX_LEN)
		return -ENOSPC;
	return 0;
}

static void gpe_write_be32(u8 *dst, u32 val)
{
	__be32 be = cpu_to_be32(val);

	memcpy(dst, &be, sizeof(be));
}

static void gpe_write_be16(u8 *dst, u16 val)
{
	__be16 be = cpu_to_be16(val);

	memcpy(dst, &be, sizeof(be));
}

static int gpe_generic_cmd(struct bcm63138_gpe_ctx *ctx, u8 opcode,
			   u8 arg0, u8 arg1, const u16 *words, u32 n_u16,
			   s32 target_off)
{
	u32 cmd_bytes = (n_u16 <= 1) ? 4 : 8;
	u32 write_off;
	u32 i;
	int err;

	if (n_u16 == 0 || n_u16 > 3)
		return -EINVAL;

	if (target_off < 0) {
		err = gpe_check_space(ctx, cmd_bytes, arg0, arg1);
		if (err)
			return err;
		write_off = ctx->byte_len;
		ctx->byte_len += cmd_bytes;
	} else {
		if ((u32)target_off + cmd_bytes > ctx->byte_len)
			return -EINVAL;
		write_off = (u32)target_off;
	}

	gpe_write_be32(&ctx->buf[write_off],
		       gpe_make_hdr(opcode, arg0, arg1, words[0]));

	for (i = 1; i < n_u16; i++)
		gpe_write_be16(&ctx->buf[write_off + 4 + (i - 1) * 2], words[i]);

	return 0;
}

static int gpe_generic_data_cmd_16(struct bcm63138_gpe_ctx *ctx, u8 opcode,
				   u8 arg0, u32 n_u16, const u16 *data_host,
				   const u16 *imm16_override,
				   u32 *out_data_sram_off)
{
	u32 extra_u16 = imm16_override ? (n_u16 + 1) : n_u16;
	u32 cmd_bytes = 4 + ((extra_u16 >> 1) << 2);
	u32 arg1 = imm16_override ? 0 : n_u16;
	u32 write_off, i;
	u16 imm16 = 0;
	int err;

	err = gpe_check_space(ctx, cmd_bytes, arg0, arg1);
	if (err)
		return err;

	write_off = ctx->byte_len;
	if (out_data_sram_off)
		*out_data_sram_off = write_off + (imm16_override ? 4 : 2);

	ctx->byte_len += cmd_bytes;

	if (imm16_override) {
		imm16 = *imm16_override;
		gpe_write_be32(&ctx->buf[write_off],
			       gpe_make_hdr(opcode, arg0, 0, imm16));
		for (i = 0; i < n_u16; i++)
			gpe_write_be16(&ctx->buf[write_off + 4 + i * 2],
				       data_host[i]);
	} else {
		if (n_u16 > 0)
			imm16 = data_host[0];
		gpe_write_be32(&ctx->buf[write_off],
			       gpe_make_hdr(opcode, arg0, (u8)n_u16, imm16));
		for (i = 1; i < n_u16; i++)
			gpe_write_be16(&ctx->buf[write_off + 4 + (i - 1) * 2],
				       data_host[i]);
	}

	/* Trailing 2-byte pad (when total halfwords are odd) remains 0xffff */
	return 0;
}

static int gpe_generic_data_cmd_32(struct bcm63138_gpe_ctx *ctx, u8 opcode,
				   u8 arg0, u32 n_u32, const u32 *data_host,
				   u32 *out_data_sram_off)
{
	u32 cmd_bytes = 4 + (n_u32 << 2);
	u32 write_off, i;
	int err;

	err = gpe_check_space(ctx, cmd_bytes, arg0, n_u32);
	if (err)
		return err;

	if (out_data_sram_off)
		*out_data_sram_off = ctx->byte_len + 4;

	write_off = ctx->byte_len;
	ctx->byte_len += cmd_bytes;

	gpe_write_be32(&ctx->buf[write_off],
		       gpe_make_hdr(opcode, arg0, (u8)n_u32, 0));

	for (i = 0; i < n_u32; i++)
		gpe_write_be32(&ctx->buf[write_off + 4 + i * 4], data_host[i]);

	return 0;
}

static int gpe_cmd_sop_push_replace_sram(struct bcm63138_gpe_ctx *ctx,
					 u32 push_bytes, u32 replace_bytes,
					 const u16 *data_host,
					 u32 *out_data_sram_off)
{
	u16 imm16;

	if ((push_bytes & 1) || (replace_bytes & 1) ||
	    replace_bytes == 0 || replace_bytes > 0xfeU)
		return -EINVAL;
	if ((s32)push_bytes > ctx->ddr_sop_pos)
		return -ENOSPC;

	ctx->ddr_sop_pos -= (s32)push_bytes;
	imm16 = GPE_SOP_SRAM_IMM_FLAG | (u16)(replace_bytes & 0xffU);

	return gpe_generic_data_cmd_16(ctx, GPE_OP_SOP_PUSH_REPLACE,
				       (u8)(push_bytes >> 1),
				       replace_bytes >> 1, data_host,
				       &imm16, out_data_sram_off);
}

static int gpe_cmd_sop_pull_replace_sram(struct bcm63138_gpe_ctx *ctx,
					 u32 pull_bytes, u32 replace_bytes,
					 const u16 *data_host,
					 u32 *out_data_sram_off)
{
	u16 imm16;

	if ((pull_bytes & 1) || (replace_bytes & 1) ||
	    replace_bytes == 0 || replace_bytes > 0xfeU)
		return -EINVAL;

	ctx->ddr_sop_pos += (s32)pull_bytes;
	imm16 = GPE_SOP_SRAM_IMM_FLAG | (u16)(replace_bytes & 0xffU);

	return gpe_generic_data_cmd_16(ctx, GPE_OP_SOP_PULL_REPLACE,
				       (u8)(pull_bytes >> 1),
				       replace_bytes >> 1, data_host,
				       &imm16, out_data_sram_off);
}

static int gpe_cmd_replace_16(struct bcm63138_gpe_ctx *ctx, u32 ddr_byte_off,
			      u32 n_u16, const u16 *data_host,
			      u32 *out_data_sram_off)
{
	if (ddr_byte_off & 1)
		return -EINVAL;
	return gpe_generic_data_cmd_16(ctx, GPE_OP_REPLACE_16,
				       (u8)(ddr_byte_off >> 1), n_u16,
				       data_host, NULL, out_data_sram_off);
}

static int gpe_cmd_replace_32(struct bcm63138_gpe_ctx *ctx, u32 ddr_byte_off,
			      u32 n_u32, const u32 *data_host,
			      u32 *out_data_sram_off)
{
	if (ddr_byte_off & 1)
		return -EINVAL;
	return gpe_generic_data_cmd_32(ctx, GPE_OP_REPLACE_32,
				       (u8)(ddr_byte_off >> 1), n_u32,
				       data_host, out_data_sram_off);
}

static int gpe_cmd_copy_add_16(struct bcm63138_gpe_ctx *ctx, u32 dst_byte_off,
			       u32 src_byte_off, s8 add_val, s32 target_off,
			       bool dst_is_sram)
{
	u8 opcode = dst_is_sram ? GPE_OP_COPY_ADD_16_SRAM : GPE_OP_COPY_ADD_16_DDR;
	u16 word;

	if ((dst_byte_off & 1) || (src_byte_off > 0xffU))
		return -EINVAL;

	word = (((u16)(u8)add_val) << 8) | ((u16)src_byte_off & 0xffU);
	return gpe_generic_cmd(ctx, opcode, (u8)(dst_byte_off >> 1), 0,
			       &word, 1, target_off);
}

static int gpe_cmd_decrement_8(struct bcm63138_gpe_ctx *ctx, u32 ddr_byte_off)
{
	u16 word = (u16)(ddr_byte_off & 0xffU);

	if (ddr_byte_off > 0xffU)
		return -EINVAL;
	return gpe_generic_cmd(ctx, GPE_OP_DECREMENT_8, 0, 0, &word, 1, -1);
}

static int gpe_cmd_apply_icsum_16(struct bcm63138_gpe_ctx *ctx,
				  u32 ddr_byte_off, u16 icsum_delta)
{
	if (ddr_byte_off & 1)
		return -EINVAL;
	return gpe_generic_cmd(ctx, GPE_OP_APPLY_ICSUM_16,
			       (u8)(ddr_byte_off >> 1), 0, &icsum_delta, 1, -1);
}

static int gpe_cmd_apply_icsum_nz_16(struct bcm63138_gpe_ctx *ctx,
				     u32 ddr_byte_off, u16 icsum_delta)
{
	if (ddr_byte_off & 1)
		return -EINVAL;
	return gpe_generic_cmd(ctx, GPE_OP_APPLY_ICSUM_NZ_16,
			       (u8)(ddr_byte_off >> 1), 0, &icsum_delta, 1, -1);
}

static int gpe_cmd_end(struct bcm63138_gpe_ctx *ctx,
		       u8 out_cmdlist[BCM63138_CMDLIST_MAX_LEN],
		       u8 *out_len64)
{
	if (ctx->byte_len + 4 > BCM63138_CMDLIST_MAX_LEN)
		return -ENOSPC;

	gpe_write_be32(&ctx->buf[ctx->byte_len],
		       gpe_make_hdr(GPE_OP_END, 0, 0, 0));
	ctx->byte_len += 4;

	memcpy(out_cmdlist, ctx->buf, BCM63138_CMDLIST_MAX_LEN);
	*out_len64 = (u8)((ctx->byte_len + 7U) >> 3);
	return 0;
}

/* One's-complement 16-bit incremental checksum helpers (RFC 1624) */
static inline u16 gpe_csum16_add(u16 a, u16 b)
{
	u32 sum = (u32)a + (u32)b;

	sum = (sum & 0xffffU) + (sum >> 16);
	return (u16)((sum & 0xffffU) + (sum >> 16));
}

static inline u16 gpe_csum16_delta16(u16 old_h, u16 new_h)
{
	if (old_h == new_h)
		return 0;
	return gpe_csum16_add(old_h, (u16)~new_h);
}

static inline u16 gpe_csum16_delta32(u32 old_h, u32 new_h)
{
	if (old_h == new_h)
		return 0;
	return gpe_csum16_add(gpe_csum16_delta16((u16)(old_h >> 16),
						 (u16)(new_h >> 16)),
			      gpe_csum16_delta16((u16)old_h, (u16)new_h));
}

static u16 gpe_csum16_delta_ipv6(const struct in6_addr *old_a,
				 const struct in6_addr *new_a)
{
	u16 acc = 0;
	int i;

	for (i = 0; i < 4; i++) {
		u32 o = be32_to_cpu(old_a->s6_addr32[i]);
		u32 n = be32_to_cpu(new_a->s6_addr32[i]);

		acc = gpe_csum16_add(acc, gpe_csum16_delta32(o, n));
	}
	return acc;
}

static int bcm63138_emit_l2(struct bcm63138_gpe_ctx *ctx,
			    const struct bcm63138_flow_spec *spec,
			    u32 *out_l3_off)
{
	u8 raw[48];
	u16 words16[24];
	u32 brcm_tag;
	u32 rx_l2_len;
	u32 tx_l2_len;
	u32 rx_l3_pre_sop;
	u32 pos = 0;
	u32 pppoe_hdr_pos = 0;
	s32 copy_slot_off = -1;
	u32 l2_sram_off = 0;
	s32 sop_delta;
	u16 inner_ethertype = spec->is_ipv6 ? ETH_P_IPV6 : ETH_P_IP;
	u32 i, n_u16;
	int err;

	if (spec->out_sf2_port >= 8 || spec->rx_vlan_num > 2 ||
	    spec->tx_vlan_num > 2)
		return -EINVAL;

	/* Broadcom Type-2 4-byte egress switch tag:
	 * [31:29]=001 (OP=unicast to portmap), [28:26]=TC, [7:0]=BIT(port)
	 */
	brcm_tag = 0x20000000U |
		   (((u32)(spec->traffic_class & 0x7U)) << 26) |
		   (1U << spec->out_sf2_port);

	/* Ingress SF2 frames in DDR retain their 4-byte Broadcom switch tag
	 * at [DA(6) | SA(6) | BRCM_TAG(4) | ...], so RX L2 length in DDR is
	 * ETH_HLEN + 4 (+ optional 802.1Q VLANs and PPPoE session header).
	 */
	rx_l2_len = ETH_HLEN + 4 + (u32)spec->rx_vlan_num * VLAN_HLEN +
		    (spec->rx_pppoe ? PPPOE_SES_HLEN : 0);
	rx_l3_pre_sop = 2 + rx_l2_len;

	/* 2-byte Runner internal SOP prefix left untouched via 0xffff */
	raw[pos++] = 0xff;
	raw[pos++] = 0xff;

	memcpy(&raw[pos], spec->h_dest, ETH_ALEN);
	pos += ETH_ALEN;
	memcpy(&raw[pos], spec->h_source, ETH_ALEN);
	pos += ETH_ALEN;

	gpe_write_be32(&raw[pos], brcm_tag);
	pos += 4;

	for (i = 0; i < spec->tx_vlan_num; i++) {
		u16 tpid = spec->tx_vlan_proto[i] ?
			   spec->tx_vlan_proto[i] : ETH_P_8021Q;

		gpe_write_be16(&raw[pos], tpid);
		pos += 2;
		gpe_write_be16(&raw[pos], spec->tx_vlan_tci[i]);
		pos += 2;
	}

	if (spec->tx_pppoe) {
		/* Reserve DDR->SRAM copy_add_16 slot before SOP shift */
		copy_slot_off = (s32)ctx->byte_len;
		err = gpe_cmd_copy_add_16(ctx, 0, 0, 0, -1, false);
		if (err)
			return err;

		gpe_write_be16(&raw[pos], ETH_P_PPP_SES);
		pos += 2;

		pppoe_hdr_pos = pos;
		raw[pos++] = 0x11; /* Ver=1, Type=1 */
		raw[pos++] = 0x00; /* Code=0 (Session Data) */
		gpe_write_be16(&raw[pos], spec->tx_pppoe_sid);
		pos += 2;
		gpe_write_be16(&raw[pos], 0x0000); /* Patched by GPE */
		pos += 2;

		gpe_write_be16(&raw[pos],
			       spec->is_ipv6 ? PPP_IPV6 : PPP_IP);
		pos += 2;
	} else {
		gpe_write_be16(&raw[pos], inner_ethertype);
		pos += 2;
	}

	tx_l2_len = pos - 2;
	sop_delta = (s32)rx_l2_len - (s32)tx_l2_len;

	n_u16 = pos >> 1;
	for (i = 0; i < n_u16; i++) {
		__be16 be;

		memcpy(&be, &raw[i * 2], sizeof(be));
		words16[i] = be16_to_cpu(be);
	}

	if (sop_delta < 0) {
		err = gpe_cmd_sop_push_replace_sram(ctx, (u32)(-sop_delta),
						    pos, words16, &l2_sram_off);
	} else {
		err = gpe_cmd_sop_pull_replace_sram(ctx, (u32)sop_delta,
						    pos, words16, &l2_sram_off);
	}
	if (err)
		return err;

	if (spec->tx_pppoe) {
		/* Patch reserved copy_add_16 to copy IP length from pre-shift
		 * DDR into the SRAM PPPoE length halfword before SOP replace:
		 *  - IPv4: tot_len (rx_l3 + 2) + 2 bytes PPP proto
		 *  - IPv6: payload_len (rx_l3 + 4) + 42 bytes (40B hdr + 2B PPP)
		 * Note: l2_sram_off points to words16[0] (raw[0..1]), so raw[k]
		 * lives at l2_sram_off + k.
		 */
		u32 pppoe_len_raw_idx = pppoe_hdr_pos + 4;
		u32 dst_sram_off = l2_sram_off + pppoe_len_raw_idx;
		u32 src_ddr_off = spec->is_ipv6 ?
				  (rx_l3_pre_sop + 4) :
				  (rx_l3_pre_sop + 2);
		s8 add_val = spec->is_ipv6 ? 42 : 2;

		err = gpe_cmd_copy_add_16(ctx, dst_sram_off, src_ddr_off,
					  add_val, copy_slot_off, true);
		if (err)
			return err;
	}

	/* SRAM inline SOP_PUSH_REPLACE / SOP_PULL_REPLACE advances Runner GPE's
	 * working packet pointer past the replaced L2 header (by `pos` bytes),
	 * so all subsequent L3/L4 commands address starting at L3 offset 0.
	 */
	*out_l3_off = 0;
	return 0;
}

static int bcm63138_emit_l4_ports_and_csum(struct bcm63138_gpe_ctx *ctx,
					   const struct bcm63138_flow_spec *spec,
					   u32 l4_off, u16 addr_icsum)
{
	u16 orig_sp = be16_to_cpu(spec->orig_sport);
	u16 orig_dp = be16_to_cpu(spec->orig_dport);
	u16 new_sp = be16_to_cpu(spec->new_sport);
	u16 new_dp = be16_to_cpu(spec->new_dport);
	bool sp_changed = (orig_sp != new_sp);
	bool dp_changed = (orig_dp != new_dp);
	u16 l4_icsum = addr_icsum;
	int err;

	if (spec->ip_proto != IPPROTO_TCP && spec->ip_proto != IPPROTO_UDP)
		return 0;

	if (sp_changed && dp_changed) {
		u32 ports32 = ((u32)new_sp << 16) | (u32)new_dp;

		err = gpe_cmd_replace_32(ctx, l4_off, 1, &ports32, NULL);
		if (err)
			return err;
	} else if (sp_changed) {
		err = gpe_cmd_replace_16(ctx, l4_off, 1, &new_sp, NULL);
		if (err)
			return err;
	} else if (dp_changed) {
		err = gpe_cmd_replace_16(ctx, l4_off + 2, 1, &new_dp, NULL);
		if (err)
			return err;
	}

	l4_icsum = gpe_csum16_add(l4_icsum, gpe_csum16_delta16(orig_sp, new_sp));
	l4_icsum = gpe_csum16_add(l4_icsum, gpe_csum16_delta16(orig_dp, new_dp));

	if (l4_icsum != 0) {
		if (spec->ip_proto == IPPROTO_TCP) {
			err = gpe_cmd_apply_icsum_16(ctx,
						     l4_off + offsetof(struct tcphdr, check),
						     l4_icsum);
		} else {
			err = gpe_cmd_apply_icsum_nz_16(ctx,
							l4_off + offsetof(struct udphdr, check),
							l4_icsum);
		}
		if (err)
			return err;
	}

	return 0;
}

static int bcm63138_emit_ipv4(struct bcm63138_gpe_ctx *ctx,
			      const struct bcm63138_flow_spec *spec,
			      u32 l3_off)
{
	u32 orig_sa = be32_to_cpu(spec->v4.orig_saddr);
	u32 orig_da = be32_to_cpu(spec->v4.orig_daddr);
	u32 new_sa = be32_to_cpu(spec->v4.new_saddr);
	u32 new_da = be32_to_cpu(spec->v4.new_daddr);
	bool sa_changed = (orig_sa != new_sa);
	bool da_changed = (orig_da != new_da);
	u16 addr_icsum = 0;
	u16 ip_icsum = 0x0100U; /* TTL decrement (-1 in upper byte of TTL|Proto) */
	int err;

	/* Decrement IPv4 TTL at l3_off + 8 */
	err = gpe_cmd_decrement_8(ctx, l3_off + offsetof(struct iphdr, ttl));
	if (err)
		return err;

	if (sa_changed && da_changed) {
		u32 addrs[2] = { new_sa, new_da };

		err = gpe_cmd_replace_32(ctx,
					 l3_off + offsetof(struct iphdr, saddr),
					 2, addrs, NULL);
		if (err)
			return err;
	} else if (sa_changed) {
		err = gpe_cmd_replace_32(ctx,
					 l3_off + offsetof(struct iphdr, saddr),
					 1, &new_sa, NULL);
		if (err)
			return err;
	} else if (da_changed) {
		err = gpe_cmd_replace_32(ctx,
					 l3_off + offsetof(struct iphdr, daddr),
					 1, &new_da, NULL);
		if (err)
			return err;
	}

	addr_icsum = gpe_csum16_add(gpe_csum16_delta32(orig_sa, new_sa),
				    gpe_csum16_delta32(orig_da, new_da));
	ip_icsum = gpe_csum16_add(ip_icsum, addr_icsum);

	err = gpe_cmd_apply_icsum_16(ctx,
				     l3_off + offsetof(struct iphdr, check),
				     ip_icsum);
	if (err)
		return err;

	return bcm63138_emit_l4_ports_and_csum(ctx, spec,
					       l3_off + sizeof(struct iphdr),
					       addr_icsum);
}

static int bcm63138_emit_ipv6(struct bcm63138_gpe_ctx *ctx,
			      const struct bcm63138_flow_spec *spec,
			      u32 l3_off)
{
	bool sa_changed = memcmp(&spec->v6.orig_saddr, &spec->v6.new_saddr,
				 sizeof(struct in6_addr)) != 0;
	bool da_changed = memcmp(&spec->v6.orig_daddr, &spec->v6.new_daddr,
				 sizeof(struct in6_addr)) != 0;
	u16 addr_icsum = 0;
	int err, i;

	/* Decrement IPv6 Hop Limit at l3_off + 7 */
	err = gpe_cmd_decrement_8(ctx,
				  l3_off + offsetof(struct ipv6hdr, hop_limit));
	if (err)
		return err;

	if (sa_changed) {
		u32 words[4];

		for (i = 0; i < 4; i++)
			words[i] = be32_to_cpu(spec->v6.new_saddr.s6_addr32[i]);
		err = gpe_cmd_replace_32(ctx,
					 l3_off + offsetof(struct ipv6hdr, saddr),
					 4, words, NULL);
		if (err)
			return err;
		addr_icsum = gpe_csum16_add(addr_icsum,
					    gpe_csum16_delta_ipv6(&spec->v6.orig_saddr,
								  &spec->v6.new_saddr));
	}

	if (da_changed) {
		u32 words[4];

		for (i = 0; i < 4; i++)
			words[i] = be32_to_cpu(spec->v6.new_daddr.s6_addr32[i]);
		err = gpe_cmd_replace_32(ctx,
					 l3_off + offsetof(struct ipv6hdr, daddr),
					 4, words, NULL);
		if (err)
			return err;
		addr_icsum = gpe_csum16_add(addr_icsum,
					    gpe_csum16_delta_ipv6(&spec->v6.orig_daddr,
								  &spec->v6.new_daddr));
	}

	return bcm63138_emit_l4_ports_and_csum(ctx, spec,
					       l3_off + sizeof(struct ipv6hdr),
					       addr_icsum);
}

int bcm63138_cmdlist_build_ucast(const struct bcm63138_flow_spec *spec,
				 u8 out_cmdlist[BCM63138_CMDLIST_MAX_LEN],
				 u8 *out_len64)
{
	struct bcm63138_gpe_ctx ctx;
	u32 l3_off = 0;
	int err;

	if (!spec || !out_cmdlist || !out_len64)
		return -EINVAL;

	if (spec->ip_proto != IPPROTO_TCP &&
	    spec->ip_proto != IPPROTO_UDP &&
	    spec->ip_proto != IPPROTO_GRE)
		return -EOPNOTSUPP;

	gpe_init(&ctx);

	err = bcm63138_emit_l2(&ctx, spec, &l3_off);
	if (err)
		return err;

	if (spec->is_ipv6)
		err = bcm63138_emit_ipv6(&ctx, spec, l3_off);
	else
		err = bcm63138_emit_ipv4(&ctx, spec, l3_off);
	if (err)
		return err;

	return gpe_cmd_end(&ctx, out_cmdlist, out_len64);
}
