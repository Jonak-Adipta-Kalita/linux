/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/mlx5/fs.h>
#include "en.h"
#include "en/params.h"
#include "en/xsk/pool.h"
#include "en/fs_ethtool.h"

struct mlx5e_ethtool_table {
	struct mlx5_flow_table *ft;
	int                    num_rules;
};

#define ETHTOOL_NUM_L3_L4_FTS 7
#define ETHTOOL_NUM_L2_FTS 4

struct mlx5e_ethtool_steering {
	struct mlx5e_ethtool_table      l3_l4_ft[ETHTOOL_NUM_L3_L4_FTS];
	struct mlx5e_ethtool_table      l2_ft[ETHTOOL_NUM_L2_FTS];
	struct list_head                rules;
	int                             tot_num_rules;
};

static int flow_type_to_traffic_type(u32 flow_type);

static u32 flow_type_mask(u32 flow_type)
{
	return flow_type & ~(FLOW_EXT | FLOW_MAC_EXT | FLOW_RSS);
}

struct mlx5e_ethtool_rule {
	struct list_head             list;
	struct ethtool_rx_flow_spec  flow_spec;
	struct mlx5_flow_handle	     *rule;
	struct mlx5e_ethtool_table   *eth_ft;
	struct mlx5e_rss             *rss;
};

static void put_flow_table(struct mlx5e_ethtool_table *eth_ft)
{
	if (!--eth_ft->num_rules) {
		mlx5_destroy_flow_table(eth_ft->ft);
		eth_ft->ft = NULL;
	}
}

#define MLX5E_ETHTOOL_L3_L4_PRIO 0
#define MLX5E_ETHTOOL_L2_PRIO (MLX5E_ETHTOOL_L3_L4_PRIO + ETHTOOL_NUM_L3_L4_FTS)
#define MLX5E_ETHTOOL_NUM_ENTRIES 64000
#define MLX5E_ETHTOOL_NUM_GROUPS  10
static struct mlx5e_ethtool_table *get_flow_table(struct mlx5e_priv *priv,
						  struct ethtool_rx_flow_spec *fs,
						  int num_tuples)
{
	struct mlx5e_ethtool_steering *ethtool = mlx5e_fs_get_ethtool(priv->fs);
	struct mlx5_flow_table_attr ft_attr = {};
	struct mlx5e_ethtool_table *eth_ft;
	struct mlx5_flow_namespace *ns;
	struct mlx5_flow_table *ft;
	int max_tuples;
	int table_size;
	int prio;

	switch (flow_type_mask(fs->flow_type)) {
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
	case IP_USER_FLOW:
	case IPV6_USER_FLOW:
		max_tuples = ETHTOOL_NUM_L3_L4_FTS;
		prio = MLX5E_ETHTOOL_L3_L4_PRIO + (max_tuples - num_tuples);
		eth_ft = &ethtool->l3_l4_ft[prio];
		break;
	case ETHER_FLOW:
		max_tuples = ETHTOOL_NUM_L2_FTS;
		prio = max_tuples - num_tuples;
		eth_ft = &ethtool->l2_ft[prio];
		prio += MLX5E_ETHTOOL_L2_PRIO;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	eth_ft->num_rules++;
	if (eth_ft->ft)
		return eth_ft;

	ns = mlx5_get_flow_namespace(priv->mdev,
				     MLX5_FLOW_NAMESPACE_ETHTOOL);
	if (!ns)
		return ERR_PTR(-EOPNOTSUPP);

	table_size = min_t(u32, BIT(MLX5_CAP_FLOWTABLE(priv->mdev,
						       flow_table_properties_nic_receive.log_max_ft_size)),
			   MLX5E_ETHTOOL_NUM_ENTRIES);

	ft_attr.prio = prio;
	ft_attr.max_fte = table_size;
	ft_attr.autogroup.max_num_groups = MLX5E_ETHTOOL_NUM_GROUPS;
	ft = mlx5_create_auto_grouped_flow_table(ns, &ft_attr);
	if (IS_ERR(ft))
		return (void *)ft;

	eth_ft->ft = ft;
	return eth_ft;
}

static void mask_spec(u8 *mask, u8 *val, size_t size)
{
	unsigned int i;

	for (i = 0; i < size; i++, mask++, val++)
		*((u8 *)val) = *((u8 *)mask) & *((u8 *)val);
}

#define MLX5E_FTE_SET(header_p, fld, v)  \
	MLX5_SET(fte_match_set_lyr_2_4, header_p, fld, v)

#define MLX5E_FTE_ADDR_OF(header_p, fld) \
	MLX5_ADDR_OF(fte_match_set_lyr_2_4, header_p, fld)

static void
set_ip4(void *headers_c, void *headers_v, __be32 ip4src_m,
	__be32 ip4src_v, __be32 ip4dst_m, __be32 ip4dst_v)
{
	if (ip4src_m) {
		memcpy(MLX5E_FTE_ADDR_OF(headers_v, src_ipv4_src_ipv6.ipv4_layout.ipv4),
		       &ip4src_v, sizeof(ip4src_v));
		memcpy(MLX5E_FTE_ADDR_OF(headers_c, src_ipv4_src_ipv6.ipv4_layout.ipv4),
		       &ip4src_m, sizeof(ip4src_m));
	}
	if (ip4dst_m) {
		memcpy(MLX5E_FTE_ADDR_OF(headers_v, dst_ipv4_dst_ipv6.ipv4_layout.ipv4),
		       &ip4dst_v, sizeof(ip4dst_v));
		memcpy(MLX5E_FTE_ADDR_OF(headers_c, dst_ipv4_dst_ipv6.ipv4_layout.ipv4),
		       &ip4dst_m, sizeof(ip4dst_m));
	}

	MLX5E_FTE_SET(headers_c, ethertype, 0xffff);
	MLX5E_FTE_SET(headers_v, ethertype, ETH_P_IP);
}

static void
set_ip6(void *headers_c, void *headers_v, __be32 ip6src_m[4],
	__be32 ip6src_v[4], __be32 ip6dst_m[4], __be32 ip6dst_v[4])
{
	u8 ip6_sz = MLX5_FLD_SZ_BYTES(ipv6_layout, ipv6);

	if (!ipv6_addr_any((struct in6_addr *)ip6src_m)) {
		memcpy(MLX5E_FTE_ADDR_OF(headers_v, src_ipv4_src_ipv6.ipv6_layout.ipv6),
		       ip6src_v, ip6_sz);
		memcpy(MLX5E_FTE_ADDR_OF(headers_c, src_ipv4_src_ipv6.ipv6_layout.ipv6),
		       ip6src_m, ip6_sz);
	}
	if (!ipv6_addr_any((struct in6_addr *)ip6dst_m)) {
		memcpy(MLX5E_FTE_ADDR_OF(headers_v, dst_ipv4_dst_ipv6.ipv6_layout.ipv6),
		       ip6dst_v, ip6_sz);
		memcpy(MLX5E_FTE_ADDR_OF(headers_c, dst_ipv4_dst_ipv6.ipv6_layout.ipv6),
		       ip6dst_m, ip6_sz);
	}

	MLX5E_FTE_SET(headers_c, ethertype, 0xffff);
	MLX5E_FTE_SET(headers_v, ethertype, ETH_P_IPV6);
}

static void
set_tcp(void *headers_c, void *headers_v, __be16 psrc_m, __be16 psrc_v,
	__be16 pdst_m, __be16 pdst_v)
{
	if (psrc_m) {
		MLX5E_FTE_SET(headers_c, tcp_sport, ntohs(psrc_m));
		MLX5E_FTE_SET(headers_v, tcp_sport, ntohs(psrc_v));
	}
	if (pdst_m) {
		MLX5E_FTE_SET(headers_c, tcp_dport, ntohs(pdst_m));
		MLX5E_FTE_SET(headers_v, tcp_dport, ntohs(pdst_v));
	}

	MLX5E_FTE_SET(headers_c, ip_protocol, 0xffff);
	MLX5E_FTE_SET(headers_v, ip_protocol, IPPROTO_TCP);
}

static void
set_udp(void *headers_c, void *headers_v, __be16 psrc_m, __be16 psrc_v,
	__be16 pdst_m, __be16 pdst_v)
{
	if (psrc_m) {
		MLX5E_FTE_SET(headers_c, udp_sport, ntohs(psrc_m));
		MLX5E_FTE_SET(headers_v, udp_sport, ntohs(psrc_v));
	}

	if (pdst_m) {
		MLX5E_FTE_SET(headers_c, udp_dport, ntohs(pdst_m));
		MLX5E_FTE_SET(headers_v, udp_dport, ntohs(pdst_v));
	}

	MLX5E_FTE_SET(headers_c, ip_protocol, 0xffff);
	MLX5E_FTE_SET(headers_v, ip_protocol, IPPROTO_UDP);
}

static void
parse_tcp4(void *headers_c, void *headers_v, struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_tcpip4_spec *l4_mask = &fs->m_u.tcp_ip4_spec;
	struct ethtool_tcpip4_spec *l4_val  = &fs->h_u.tcp_ip4_spec;

	set_ip4(headers_c, headers_v, l4_mask->ip4src, l4_val->ip4src,
		l4_mask->ip4dst, l4_val->ip4dst);

	set_tcp(headers_c, headers_v, l4_mask->psrc, l4_val->psrc,
		l4_mask->pdst, l4_val->pdst);
}

static void
parse_udp4(void *headers_c, void *headers_v, struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_tcpip4_spec *l4_mask = &fs->m_u.udp_ip4_spec;
	struct ethtool_tcpip4_spec *l4_val  = &fs->h_u.udp_ip4_spec;

	set_ip4(headers_c, headers_v, l4_mask->ip4src, l4_val->ip4src,
		l4_mask->ip4dst, l4_val->ip4dst);

	set_udp(headers_c, headers_v, l4_mask->psrc, l4_val->psrc,
		l4_mask->pdst, l4_val->pdst);
}

static void
parse_ip4(void *headers_c, void *headers_v, struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_usrip4_spec *l3_mask = &fs->m_u.usr_ip4_spec;
	struct ethtool_usrip4_spec *l3_val  = &fs->h_u.usr_ip4_spec;

	set_ip4(headers_c, headers_v, l3_mask->ip4src, l3_val->ip4src,
		l3_mask->ip4dst, l3_val->ip4dst);

	if (l3_mask->proto) {
		MLX5E_FTE_SET(headers_c, ip_protocol, l3_mask->proto);
		MLX5E_FTE_SET(headers_v, ip_protocol, l3_val->proto);
	}
}

static void
parse_ip6(void *headers_c, void *headers_v, struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_usrip6_spec *l3_mask = &fs->m_u.usr_ip6_spec;
	struct ethtool_usrip6_spec *l3_val  = &fs->h_u.usr_ip6_spec;

	set_ip6(headers_c, headers_v, l3_mask->ip6src,
		l3_val->ip6src, l3_mask->ip6dst, l3_val->ip6dst);

	if (l3_mask->l4_proto) {
		MLX5E_FTE_SET(headers_c, ip_protocol, l3_mask->l4_proto);
		MLX5E_FTE_SET(headers_v, ip_protocol, l3_val->l4_proto);
	}
}

static void
parse_tcp6(void *headers_c, void *headers_v, struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_tcpip6_spec *l4_mask = &fs->m_u.tcp_ip6_spec;
	struct ethtool_tcpip6_spec *l4_val  = &fs->h_u.tcp_ip6_spec;

	set_ip6(headers_c, headers_v, l4_mask->ip6src,
		l4_val->ip6src, l4_mask->ip6dst, l4_val->ip6dst);

	set_tcp(headers_c, headers_v, l4_mask->psrc, l4_val->psrc,
		l4_mask->pdst, l4_val->pdst);
}

static void
parse_udp6(void *headers_c, void *headers_v, struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_tcpip6_spec *l4_mask = &fs->m_u.udp_ip6_spec;
	struct ethtool_tcpip6_spec *l4_val  = &fs->h_u.udp_ip6_spec;

	set_ip6(headers_c, headers_v, l4_mask->ip6src,
		l4_val->ip6src, l4_mask->ip6dst, l4_val->ip6dst);

	set_udp(headers_c, headers_v, l4_mask->psrc, l4_val->psrc,
		l4_mask->pdst, l4_val->pdst);
}

static void
parse_ether(void *headers_c, void *headers_v, struct ethtool_rx_flow_spec *fs)
{
	struct ethhdr *eth_mask = &fs->m_u.ether_spec;
	struct ethhdr *eth_val = &fs->h_u.ether_spec;

	mask_spec((u8 *)eth_mask, (u8 *)eth_val, sizeof(*eth_mask));
	ether_addr_copy(MLX5E_FTE_ADDR_OF(headers_c, smac_47_16), eth_mask->h_source);
	ether_addr_copy(MLX5E_FTE_ADDR_OF(headers_v, smac_47_16), eth_val->h_source);
	ether_addr_copy(MLX5E_FTE_ADDR_OF(headers_c, dmac_47_16), eth_mask->h_dest);
	ether_addr_copy(MLX5E_FTE_ADDR_OF(headers_v, dmac_47_16), eth_val->h_dest);
	MLX5E_FTE_SET(headers_c, ethertype, ntohs(eth_mask->h_proto));
	MLX5E_FTE_SET(headers_v, ethertype, ntohs(eth_val->h_proto));
}

static void
set_cvlan(void *headers_c, void *headers_v, __be16 vlan_tci)
{
	MLX5E_FTE_SET(headers_c, cvlan_tag, 1);
	MLX5E_FTE_SET(headers_v, cvlan_tag, 1);
	MLX5E_FTE_SET(headers_c, first_vid, 0xfff);
	MLX5E_FTE_SET(headers_v, first_vid, ntohs(vlan_tci));
}

static void
set_dmac(void *headers_c, void *headers_v,
	 unsigned char m_dest[ETH_ALEN], unsigned char v_dest[ETH_ALEN])
{
	ether_addr_copy(MLX5E_FTE_ADDR_OF(headers_c, dmac_47_16), m_dest);
	ether_addr_copy(MLX5E_FTE_ADDR_OF(headers_v, dmac_47_16), v_dest);
}

static int set_flow_attrs(u32 *match_c, u32 *match_v,
			  struct ethtool_rx_flow_spec *fs)
{
	void *outer_headers_c = MLX5_ADDR_OF(fte_match_param, match_c,
					     outer_headers);
	void *outer_headers_v = MLX5_ADDR_OF(fte_match_param, match_v,
					     outer_headers);
	u32 flow_type = flow_type_mask(fs->flow_type);

	switch (flow_type) {
	case TCP_V4_FLOW:
		parse_tcp4(outer_headers_c, outer_headers_v, fs);
		break;
	case UDP_V4_FLOW:
		parse_udp4(outer_headers_c, outer_headers_v, fs);
		break;
	case IP_USER_FLOW:
		parse_ip4(outer_headers_c, outer_headers_v, fs);
		break;
	case TCP_V6_FLOW:
		parse_tcp6(outer_headers_c, outer_headers_v, fs);
		break;
	case UDP_V6_FLOW:
		parse_udp6(outer_headers_c, outer_headers_v, fs);
		break;
	case IPV6_USER_FLOW:
		parse_ip6(outer_headers_c, outer_headers_v, fs);
		break;
	case ETHER_FLOW:
		parse_ether(outer_headers_c, outer_headers_v, fs);
		break;
	default:
		return -EINVAL;
	}

	if ((fs->flow_type & FLOW_EXT) &&
	    (fs->m_ext.vlan_tci & cpu_to_be16(VLAN_VID_MASK)))
		set_cvlan(outer_headers_c, outer_headers_v, fs->h_ext.vlan_tci);

	if (fs->flow_type & FLOW_MAC_EXT &&
	    !is_zero_ether_addr(fs->m_ext.h_dest)) {
		mask_spec(fs->m_ext.h_dest, fs->h_ext.h_dest, ETH_ALEN);
		set_dmac(outer_headers_c, outer_headers_v, fs->m_ext.h_dest,
			 fs->h_ext.h_dest);
	}

	return 0;
}

static void add_rule_to_list(struct mlx5e_priv *priv,
			     struct mlx5e_ethtool_rule *rule)
{
	struct mlx5e_ethtool_steering *ethtool = mlx5e_fs_get_ethtool(priv->fs);
	struct list_head *head = &ethtool->rules;
	struct mlx5e_ethtool_rule *iter;

	list_for_each_entry(iter, &ethtool->rules, list) {
		if (iter->flow_spec.location > rule->flow_spec.location)
			break;
		head = &iter->list;
	}
	ethtool->tot_num_rules++;
	list_add(&rule->list, head);
}

static bool outer_header_zero(u32 *match_criteria)
{
	int size = MLX5_FLD_SZ_BYTES(fte_match_param, outer_headers);
	char *outer_headers_c = MLX5_ADDR_OF(fte_match_param, match_criteria,
					     outer_headers);

	return outer_headers_c[0] == 0 && !memcmp(outer_headers_c,
						  outer_headers_c + 1,
						  size - 1);
}

static int flow_get_tirn(struct mlx5e_priv *priv,
			 struct mlx5e_ethtool_rule *eth_rule,
			 struct ethtool_rx_flow_spec *fs,
			 u32 rss_context, u32 *tirn)
{
	if (fs->flow_type & FLOW_RSS) {
		struct mlx5e_packet_merge_param pkt_merge_param;
		struct mlx5e_rss *rss;
		u32 flow_type;
		int err;
		int tt;

		rss = mlx5e_rx_res_rss_get(priv->rx_res, rss_context);
		if (!rss)
			return -ENOENT;

		flow_type = flow_type_mask(fs->flow_type);
		tt = flow_type_to_traffic_type(flow_type);
		if (tt < 0)
			return -EINVAL;

		pkt_merge_param = priv->channels.params.packet_merge;
		err = mlx5e_rss_obtain_tirn(rss, tt, &pkt_merge_param, false, tirn);
		if (err)
			return err;
		eth_rule->rss = rss;
		mlx5e_rss_refcnt_inc(eth_rule->rss);
	} else {
		*tirn = mlx5e_rx_res_get_tirn_direct(priv->rx_res, fs->ring_cookie);
	}

	return 0;
}

static struct mlx5_flow_handle *
add_ethtool_flow_rule(struct mlx5e_priv *priv,
		      struct mlx5e_ethtool_rule *eth_rule,
		      struct mlx5_flow_table *ft,
		      struct ethtool_rx_flow_spec *fs, u32 rss_context)
{
	struct mlx5_flow_act flow_act = { .flags = FLOW_ACT_NO_APPEND };
	struct mlx5_flow_destination *dst = NULL;
	struct mlx5_flow_handle *rule;
	struct mlx5_flow_spec *spec;
	int err = 0;

	spec = kvzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return ERR_PTR(-ENOMEM);
	err = set_flow_attrs(spec->match_criteria, spec->match_value,
			     fs);
	if (err)
		goto free;

	if (fs->ring_cookie == RX_CLS_FLOW_DISC) {
		flow_act.action = MLX5_FLOW_CONTEXT_ACTION_DROP;
	} else {
		dst = kzalloc(sizeof(*dst), GFP_KERNEL);
		if (!dst) {
			err = -ENOMEM;
			goto free;
		}

		err = flow_get_tirn(priv, eth_rule, fs, rss_context, &dst->tir_num);
		if (err)
			goto free;

		dst->type = MLX5_FLOW_DESTINATION_TYPE_TIR;
		flow_act.action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	}

	spec->match_criteria_enable = (!outer_header_zero(spec->match_criteria));
	spec->flow_context.flow_tag = MLX5_FS_DEFAULT_FLOW_TAG;
	rule = mlx5_add_flow_rules(ft, spec, &flow_act, dst, dst ? 1 : 0);
	if (IS_ERR(rule)) {
		err = PTR_ERR(rule);
		netdev_err(priv->netdev, "%s: failed to add ethtool steering rule: %d\n",
			   __func__, err);
		goto free;
	}
free:
	kvfree(spec);
	kfree(dst);
	return err ? ERR_PTR(err) : rule;
}

static void del_ethtool_rule(struct mlx5e_flow_steering *fs,
			     struct mlx5e_ethtool_rule *eth_rule)
{
	struct mlx5e_ethtool_steering *ethtool = mlx5e_fs_get_ethtool(fs);
	if (eth_rule->rule)
		mlx5_del_flow_rules(eth_rule->rule);
	if (eth_rule->rss)
		mlx5e_rss_refcnt_dec(eth_rule->rss);
	list_del(&eth_rule->list);
	ethtool->tot_num_rules--;
	put_flow_table(eth_rule->eth_ft);
	kfree(eth_rule);
}

static struct mlx5e_ethtool_rule *find_ethtool_rule(struct mlx5e_priv *priv,
						    int location)
{
	struct mlx5e_ethtool_steering *ethtool = mlx5e_fs_get_ethtool(priv->fs);
	struct mlx5e_ethtool_rule *iter;

	list_for_each_entry(iter, &ethtool->rules, list) {
		if (iter->flow_spec.location == location)
			return iter;
	}
	return NULL;
}

static struct mlx5e_ethtool_rule *get_ethtool_rule(struct mlx5e_priv *priv,
						   int location)
{
	struct mlx5e_ethtool_rule *eth_rule;

	eth_rule = find_ethtool_rule(priv, location);
	if (eth_rule)
		del_ethtool_rule(priv->fs, eth_rule);

	eth_rule = kzalloc(sizeof(*eth_rule), GFP_KERNEL);
	if (!eth_rule)
		return ERR_PTR(-ENOMEM);

	add_rule_to_list(priv, eth_rule);
	return eth_rule;
}

#define MAX_NUM_OF_ETHTOOL_RULES BIT(10)

#define all_ones(field) (field == (__force typeof(field))-1)
#define all_zeros_or_all_ones(field)		\
	((field) == 0 || (field) == (__force typeof(field))-1)

static int validate_ethter(struct ethtool_rx_flow_spec *fs)
{
	struct ethhdr *eth_mask = &fs->m_u.ether_spec;
	int ntuples = 0;

	if (!is_zero_ether_addr(eth_mask->h_dest))
		ntuples++;
	if (!is_zero_ether_addr(eth_mask->h_source))
		ntuples++;
	if (eth_mask->h_proto)
		ntuples++;
	return ntuples;
}

static int validate_tcpudp4(struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_tcpip4_spec *l4_mask = &fs->m_u.tcp_ip4_spec;
	int ntuples = 0;

	if (l4_mask->tos)
		return -EINVAL;

	if (l4_mask->ip4src)
		ntuples++;
	if (l4_mask->ip4dst)
		ntuples++;
	if (l4_mask->psrc)
		ntuples++;
	if (l4_mask->pdst)
		ntuples++;
	/* Flow is TCP/UDP */
	return ++ntuples;
}

static int validate_ip4(struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_usrip4_spec *l3_mask = &fs->m_u.usr_ip4_spec;
	int ntuples = 0;

	if (l3_mask->l4_4_bytes || l3_mask->tos ||
	    fs->h_u.usr_ip4_spec.ip_ver != ETH_RX_NFC_IP4)
		return -EINVAL;
	if (l3_mask->ip4src)
		ntuples++;
	if (l3_mask->ip4dst)
		ntuples++;
	if (l3_mask->proto)
		ntuples++;
	/* Flow is IPv4 */
	return ++ntuples;
}

static int validate_ip6(struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_usrip6_spec *l3_mask = &fs->m_u.usr_ip6_spec;
	int ntuples = 0;

	if (l3_mask->l4_4_bytes || l3_mask->tclass)
		return -EINVAL;
	if (!ipv6_addr_any((struct in6_addr *)l3_mask->ip6src))
		ntuples++;

	if (!ipv6_addr_any((struct in6_addr *)l3_mask->ip6dst))
		ntuples++;
	if (l3_mask->l4_proto)
		ntuples++;
	/* Flow is IPv6 */
	return ++ntuples;
}

static int validate_tcpudp6(struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_tcpip6_spec *l4_mask = &fs->m_u.tcp_ip6_spec;
	int ntuples = 0;

	if (l4_mask->tclass)
		return -EINVAL;

	if (!ipv6_addr_any((struct in6_addr *)l4_mask->ip6src))
		ntuples++;

	if (!ipv6_addr_any((struct in6_addr *)l4_mask->ip6dst))
		ntuples++;

	if (l4_mask->psrc)
		ntuples++;
	if (l4_mask->pdst)
		ntuples++;
	/* Flow is TCP/UDP */
	return ++ntuples;
}

static int validate_vlan(struct ethtool_rx_flow_spec *fs)
{
	if (fs->m_ext.vlan_etype ||
	    fs->m_ext.vlan_tci != cpu_to_be16(VLAN_VID_MASK))
		return -EINVAL;

	if (fs->m_ext.vlan_tci &&
	    (be16_to_cpu(fs->h_ext.vlan_tci) >= VLAN_N_VID))
		return -EINVAL;

	return 1;
}

static int validate_flow(struct mlx5e_priv *priv,
			 struct ethtool_rx_flow_spec *fs)
{
	int num_tuples = 0;
	int ret = 0;

	if (fs->location >= MAX_NUM_OF_ETHTOOL_RULES)
		return -ENOSPC;

	if (fs->ring_cookie != RX_CLS_FLOW_DISC)
		if (fs->ring_cookie >= priv->channels.params.num_channels)
			return -EINVAL;

	switch (flow_type_mask(fs->flow_type)) {
	case ETHER_FLOW:
		num_tuples += validate_ethter(fs);
		break;
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
		ret = validate_tcpudp4(fs);
		if (ret < 0)
			return ret;
		num_tuples += ret;
		break;
	case IP_USER_FLOW:
		ret = validate_ip4(fs);
		if (ret < 0)
			return ret;
		num_tuples += ret;
		break;
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
		ret = validate_tcpudp6(fs);
		if (ret < 0)
			return ret;
		num_tuples += ret;
		break;
	case IPV6_USER_FLOW:
		ret = validate_ip6(fs);
		if (ret < 0)
			return ret;
		num_tuples += ret;
		break;
	default:
		return -ENOTSUPP;
	}
	if ((fs->flow_type & FLOW_EXT)) {
		ret = validate_vlan(fs);
		if (ret < 0)
			return ret;
		num_tuples += ret;
	}

	if (fs->flow_type & FLOW_MAC_EXT &&
	    !is_zero_ether_addr(fs->m_ext.h_dest))
		num_tuples++;

	return num_tuples;
}

static int
mlx5e_ethtool_flow_replace(struct mlx5e_priv *priv,
			   struct ethtool_rx_flow_spec *fs, u32 rss_context)
{
	struct mlx5e_ethtool_table *eth_ft;
	struct mlx5e_ethtool_rule *eth_rule;
	struct mlx5_flow_handle *rule;
	int num_tuples;
	int err;

	num_tuples = validate_flow(priv, fs);
	if (num_tuples <= 0) {
		netdev_warn(priv->netdev, "%s: flow is not valid %d\n",
			    __func__, num_tuples);
		return num_tuples < 0 ? num_tuples : -EINVAL;
	}

	eth_ft = get_flow_table(priv, fs, num_tuples);
	if (IS_ERR(eth_ft))
		return PTR_ERR(eth_ft);

	eth_rule = get_ethtool_rule(priv, fs->location);
	if (IS_ERR(eth_rule)) {
		put_flow_table(eth_ft);
		return PTR_ERR(eth_rule);
	}

	eth_rule->flow_spec = *fs;
	eth_rule->eth_ft = eth_ft;

	rule = add_ethtool_flow_rule(priv, eth_rule, eth_ft->ft, fs, rss_context);
	if (IS_ERR(rule)) {
		err = PTR_ERR(rule);
		goto del_ethtool_rule;
	}

	eth_rule->rule = rule;

	return 0;

del_ethtool_rule:
	del_ethtool_rule(priv->fs, eth_rule);

	return err;
}

static int
mlx5e_ethtool_flow_remove(struct mlx5e_priv *priv, int location)
{
	struct mlx5e_ethtool_rule *eth_rule;
	int err = 0;

	if (location >= MAX_NUM_OF_ETHTOOL_RULES)
		return -ENOSPC;

	eth_rule = find_ethtool_rule(priv, location);
	if (!eth_rule) {
		err =  -ENOENT;
		goto out;
	}

	del_ethtool_rule(priv->fs, eth_rule);
out:
	return err;
}

static int
mlx5e_ethtool_get_flow(struct mlx5e_priv *priv,
		       struct ethtool_rxnfc *info, int location)
{
	struct mlx5e_ethtool_steering *ethtool = mlx5e_fs_get_ethtool(priv->fs);
	struct mlx5e_ethtool_rule *eth_rule;

	if (location < 0 || location >= MAX_NUM_OF_ETHTOOL_RULES)
		return -EINVAL;

	list_for_each_entry(eth_rule, &ethtool->rules, list) {
		int index;

		if (eth_rule->flow_spec.location != location)
			continue;
		if (!info)
			return 0;
		info->fs = eth_rule->flow_spec;
		if (!eth_rule->rss)
			return 0;
		index = mlx5e_rx_res_rss_index(priv->rx_res, eth_rule->rss);
		if (index < 0)
			return index;
		info->rss_context = index;
		return 0;
	}

	return -ENOENT;
}

static int
mlx5e_ethtool_get_all_flows(struct mlx5e_priv *priv,
			    struct ethtool_rxnfc *info, u32 *rule_locs)
{
	int location = 0;
	int idx = 0;
	int err = 0;

	info->data = MAX_NUM_OF_ETHTOOL_RULES;
	while ((!err || err == -ENOENT) && idx < info->rule_cnt) {
		err = mlx5e_ethtool_get_flow(priv, NULL, location);
		if (!err)
			rule_locs[idx++] = location;
		location++;
	}
	return err;
}

int mlx5e_ethtool_alloc(struct mlx5e_ethtool_steering **ethtool)
{
	*ethtool =  kvzalloc(sizeof(**ethtool), GFP_KERNEL);
	if (!*ethtool)
		return -ENOMEM;
	return 0;
}

void mlx5e_ethtool_free(struct mlx5e_ethtool_steering *ethtool)
{
	kvfree(ethtool);
}

void mlx5e_ethtool_cleanup_steering(struct mlx5e_flow_steering *fs)
{
	struct mlx5e_ethtool_steering *ethtool = mlx5e_fs_get_ethtool(fs);
	struct mlx5e_ethtool_rule *iter;
	struct mlx5e_ethtool_rule *temp;

	list_for_each_entry_safe(iter, temp, &ethtool->rules, list)
		del_ethtool_rule(fs, iter);
}

void mlx5e_ethtool_init_steering(struct mlx5e_flow_steering *fs)
{
	struct mlx5e_ethtool_steering *ethtool = mlx5e_fs_get_ethtool(fs);

	INIT_LIST_HEAD(&ethtool->rules);
}

static int flow_type_to_traffic_type(u32 flow_type)
{
	switch (flow_type) {
	case TCP_V4_FLOW:
		return MLX5_TT_IPV4_TCP;
	case TCP_V6_FLOW:
		return MLX5_TT_IPV6_TCP;
	case UDP_V4_FLOW:
		return MLX5_TT_IPV4_UDP;
	case UDP_V6_FLOW:
		return MLX5_TT_IPV6_UDP;
	case AH_V4_FLOW:
		return MLX5_TT_IPV4_IPSEC_AH;
	case AH_V6_FLOW:
		return MLX5_TT_IPV6_IPSEC_AH;
	case ESP_V4_FLOW:
		return MLX5_TT_IPV4_IPSEC_ESP;
	case ESP_V6_FLOW:
		return MLX5_TT_IPV6_IPSEC_ESP;
	case IPV4_FLOW:
	case IP_USER_FLOW:
		return MLX5_TT_IPV4;
	case IPV6_FLOW:
	case IPV6_USER_FLOW:
		return MLX5_TT_IPV6;
	default:
		return -EINVAL;
	}
}

int mlx5e_ethtool_set_rxfh_fields(struct mlx5e_priv *priv,
				  const struct ethtool_rxfh_fields *nfc,
				  struct netlink_ext_ack *extack)
{
	u8 rx_hash_field = 0;
	u32 flow_type = 0;
	u32 rss_idx;
	int err;
	int tt;

	rss_idx = nfc->rss_context;

	flow_type = flow_type_mask(nfc->flow_type);
	tt = flow_type_to_traffic_type(flow_type);
	if (tt < 0)
		return tt;

	/*  RSS does not support anything other than hashing to queues
	 *  on src IP, dest IP, TCP/UDP src port and TCP/UDP dest
	 *  port.
	 */
	if (flow_type != TCP_V4_FLOW &&
	    flow_type != TCP_V6_FLOW &&
	    flow_type != UDP_V4_FLOW &&
	    flow_type != UDP_V6_FLOW)
		return -EOPNOTSUPP;

	if (nfc->data & ~(RXH_IP_SRC | RXH_IP_DST |
			  RXH_L4_B_0_1 | RXH_L4_B_2_3))
		return -EOPNOTSUPP;

	if (nfc->data & RXH_IP_SRC)
		rx_hash_field |= MLX5_HASH_FIELD_SEL_SRC_IP;
	if (nfc->data & RXH_IP_DST)
		rx_hash_field |= MLX5_HASH_FIELD_SEL_DST_IP;
	if (nfc->data & RXH_L4_B_0_1)
		rx_hash_field |= MLX5_HASH_FIELD_SEL_L4_SPORT;
	if (nfc->data & RXH_L4_B_2_3)
		rx_hash_field |= MLX5_HASH_FIELD_SEL_L4_DPORT;

	mutex_lock(&priv->state_lock);
	err = mlx5e_rx_res_rss_set_hash_fields(priv->rx_res, rss_idx, tt, rx_hash_field);
	mutex_unlock(&priv->state_lock);

	return err;
}

int mlx5e_ethtool_get_rxfh_fields(struct mlx5e_priv *priv,
				  struct ethtool_rxfh_fields *nfc)
{
	int hash_field = 0;
	u32 flow_type = 0;
	u32 rss_idx;
	int tt;

	rss_idx = nfc->rss_context;

	flow_type = flow_type_mask(nfc->flow_type);
	tt = flow_type_to_traffic_type(flow_type);
	if (tt < 0)
		return tt;

	hash_field = mlx5e_rx_res_rss_get_hash_fields(priv->rx_res, rss_idx, tt);
	if (hash_field < 0)
		return hash_field;

	nfc->data = 0;

	if (hash_field & MLX5_HASH_FIELD_SEL_SRC_IP)
		nfc->data |= RXH_IP_SRC;
	if (hash_field & MLX5_HASH_FIELD_SEL_DST_IP)
		nfc->data |= RXH_IP_DST;
	if (hash_field & MLX5_HASH_FIELD_SEL_L4_SPORT)
		nfc->data |= RXH_L4_B_0_1;
	if (hash_field & MLX5_HASH_FIELD_SEL_L4_DPORT)
		nfc->data |= RXH_L4_B_2_3;

	return 0;
}

int mlx5e_ethtool_set_rxnfc(struct mlx5e_priv *priv, struct ethtool_rxnfc *cmd)
{
	int err = 0;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		err = mlx5e_ethtool_flow_replace(priv, &cmd->fs, cmd->rss_context);
		break;
	case ETHTOOL_SRXCLSRLDEL:
		err = mlx5e_ethtool_flow_remove(priv, cmd->fs.location);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

int mlx5e_ethtool_get_rxnfc(struct mlx5e_priv *priv,
			    struct ethtool_rxnfc *info, u32 *rule_locs)
{
	struct mlx5e_ethtool_steering *ethtool = mlx5e_fs_get_ethtool(priv->fs);
	int err = 0;

	switch (info->cmd) {
	case ETHTOOL_GRXCLSRLCNT:
		info->rule_cnt = ethtool->tot_num_rules;
		break;
	case ETHTOOL_GRXCLSRULE:
		err = mlx5e_ethtool_get_flow(priv, info, info->fs.location);
		break;
	case ETHTOOL_GRXCLSRLALL:
		err = mlx5e_ethtool_get_all_flows(priv, info, rule_locs);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

