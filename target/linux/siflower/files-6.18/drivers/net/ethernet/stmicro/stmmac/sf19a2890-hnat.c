// SPDX-License-Identifier: GPL-2.0
/* Siflower SF19A2890 GMAC hardware NAT (HNAT) engine.
 *
 * The engine sits between the GMAC's MAC and DMA. nf_flow_table flows
 * are offloaded to it through the GMAC's ndo_setup_tc (TC_SETUP_FT, via
 * the setup_tc_ft platform hook). Table layout and register programming
 * follow the vendor 4.14 SDK (sf_hnat.c); the flow_block and flow_rule
 * handling follows mtk_ppe_offload.c.
 *
 * debugfs (sf19a2890_hnat/): registers, tables, a table access
 * self-test and raw table access.
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/rhashtable.h>
#include <linux/if_ether.h>
#include <linux/etherdevice.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/inetdevice.h>
#include <linux/if_vlan.h>
#include <net/flow_offload.h>
#include <net/dsa.h>
#include <linux/dsa/8021q.h>
#include <net/pkt_cls.h>

#include "sf19a2890-hnat-reg.h"
#include "sf19a2890-hnat.h"

#define HNAT_MAP_OFFSET			HNAT_CSR_BASE

static const u32 hnat_zero_row[HWNAT_TB_WRDATA_NO];

/* offsets below are relative to HNAT_MAP_OFFSET, i.e. HNAT_CSR_BASE */
#define REG(x)				((x) - HNAT_MAP_OFFSET)

/* TC_SETUP_FT offload.
 *
 * ndo_setup_tc reaches sf19a2890_hnat_setup_tc_ft() through the
 * setup_tc_ft platform hook, which registers a flow_block callback.
 * Flow add/del/stats requests then arrive as TC_SETUP_CLSFLOWER with a
 * flow_cls_offload carrying the flow's match keys (5-tuple) and actions
 * (NAT mangle, redirect). sf19a2890_hnat_commit_entry() and
 * sf19a2890_hnat_clear_entry() program the tables.
 */

struct sf19a2890_hnat_flow_data {
	struct ethhdr eth;

	/* mutated by FLOW_ACTION_MANGLE -> final/NAT'd tuple */
	__be32 src_ip, dst_ip;
	__be16 src_port, dst_port;

	/* as matched at ingress, before any mangle -- needed for hash
	 * placement / hardware lookup key, which must key off the
	 * pre-NAT tuple the hardware actually sees on the wire.
	 */
	struct {
		__be32 src_ip, dst_ip;
		__be16 src_port, dst_port;
	} orig;

	u8 proto;
	struct net_device *odev;

	/* 802.1Q VID matched at ingress / pushed at egress, 0 = untagged */
	u16 in_vid, out_vid;

	/* PPPoE session pushed at egress (FLOW_ACTION_PPPOE_PUSH) */
	bool pppoe;
	u16 pppoe_sid;
};

/* Register-level table management, ported from the vendor SDK's
 * sf_hnat.c. NAPT/DIP/DMAC/ROUTER_MAC/RT_PUB_NET table state is tracked
 * in software (the hardware has no "find me a free slot" query), under
 * the struct sf19a2890_hnat lock.
 *
 * Scope (see sf19a2890_hnat_flow_replace() for the matching flow_rule
 * restrictions): IPv4 TCP/UDP, single WAN, at most one 802.1Q tag per
 * side (untagged uses the vendor's "no vlan" value 0x7f), PPPoE on the
 * WAN side, HNAT_MODE_SYMMETRIC (full 5-tuple hash in both directions).
 * The engine has no IPv6 path, so IPv6 flows stay on the software
 * flowtable.
 */

#define SF_HNAT_NO_VLAN_INDEX	0x7f

struct sf_dip_entry {
	__be32 dip;
	u8 dmac_index;
	u8 vlan_index;
	bool valid;
	u16 ref_count;
};

struct sf_dip_crc_info {
	u16 crc_key[2];
	u8 ptr_index;
};

struct sf_dmac_entry {
	u8 dmac[ETH_ALEN];
	u8 router_mac_index;
	bool valid;
	u16 ref_count;
};

struct sf_rmac_entry {
	u8 rmac[ETH_ALEN];
	bool valid;
	u16 ref_count;
};

struct sf_rt_pub_net_entry {
	__be32 router_pub_ip;
	u8 pub_vlan_index;
	u8 mask;		/* WAN prefix length, 31 for a /32 (PPPoE) */
	bool valid;
	u16 ref_count;
};

struct sf_napt_crc_info {
	u16 inat_crc_key[3];
	u16 enat_crc_key[3];
	u8 inat_ptr_index;
	u8 enat_ptr_index;
};

static bool hnat_debug;
module_param(hnat_debug, bool, 0644);
MODULE_PARM_DESC(hnat_debug, "log every offload request and its parsed fields");

struct sf_ppphd_entry {
	u16 sid;
	bool valid;
	u16 ref_count;
};

struct sf_vlan_entry {
	u16 vid;
	bool valid;
	u16 ref_count;
};

struct sf_lan_subnet {
	u32 ip;		/* host order, the router's own address, as the vendor writes it */
	u8 prefix_len;
	bool valid;
};

struct sf19a2890_hnat {
	void __iomem *base;		/* HNAT_CSR_BASE in the GMAC window */
	struct device *dev;
	struct net_device *ndev;
	struct dentry *debugfs;
	bool csr_ready;			/* sf19a2890_hnat_csr_init() has run */

	/* serialises flow commits against engine (re)programming */
	struct mutex flow_mutex;
	struct list_head block_cb_list;
	struct rhashtable flow_table;
	struct rhashtable napt_table;

	/* software copies of the hardware tables */
	spinlock_t lock;
	struct sf_dip_entry dip_table[DIP_TABLE_MAX];
	struct sf_dip_crc_info dip_crc[DIP_TABLE_MAX];
	struct sf_dmac_entry dmac_table[DMAC_TABLE_MAX];
	struct sf_rmac_entry rmac_table[ROUTER_MAC_TABLE_MAX];
	struct sf_rt_pub_net_entry rt_pub_net_table[RT_PUB_NET_TABLE_MAX];
	struct sf_ppphd_entry ppphd_table[PPPHD_TABLE_MAX];
	struct sf_vlan_entry vlan_table[VLAN_TABLE_MAX];
	struct sf_lan_subnet lan_subnets[SUBNET_TABLE_MAX];
	/* NULL = free slot; non-NULL = owning struct sf19a2890_hnat_napt */
	void *napt_slots[NAPT_TABLE_MAX];
};

static u32 hnat_r(struct sf19a2890_hnat *h, u32 reg)
{
	return readl(h->base + REG(reg));
}

static void hnat_w(struct sf19a2890_hnat *h, u32 reg, u32 val)
{
	writel(val, h->base + REG(reg));
}

/* poll HWNAT_REG1_TB_STATUS busy bit; returns 0 on success, -ETIMEDOUT otherwise */
static int hnat_wait_idle(struct sf19a2890_hnat *h)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if (!(hnat_r(h, HWNAT_REG1_TB_STATUS) & HWNAT_REG1_TB_STATUS_BUSY))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int hnat_table_write(struct sf19a2890_hnat *h, u32 table_no, u32 index,
			    const u32 wdata[HWNAT_TB_WRDATA_NO])
{
	int ret, i;
	static const u32 wdata_regs[HWNAT_TB_WRDATA_NO] = {
		HWNAT_REG5_TB_WRDATA0, HWNAT_REG6_TB_WRDATA1, HWNAT_REG7_TB_WRDATA2,
		HWNAT_REG8_TB_WRDATA3, HWNAT_REG9_TB_WRDATA4,
	};

	ret = hnat_wait_idle(h);
	if (ret)
		return ret;

	for (i = 0; i < HWNAT_TB_WRDATA_NO; i++)
		hnat_w(h, wdata_regs[i], wdata[i]);

	hnat_w(h, HWNAT_REG3_TB_ADDRESS, (table_no << HWNAT_REG3_TB_ADD_NO_OFF) | index);
	hnat_w(h, HWNAT_REG4_TB_OP_CODE, HWNAT_REG4_TB_OP_CODE_WR);

	return hnat_wait_idle(h);
}

static int hnat_table_read(struct sf19a2890_hnat *h, u32 table_no, u32 index,
			   u32 rdata[HWNAT_TB_RDDATA_NO])
{
	int ret, i;
	static const u32 rdata_regs[HWNAT_TB_RDDATA_NO] = {
		HWNAT_REG10_TB_RDDATA0, HWNAT_REG11_TB_RDDATA1, HWNAT_REG12_TB_RDDATA2,
		HWNAT_REG13_TB_RDDATA3, HWNAT_REG14_TB_RDDATA4, HWNAT_REG15_TB_RDDATA5,
		HWNAT_REG16_TB_RDDATA6,
	};

	ret = hnat_wait_idle(h);
	if (ret)
		return ret;

	hnat_w(h, HWNAT_REG3_TB_ADDRESS, (table_no << HWNAT_REG3_TB_ADD_NO_OFF) | index);
	hnat_w(h, HWNAT_REG4_TB_OP_CODE, HWNAT_REG4_TB_OP_CODE_RD);

	ret = hnat_wait_idle(h);
	if (ret)
		return ret;

	for (i = 0; i < HWNAT_TB_RDDATA_NO; i++)
		rdata[i] = hnat_r(h, rdata_regs[i]);

	return 0;
}

static int hnat_regs_show(struct seq_file *s, void *unused)
{
	struct sf19a2890_hnat *h = s->private;

	seq_printf(s, "REG0_CSR                 0x%08x\n", hnat_r(h, HWNAT_REG0_CSR));
	seq_printf(s, "REG1_CSR                 0x%08x\n", hnat_r(h, HWNAT_REG1_CSR));
	seq_printf(s, "REG2_CSR (table enable)  0x%08x\n", hnat_r(h, HWNAT_REG2_CSR));
	seq_printf(s, "REG1_TB_STATUS (busy)    0x%08x\n", hnat_r(h, HWNAT_REG1_TB_STATUS));
	seq_printf(s, "REG2_TB_CONFIG           0x%08x\n", hnat_r(h, HWNAT_REG2_TB_CONFIG));
	seq_printf(s, "LAN_SUBNET0 / MASK0-3    0x%08x / 0x%08x\n",
		   hnat_r(h, HWNAT_REG16_CSR), hnat_r(h, HWNAT_REG30_CSR));
	/* the counters only update on a 0->1 edge of the latch bit, as in
	 * the vendor's sf_hnat_read_count_registers(); they are clear-on-read
	 */
	hnat_w(h, HWNAT_REG2_CSR, hnat_r(h, HWNAT_REG2_CSR) & ~HWNAT_REG2_CSR_CNT_LATCH);
	hnat_w(h, HWNAT_REG2_CSR, hnat_r(h, HWNAT_REG2_CSR) | HWNAT_REG2_CSR_CNT_LATCH);
	seq_printf(s, "RX_ENTER_SOF_CNT_L       0x%08x\n", hnat_r(h, HWNAT_RX_ENTER_SOF_CNT_L));
	seq_printf(s, "RX_ENTER_DROP_CNT        0x%08x\n", hnat_r(h, HWNAT_RX_ENTER_DROP_CNT));
	seq_printf(s, "TX_ENAT_CNT              0x%08x\n", hnat_r(h, HWNAT_TX_ENAT_CNT));
	seq_printf(s, "RX_ENAT_CNT              0x%08x\n", hnat_r(h, HWNAT_RX_ENAT_CNT));
	seq_printf(s, "RX_INAT_CNT              0x%08x\n", hnat_r(h, HWNAT_RX_INAT_CNT));
	seq_printf(s, "RX_TOTAL_ERR_CNT         0x%08x\n", hnat_r(h, HWNAT_RX_TOTAL_ERR_CNT));
	seq_printf(s, "RX2TX_DROP_CNT           0x%08x\n", hnat_r(h, HWNAT_RX2TX_DROP_CNT));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hnat_regs);

/* CRC16, two different polynomials, matching sf_hnat_crc16() exactly --
 * this must reproduce whatever the hardware's own search engine computes
 * internally, since entries are placed at these exact indices for the
 * hardware to find on lookup (not just a software bookkeeping choice).
 */
static u16 sf_hnat_crc16(const u8 *buf, u8 len, u16 crc_key[3])
{
	u16 crc1 = 0, crc2 = 0;
	int i;

	for (i = 0; i < len; i++) {
		int b;

		crc1 ^= buf[i] << 8;
		for (b = 0; b < 8; b++)
			crc1 = (crc1 & 0x8000) ? (crc1 << 1) ^ 0x1021 : crc1 << 1;
	}
	for (i = 0; i < len; i++) {
		int b;

		crc2 ^= buf[i] << 8;
		for (b = 0; b < 8; b++)
			crc2 = (crc2 & 0x8000) ? (crc2 << 1) ^ 0x8005 : crc2 << 1;
	}

	if (crc_key) {
		crc_key[0] = (crc1 & 0xff80) >> 7;
		crc_key[1] = crc1 & 0xff;
		crc_key[2] = (crc2 & 0x1f0) >> 4;
	}
	return crc1;
}

/* HNAT_MODE_SYMMETRIC CRC input layout, from sf_napt_key_crc_get(): the
 * protocol byte is the engine's is_udp flag (0 = TCP, 1 = UDP),
 * addresses and ports in network byte order. That byte order holds with
 * HWNAT_REG2_TB_CONFIG_EN_BIG_ENDIAN set, which is how the vendor
 * driver runs the engine (TB_CONFIG 0x0fff0414); with it clear the
 * engine hashes host-order fields instead.
 */
static void sf_napt_crc_data(bool is_inat, u8 proto, __be32 dip, __be16 dport,
			     __be32 router_pub_ip, __be16 router_port,
			     u16 crc_key[3])
{
	u8 crc_data[13] = { 0 };

	crc_data[0] = proto == IPPROTO_UDP;
	memcpy(crc_data + 1, &dport, 2);
	memcpy(crc_data + 3, &dip, 4);
	memcpy(crc_data + 7, &router_port, 2);
	memcpy(crc_data + 9, &router_pub_ip, 4);
	sf_hnat_crc16(crc_data, 13, crc_key);
	(void)is_inat; /* SYMMETRIC_MODE's crc_data layout is dport/dip/router_port/router_pub_ip
			* for INAT; for ENAT the caller passes sport/sip in the dport/dip
			* slots and dport/dip in the router_port/router_pub_ip slots.
			*/
}

/* ----- NAPT table row + hash-chain placement ----- */

static int sf_hnat_napt_clear_row(struct sf19a2890_hnat *h, u32 napt_index)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	return hnat_table_write(h, NAPT_NO, napt_index, data);
}

static int sf_hnat_napt_vld_cfg(struct sf19a2890_hnat *h, u32 napt_index, bool enable)
{
	u32 depth = napt_index / 32;
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };
	int ret;

	ret = hnat_table_read(h, NAPT_VLD_NO, depth, data);
	if (ret)
		return ret;
	if (enable)
		data[0] |= BIT(napt_index - depth * 32);
	else
		data[0] &= ~BIT(napt_index - depth * 32);
	return hnat_table_write(h, NAPT_VLD_NO, depth, data);
}

/* 2-way hash bucket used by HASH1 and HASH3, per sf_hnat_napt_add_crc_hash_table_13(). */
static int sf_hnat_napt_hash13_add(struct sf19a2890_hnat *h, u32 tab_no, u32 hash_index,
				   u32 napt_index)
{
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };
	int i, ret;

	ret = hnat_table_read(h, tab_no, hash_index, data);
	if (ret)
		return ret;

	for (i = 0; i < 2; i++)
		if (!(data[0] & BIT(20 + i)))
			break;
	if (i == 2)
		return -ENOSPC;

	data[0] &= ~(0x3ff << (i * 10));
	data[0] |= (napt_index << (i * 10)) | BIT(20 + i);
	ret = hnat_table_write(h, tab_no, hash_index, data);
	return ret ? : i;
}

/* 4-way hash bucket used by HASH2, per sf_hnat_napt_add_crc_hash_table_2(). */
static int sf_hnat_napt_hash2_add(struct sf19a2890_hnat *h, u32 tab_no, u32 hash_index,
				  u32 napt_index)
{
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };
	int i, ret;

	ret = hnat_table_read(h, tab_no, hash_index, data);
	if (ret)
		return ret;

	for (i = 0; i < 4; i++)
		if (!(data[1] & BIT(i + 8)))
			break;
	if (i == 4)
		return -ENOSPC;

	if (i < 3) {
		data[0] &= ~(0x3ff << (i * 10));
		data[0] |= napt_index << (i * 10);
		data[1] |= BIT(i + 8);
	} else {
		data[0] &= ~(0x3 << (i * 10));
		data[1] &= ~0xff;
		data[0] |= (napt_index & 0x3) << (i * 10);
		data[1] |= BIT(i + 8) | ((napt_index & 0x3fc) >> 2);
	}
	ret = hnat_table_write(h, tab_no, hash_index, data);
	return ret ? : i;
}

static int sf_hnat_napt_hash13_del(struct sf19a2890_hnat *h, u32 tab_no, u32 hash_index, u32 way)
{
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };
	int ret;

	ret = hnat_table_read(h, tab_no, hash_index, data);
	if (ret)
		return ret;
	data[0] &= ~BIT(20 + way);
	return hnat_table_write(h, tab_no, hash_index, data);
}

static int sf_hnat_napt_hash2_del(struct sf19a2890_hnat *h, u32 tab_no, u32 hash_index, u32 way)
{
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };
	int ret;

	ret = hnat_table_read(h, tab_no, hash_index, data);
	if (ret)
		return ret;
	data[1] &= ~BIT(way + 8);
	return hnat_table_write(h, tab_no, hash_index, data);
}

/*
 * Places one direction's hash pointer at napt_index, trying HASH1 then
 * HASH2 then HASH3 (sf_hnat_napt_common_add_crc_hash_entry()). is_inat
 * selects which triplet of hash tables (INAT_NAPT_HASH1/2/3_NO vs
 * ENAT_NAPT_HASH1/2/3_NO) and which pair of fields form the CRC input:
 * ENAT hashes off the private-side view (sip/sport as the "dip/dport"
 * slot, dip/dport as the "router_pub_ip/router_port" slot -- ENAT's own
 * match tuple, i.e. what the LAN client actually sent); INAT hashes off
 * the public-side view (dip/dport, router_pub_ip/router_port exactly as
 * named) -- i.e. what arrives from the WAN. Both resolve to the SAME
 * napt_index; the hardware presumably picks the rewrite direction based
 * on which hash chain matched, not on anything stored in the row itself.
 */
static int sf19a2890_hnat_napt_add_hash(struct sf19a2890_hnat *h, struct sf_napt_crc_info *info,
					u32 napt_index, bool is_inat, u8 proto, __be32 dip,
					__be16 dport, __be32 router_pub_ip, __be16 router_port)
{
	int tab_no_start = is_inat ? INAT_NAPT_HASH1_NO : ENAT_NAPT_HASH1_NO;
	u16 *crc_key = is_inat ? info->inat_crc_key : info->enat_crc_key;
	int ret;

	sf_napt_crc_data(is_inat, proto, dip, dport, router_pub_ip, router_port, crc_key);

	ret = sf_hnat_napt_hash13_add(h, tab_no_start, crc_key[0], napt_index);
	if (ret >= 0)
		return ret;

	ret = sf_hnat_napt_hash2_add(h, tab_no_start + 1, crc_key[1], napt_index);
	if (ret >= 0)
		return ret + NAPT_HASH2_INDEX_OFFSET;

	ret = sf_hnat_napt_hash13_add(h, tab_no_start + 2, crc_key[2], napt_index);
	if (ret >= 0)
		return ret + NAPT_HASH3_INDEX_OFFSET;

	return -ENOSPC;
}

static void sf19a2890_hnat_napt_del_hash(struct sf19a2890_hnat *h, struct sf_napt_crc_info *info,
					 bool is_inat)
{
	int tab_no_start = is_inat ? INAT_NAPT_HASH1_NO : ENAT_NAPT_HASH1_NO;
	u16 *crc_key = is_inat ? info->inat_crc_key : info->enat_crc_key;
	u8 index = is_inat ? info->inat_ptr_index : info->enat_ptr_index;

	if (index < NAPT_HASH2_INDEX_OFFSET)
		sf_hnat_napt_hash13_del(h, tab_no_start, crc_key[0], index);
	else if (index < NAPT_HASH3_INDEX_OFFSET)
		sf_hnat_napt_hash2_del(h, tab_no_start + 1, crc_key[1],
				       index - NAPT_HASH2_INDEX_OFFSET);
	else
		sf_hnat_napt_hash13_del(h, tab_no_start + 2, crc_key[2],
					index - NAPT_HASH3_INDEX_OFFSET);
}

/* ----- DIP table: find-or-create by (ip, vlan_index), ref-counted ----- */

/* One valid bit per DIP row, 32 rows per DIP_VLD word; the engine
 * ignores a DIP row whose bit is clear (sf_hnat_dip_vld_cfg() in the
 * vendor driver).
 */
static int sf_hnat_dip_vld_cfg(struct sf19a2890_hnat *h, u32 dip_index, bool enable)
{
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };
	int ret;

	ret = hnat_table_read(h, DIP_VLD_NO, dip_index / 32, data);
	if (ret)
		return ret;
	if (enable)
		data[0] |= BIT(dip_index % 32);
	else
		data[0] &= ~BIT(dip_index % 32);
	return hnat_table_write(h, DIP_VLD_NO, dip_index / 32, data);
}

static int sf_hnat_dip_write_row(struct sf19a2890_hnat *h, u32 dip_index)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };
	struct sf_dip_entry *e = &h->dip_table[dip_index];

	data[0] = (__force u32)e->dip;
	data[1] = ((u32)e->vlan_index << 7) | e->dmac_index;
	return hnat_table_write(h, DIP_NO, dip_index, data);
}

static int sf_hnat_dip_hash1_add(struct sf19a2890_hnat *h, u32 hash_index, u32 dip_index)
{
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };
	int i, ret;

	ret = hnat_table_read(h, DIP_HASH1_NO, hash_index, data);
	if (ret)
		return ret;

	for (i = 0; i < 4; i++) {
		if (i == 2) /* RM#9392: skip way 2 in HASH1 */
			continue;
		if (!(data[1] & BIT(i + 4)))
			break;
	}
	if (i >= 4)
		return -ENOSPC;

	if (i < 2) {
		data[0] &= ~(0x1ff << (i * 9));
		data[0] |= dip_index << (i * 9);
		data[1] |= BIT(i + 4);
	} else { /* i == 3 */
		data[0] &= ~(0x1f << (i * 9));
		data[1] &= ~0xf;
		data[0] |= (dip_index & 0x1f) << (i * 9);
		data[1] |= BIT(i + 4) | ((dip_index & 0x1e0) >> 5);
	}
	ret = hnat_table_write(h, DIP_HASH1_NO, hash_index, data);
	return ret ? : i;
}

static int sf_hnat_dip_hash2_add(struct sf19a2890_hnat *h, u32 hash_index, u32 dip_index)
{
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };
	int i, ret;

	ret = hnat_table_read(h, DIP_HASH2_NO, hash_index, data);
	if (ret)
		return ret;

	for (i = 0; i < 4; i++)
		if (!(data[1] & BIT(i + 4)))
			break;
	if (i == 4)
		return -ENOSPC;

	if (i < 3) {
		data[0] &= ~(0x1ff << (i * 9));
		data[0] |= dip_index << (i * 9);
		data[1] |= BIT(i + 4);
	} else {
		data[0] &= ~(0x1f << (i * 9));
		data[1] &= ~0xf;
		data[0] |= (dip_index & 0x1f) << (i * 9);
		data[1] |= BIT(i + 4) | ((dip_index & 0x1e0) >> 5);
	}
	ret = hnat_table_write(h, DIP_HASH2_NO, hash_index, data);
	return ret ? : i;
}

static int sf19a2890_hnat_dip_add_hash(struct sf19a2890_hnat *h, struct sf_dip_crc_info *info,
				       u32 dip_index)
{
	u8 crc_data[5] = { 0 };
	u16 crc_value;
	int i, ret;

	/* network byte order, like the NAPT hash (see sf_napt_crc_data()) */
	memcpy(crc_data, &h->dip_table[dip_index].dip, 4);
	crc_data[4] = h->dip_table[dip_index].vlan_index;
	/* rcc_dip(): rotate the vlan-index byte's low bit up into the ip's
	 * top byte before hashing -- matches the vendor driver exactly.
	 */
	for (i = 3; i >= 0; i--) {
		crc_data[i + 1] = (crc_data[i + 1] & 0x7f) | ((crc_data[i] & 1) << 7);
		crc_data[i] >>= 1;
	}
	/* DIP hashing uses the raw crc1 return value with its OWN bit
	 * slices, deliberately different from the crc_key[0..2] triple
	 * sf_hnat_crc16() derives for NAPT -- matches sf_hnat_add_dip_crc_hash_entry()
	 * exactly (crc_key[0]=crc_value&0xff, crc_key[1]=(crc_value&0xfc00)>>10).
	 */
	crc_value = sf_hnat_crc16(crc_data, 5, NULL);
	info->crc_key[0] = crc_value & 0xff;
	info->crc_key[1] = (crc_value & 0xfc00) >> 10;

	ret = sf_hnat_dip_hash1_add(h, info->crc_key[0], dip_index);
	if (ret >= 0) {
		info->ptr_index = ret;
		return 0;
	}
	ret = sf_hnat_dip_hash2_add(h, info->crc_key[1], dip_index);
	if (ret >= 0) {
		info->ptr_index = ret + DIP_HASH2_INDEX_OFFSET;
		return 0;
	}
	return -ENOSPC;
}

static void sf19a2890_hnat_dip_del_hash(struct sf19a2890_hnat *h, struct sf_dip_crc_info *info)
{
	u32 data[HWNAT_TB_RDDATA_NO] = { 0 };

	if (info->ptr_index < DIP_HASH2_INDEX_OFFSET) {
		if (hnat_table_read(h, DIP_HASH1_NO, info->crc_key[0], data))
			return;
		data[1] &= ~BIT(info->ptr_index + 4);
		hnat_table_write(h, DIP_HASH1_NO, info->crc_key[0], data);
	} else {
		if (hnat_table_read(h, DIP_HASH2_NO, info->crc_key[1], data))
			return;
		data[1] &= ~BIT(info->ptr_index + 4 - DIP_HASH2_INDEX_OFFSET);
		hnat_table_write(h, DIP_HASH2_NO, info->crc_key[1], data);
	}
}

static int sf_hnat_search_router_mac(struct sf19a2890_hnat *h, const u8 *mac)
{
	int i;

	for (i = 0; i < ROUTER_MAC_TABLE_MAX; i++)
		if (h->rmac_table[i].valid && ether_addr_equal(h->rmac_table[i].rmac, mac))
			return i;
	return -1;
}

static void sf_hnat_router_mac_put(struct sf19a2890_hnat *h, int index)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	if (index < 0 || --h->rmac_table[index].ref_count > 0)
		return;
	h->rmac_table[index].valid = false;
	hnat_table_write(h, ROUTER_MAC_NO, index, data);
}

static int sf_hnat_router_mac_write(struct sf19a2890_hnat *h, int i)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	/* data[0] = bits 0-31, data[1] low 16 = bits 32-47 */
	memcpy(data, h->rmac_table[i].rmac, ETH_ALEN);
	return hnat_table_write(h, ROUTER_MAC_NO, i, data);
}

static int sf_hnat_router_mac_get(struct sf19a2890_hnat *h, const u8 *mac)
{
	int i, ret;

	i = sf_hnat_search_router_mac(h, mac);
	if (i >= 0) {
		h->rmac_table[i].ref_count++;
		return i;
	}

	for (i = 0; i < ROUTER_MAC_TABLE_MAX; i++)
		if (!h->rmac_table[i].valid)
			break;
	if (i == ROUTER_MAC_TABLE_MAX)
		return -ENOSPC;

	memcpy(h->rmac_table[i].rmac, mac, ETH_ALEN);
	ret = sf_hnat_router_mac_write(h, i);
	if (ret)
		return ret;
	h->rmac_table[i].valid = true;
	h->rmac_table[i].ref_count = 1;
	return i;
}

static int sf_hnat_search_dmac(struct sf19a2890_hnat *h, const u8 *mac, const u8 *router_mac)
{
	int i;

	for (i = 0; i < DMAC_TABLE_MAX; i++) {
		if (!h->dmac_table[i].valid || !ether_addr_equal(h->dmac_table[i].dmac, mac))
			continue;
		if (ether_addr_equal(h->rmac_table[h->dmac_table[i].router_mac_index].rmac,
				     router_mac))
			return i;
	}
	return -1;
}

static void sf_hnat_dmac_put(struct sf19a2890_hnat *h, int index)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	if (index < 0 || --h->dmac_table[index].ref_count > 0)
		return;
	sf_hnat_router_mac_put(h, h->dmac_table[index].router_mac_index);
	h->dmac_table[index].valid = false;
	hnat_table_write(h, DMAC_NO, index, data);
}

static int sf_hnat_dmac_write(struct sf19a2890_hnat *h, int i)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	memcpy(data, h->dmac_table[i].dmac, ETH_ALEN);
	data[1] |= (u32)h->dmac_table[i].router_mac_index << 16;
	return hnat_table_write(h, DMAC_NO, i, data);
}

static int sf_hnat_dmac_get(struct sf19a2890_hnat *h, const u8 *mac, const u8 *router_mac)
{
	int i, rmac_index, ret;

	i = sf_hnat_search_dmac(h, mac, router_mac);
	if (i >= 0) {
		h->dmac_table[i].ref_count++;
		return i;
	}

	for (i = 0; i < DMAC_TABLE_MAX; i++)
		if (!h->dmac_table[i].valid)
			break;
	if (i == DMAC_TABLE_MAX)
		return -ENOSPC;

	rmac_index = sf_hnat_router_mac_get(h, router_mac);
	if (rmac_index < 0)
		return rmac_index;

	memcpy(h->dmac_table[i].dmac, mac, ETH_ALEN);
	h->dmac_table[i].router_mac_index = rmac_index;
	ret = sf_hnat_dmac_write(h, i);
	if (ret) {
		sf_hnat_router_mac_put(h, rmac_index);
		return ret;
	}
	h->dmac_table[i].valid = true;
	h->dmac_table[i].ref_count = 1;
	return i;
}

static int sf_hnat_search_dip(struct sf19a2890_hnat *h, __be32 ip, u8 vlan_index)
{
	int i;

	for (i = 0; i < DIP_TABLE_MAX; i++)
		if (h->dip_table[i].valid && h->dip_table[i].dip == ip &&
		    h->dip_table[i].vlan_index == vlan_index)
			return i;
	return -1;
}

/* find-or-create a DIP entry mapping `ip` to the L2 pair (mac, router_mac)
 * (router_mac = our own interface's mac to use as source when sending to
 * this ip; mac = the L2 next-hop/destination mac for this ip). Returns
 * the dip_index, or a negative errno.
 */
static int sf19a2890_hnat_dip_get(struct sf19a2890_hnat *h, __be32 ip, u8 vlan_index, const u8 *mac,
				  const u8 *router_mac)
{
	int i, dmac_index, ret;

	i = sf_hnat_search_dip(h, ip, vlan_index);
	if (i >= 0) {
		struct sf_dmac_entry *d = &h->dmac_table[h->dip_table[i].dmac_index];

		/* next hop moved (ARP change, new PPPoE peer): repoint the
		 * existing entry rather than keep forwarding to a stale MAC
		 */
		if (!ether_addr_equal(d->dmac, mac) ||
		    !ether_addr_equal(h->rmac_table[d->router_mac_index].rmac, router_mac)) {
			dmac_index = sf_hnat_dmac_get(h, mac, router_mac);
			if (dmac_index < 0)
				return dmac_index;
			sf_hnat_dmac_put(h, h->dip_table[i].dmac_index);
			h->dip_table[i].dmac_index = dmac_index;
			sf_hnat_dip_write_row(h, i);
		}
		h->dip_table[i].ref_count++;
		return i;
	}

	for (i = 0; i < DIP_TABLE_MAX; i++)
		if (!h->dip_table[i].valid)
			break;
	if (i == DIP_TABLE_MAX)
		return -ENOSPC;

	dmac_index = sf_hnat_dmac_get(h, mac, router_mac);
	if (dmac_index < 0)
		return dmac_index;

	h->dip_table[i].dip = ip;
	h->dip_table[i].vlan_index = vlan_index;
	h->dip_table[i].dmac_index = dmac_index;

	ret = sf_hnat_dip_write_row(h, i);
	if (ret)
		goto err_dmac;
	ret = sf19a2890_hnat_dip_add_hash(h, &h->dip_crc[i], i);
	if (ret)
		goto err_row;
	sf_hnat_dip_vld_cfg(h, i, true);

	h->dip_table[i].valid = true;
	h->dip_table[i].ref_count = 1;
	return i;

err_row:
	h->dip_table[i].dip = 0;
	h->dip_table[i].dmac_index = 0;
	hnat_table_write(h, DIP_NO, i, hnat_zero_row);
err_dmac:
	sf_hnat_dmac_put(h, dmac_index);
	return ret;
}

static void sf_hnat_dip_del(struct sf19a2890_hnat *h, int index)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	if (index < 0 || --h->dip_table[index].ref_count > 0)
		return;
	sf_hnat_dip_vld_cfg(h, index, false);
	sf19a2890_hnat_dip_del_hash(h, &h->dip_crc[index]);
	sf_hnat_dmac_put(h, h->dip_table[index].dmac_index);
	hnat_table_write(h, DIP_NO, index, data);
	h->dip_table[index].valid = false;
}

/* ----- RT_PUB_NET: find-or-create by (router_pub_ip, wan vlan), ref-counted ----- */

static int sf_hnat_search_rt_pub_net(struct sf19a2890_hnat *h, __be32 router_pub_ip,
				     u8 pub_vlan_index)
{
	int i;

	for (i = 0; i < RT_PUB_NET_TABLE_MAX; i++)
		if (h->rt_pub_net_table[i].valid &&
		    h->rt_pub_net_table[i].router_pub_ip == router_pub_ip &&
		    h->rt_pub_net_table[i].pub_vlan_index == pub_vlan_index)
			return i;
	return -1;
}

static int sf_hnat_rt_pub_net_write(struct sf19a2890_hnat *h, int i)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	data[0] = (__force u32)h->rt_pub_net_table[i].router_pub_ip;
	data[1] = h->rt_pub_net_table[i].mask |
		  ((u32)h->rt_pub_net_table[i].pub_vlan_index << 5);
	return hnat_table_write(h, RT_PUB_NET_NO, i, data);
}

/* The vendor's default "auto judge" mode (TB_CONFIG USE_DIP clear)
 * looks an off-link ENAT destination up by router_pub_ip, expecting the
 * gateway's DIP entry there; on this engine that lookup never hits and
 * every off-link ENAT frame goes to the host. So the engine runs with
 * USE_DIP set and every ENAT destination gets its own DIP entry
 * carrying the next hop's MAC (see sf19a2890_hnat_commit_entry()). mask
 * is still programmed as the WAN prefix length (/32 PPPoE as 31, as
 * sf_hnat_add_rt_pub_net() does).
 */
static int sf19a2890_hnat_rt_pub_net_get(struct sf19a2890_hnat *h, __be32 router_pub_ip,
					 u8 pub_vlan_index, u8 mask)
{
	int i, ret;

	i = sf_hnat_search_rt_pub_net(h, router_pub_ip, pub_vlan_index);
	if (i >= 0) {
		if (h->rt_pub_net_table[i].mask != mask) {
			h->rt_pub_net_table[i].mask = mask;
			sf_hnat_rt_pub_net_write(h, i);
		}
		h->rt_pub_net_table[i].ref_count++;
		return i;
	}

	for (i = 0; i < RT_PUB_NET_TABLE_MAX; i++)
		if (!h->rt_pub_net_table[i].valid)
			break;
	if (i == RT_PUB_NET_TABLE_MAX)
		return -ENOSPC;

	h->rt_pub_net_table[i].router_pub_ip = router_pub_ip;
	h->rt_pub_net_table[i].pub_vlan_index = pub_vlan_index;
	h->rt_pub_net_table[i].mask = mask;
	ret = sf_hnat_rt_pub_net_write(h, i);
	if (ret)
		return ret;

	h->rt_pub_net_table[i].valid = true;
	h->rt_pub_net_table[i].ref_count = 1;
	return i;
}

static void sf_hnat_rt_pub_net_put(struct sf19a2890_hnat *h, int index)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	if (index < 0 || --h->rt_pub_net_table[index].ref_count > 0)
		return;
	h->rt_pub_net_table[index].valid = false;
	hnat_table_write(h, RT_PUB_NET_NO, index, data);
}

/* ----- VLAN_ID table: find-or-create by vid, ref-counted (sf_hnat_add_vlan()) ----- */

static int sf_hnat_vlan_write(struct sf19a2890_hnat *h, int i)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { h->vlan_table[i].vid };

	return hnat_table_write(h, VLAN_ID_NO, i, data);
}

static int sf_hnat_vlan_get(struct sf19a2890_hnat *h, u16 vid)
{
	int i, free = -1, ret;

	if (!vid)
		return SF_HNAT_NO_VLAN_INDEX;

	for (i = 0; i < SF_HNAT_NO_VLAN_INDEX; i++) {
		if (h->vlan_table[i].valid && h->vlan_table[i].vid == vid) {
			h->vlan_table[i].ref_count++;
			return i;
		}
		if (!h->vlan_table[i].valid && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	h->vlan_table[free].vid = vid;
	ret = sf_hnat_vlan_write(h, free);
	if (ret)
		return ret;

	h->vlan_table[free].valid = true;
	h->vlan_table[free].ref_count = 1;
	return free;
}

static void sf_hnat_vlan_put(struct sf19a2890_hnat *h, int index)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	if (index < 0 || index >= SF_HNAT_NO_VLAN_INDEX ||
	    --h->vlan_table[index].ref_count > 0)
		return;
	hnat_table_write(h, VLAN_ID_NO, index, data);
	h->vlan_table[index].valid = false;
}

/* ----- PPPOE_HD: find-or-create by session id, ref-counted (sf_hnat_add_ppphd()).
 * The engine builds the 8-byte PPPoE header from it on ENAT (ver/type 0x11,
 * code 0, the session id, PPP protocol 0x0021 = IPv4) and strips one on
 * INAT. -----
 */

static int sf_hnat_ppphd_write(struct sf19a2890_hnat *h, int i)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	data[0] = 0x00000021;
	data[1] = 0x11000000 | h->ppphd_table[i].sid;
	return hnat_table_write(h, PPPOE_HD_NO, i, data);
}

static int sf_hnat_ppphd_get(struct sf19a2890_hnat *h, u16 sid)
{
	int i, free = -1, ret;

	for (i = 0; i < PPPHD_TABLE_MAX; i++) {
		if (h->ppphd_table[i].valid && h->ppphd_table[i].sid == sid) {
			h->ppphd_table[i].ref_count++;
			return i;
		}
		if (!h->ppphd_table[i].valid && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	h->ppphd_table[free].sid = sid;
	ret = sf_hnat_ppphd_write(h, free);
	if (ret)
		return ret;
	h->ppphd_table[free].valid = true;
	h->ppphd_table[free].ref_count = 1;
	return free;
}

static void sf_hnat_ppphd_put(struct sf19a2890_hnat *h, int index)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };

	if (index < 0 || --h->ppphd_table[index].ref_count > 0)
		return;
	hnat_table_write(h, PPPOE_HD_NO, index, data);
	h->ppphd_table[index].valid = false;
}

/* prefix length of the local address `ip` (our WAN address), 32 if none */
static u8 sf_hnat_local_prefix(__be32 ip)
{
	struct net_device *dev;
	u8 plen = 32;

	rcu_read_lock();
	for_each_netdev_rcu(&init_net, dev) {
		struct in_device *in_dev = __in_dev_get_rcu(dev);
		const struct in_ifaddr *ifa;

		if (!in_dev)
			continue;
		in_dev_for_each_ifa_rcu(ifa, in_dev) {
			if (ifa->ifa_local == ip) {
				plen = ifa->ifa_prefixlen;
				goto out;
			}
		}
	}
out:
	rcu_read_unlock();
	return plen;
}

/* ----- LAN subnets (REG16-REG31): the engine classifies a frame as
 * INAT or ENAT by whether its source ip is inside one of these, so
 * nothing is translated until the LAN's subnet is programmed. The
 * vendor kernel fed this from a devinet.c hook on LAN address changes;
 * here it is derived from the local interface address covering each
 * offloaded LAN client. -----
 */

static void sf_hnat_lan_subnet_write(struct sf19a2890_hnat *h, int i)
{
	u32 mask_reg = i < 4 ? HWNAT_REG30_CSR : HWNAT_REG31_CSR;
	u32 shift = (i % 4) * 8;
	u32 mask = hnat_r(h, mask_reg) & ~(0xffu << shift);
	u32 r2;

	if (h->lan_subnets[i].valid)
		mask |= ((32 - h->lan_subnets[i].prefix_len) | 0x20) << shift;
	hnat_w(h, HWNAT_REG16_CSR + 4 * i, h->lan_subnets[i].valid ? h->lan_subnets[i].ip : 0);
	hnat_w(h, mask_reg, mask);

	r2 = hnat_r(h, HWNAT_REG2_CSR) & ~HWNAT_REG2_CSR_SUBNET_UPDATE;
	hnat_w(h, HWNAT_REG2_CSR, r2);
	hnat_w(h, HWNAT_REG2_CSR, r2 | HWNAT_REG2_CSR_SUBNET_UPDATE);
}

static void sf_hnat_lan_subnet_ensure(struct sf19a2890_hnat *h, __be32 client)
{
	struct net_device *dev;
	u32 ip = 0;
	u8 plen = 0;
	int i, free = -1;

	rcu_read_lock();
	for_each_netdev_rcu(&init_net, dev) {
		struct in_device *in_dev = __in_dev_get_rcu(dev);
		const struct in_ifaddr *ifa;

		if (!in_dev)
			continue;
		in_dev_for_each_ifa_rcu(ifa, in_dev) {
			if (ifa->ifa_prefixlen && ifa->ifa_prefixlen < 32 &&
			    inet_ifa_match(client, ifa)) {
				ip = ntohl(ifa->ifa_local);
				plen = ifa->ifa_prefixlen;
				goto found;
			}
		}
	}
found:
	rcu_read_unlock();
	if (!ip)
		return;

	for (i = 0; i < SUBNET_TABLE_MAX; i++) {
		if (h->lan_subnets[i].valid && h->lan_subnets[i].ip == ip &&
		    h->lan_subnets[i].prefix_len == plen)
			return;
		if (!h->lan_subnets[i].valid && free < 0)
			free = i;
	}
	if (free < 0)
		return;

	h->lan_subnets[free].ip = ip;
	h->lan_subnets[free].prefix_len = plen;
	h->lan_subnets[free].valid = true;
	sf_hnat_lan_subnet_write(h, free);
	dev_info(h->dev, "LAN subnet %d = %pI4h/%u\n", free, &ip, plen);
}

/* ----- shared per-connection NAPT resource, keyed by the "public-side"
 * tuple that both directions of a masqueraded flow present identically
 * (remote ip/port + our own WAN ip/port) -- see sf19a2890_hnat_commit_entry()
 * for why a single direction's flow_rule mangle actions already reveal
 * this whole tuple. -----
 */
struct sf19a2890_hnat_pubkey {
	__be32 dip;
	__be32 router_pub_ip;
	__be16 dport;
	__be16 router_port;
	u8 proto;
} __packed;

struct sf19a2890_hnat_napt {
	struct rhash_head node;
	struct sf19a2890_hnat_pubkey key;
	__be32 sip;
	__be16 sport;
	int napt_index;
	int rt_pub_net_index;
	int lan_vlan_index;
	int wan_vlan_index;
	int src_dip_index;	/* private side: sip -> client's own L2 pair */
	int dest_dip_index;	/* public side: dip -> gateway/next-hop L2 pair */
	struct sf_napt_crc_info crc;
	u8 refcount;		/* directions currently attached: 1 or 2 */
	bool have_enat, have_inat;
	bool pppoe;		/* WAN side is a PPPoE session (ppphd_index) */
	int ppphd_index;
};

static int sf_hnat_napt_write_row(struct sf19a2890_hnat *h, const struct sf19a2890_hnat_napt *n)
{
	u32 data[HWNAT_TB_WRDATA_NO] = { 0 };
	u32 sip = (__force u32)n->sip;

	data[0] = (__force u32)n->key.dip;
	data[1] = (sip & 0xFFFF) << 16 | (__force u16)n->key.dport;
	data[2] = (__force u16)n->sport << 16 | ((sip >> 16) & 0xFFFF);
	/* bits 0-3 rt_pub_net index, 4-19 router port, 20-26 LAN vlan index,
	 * 27 protocol (the engine's code: 0 = TCP, 1 = UDP), 28 INAT to host,
	 * 29 PPPoE, 30-31 + data[4] bit 0 PPPOE_HD index
	 */
	data[3] = ((u32)(n->key.proto == IPPROTO_UDP) << 27) |
		  ((u32)n->lan_vlan_index << 20) |
		  ((u32)(__force u16)n->key.router_port << 4) | n->rt_pub_net_index;
	if (n->pppoe) {
		data[3] |= ((u32)(n->ppphd_index & 0x3) << 30) | BIT(29);
		data[4] = (n->ppphd_index & 0x4) >> 2;
	}

	return hnat_table_write(h, NAPT_NO, n->napt_index, data);
}

/* place one direction's hash pointer: ENAT keys on the private tuple the
 * LAN frame carries (sport/sip/dport/dip), INAT on the public one
 * (dport/dip/router_port/router_pub_ip), as in sf_napt_key_crc_get()
 */
static int sf_hnat_napt_hash_add(struct sf19a2890_hnat *h, struct sf19a2890_hnat_napt *n,
				 bool is_inat)
{
	int ret;

	if (is_inat)
		ret = sf19a2890_hnat_napt_add_hash(h, &n->crc, n->napt_index, true,
						   n->key.proto, n->key.dip, n->key.dport,
						   n->key.router_pub_ip, n->key.router_port);
	else
		ret = sf19a2890_hnat_napt_add_hash(h, &n->crc, n->napt_index, false,
						   n->key.proto, n->sip, n->sport,
						   n->key.dip, n->key.dport);
	if (ret < 0)
		return ret;
	if (is_inat)
		n->crc.inat_ptr_index = ret;
	else
		n->crc.enat_ptr_index = ret;
	return 0;
}

static const struct rhashtable_params sf19a2890_hnat_napt_ht_params = {
	.head_offset = offsetof(struct sf19a2890_hnat_napt, node),
	.key_offset = offsetof(struct sf19a2890_hnat_napt, key),
	.key_len = sizeof(struct sf19a2890_hnat_pubkey),
	.automatic_shrinking = true,
};

struct sf19a2890_hnat_entry {
	struct rhash_head node;
	unsigned long cookie;
	struct sf19a2890_hnat_napt *napt;
	bool is_inat;
};

static const struct rhashtable_params sf19a2890_hnat_ht_params = {
	.head_offset = offsetof(struct sf19a2890_hnat_entry, node),
	.key_offset = offsetof(struct sf19a2890_hnat_entry, cookie),
	.key_len = sizeof(unsigned long),
	.automatic_shrinking = true,
};

static void sf19a2890_hnat_mangle_eth(const struct flow_action_entry *act, void *eth)
{
	void *dest = eth + act->mangle.offset;
	const void *src = &act->mangle.val;

	if (act->mangle.offset > 8)
		return;

	if (act->mangle.mask == 0xffff) {
		src += 2;
		dest += 2;
	}

	memcpy(dest, src, act->mangle.mask ? 2 : 4);
}

static int sf19a2890_hnat_mangle_ports(const struct flow_action_entry *act,
				       struct sf19a2890_hnat_flow_data *data)
{
	u32 val = ntohl(act->mangle.val);

	switch (act->mangle.offset) {
	case 0:
		if (act->mangle.mask == ~htonl(0xffff))
			data->dst_port = cpu_to_be16(val);
		else
			data->src_port = cpu_to_be16(val >> 16);
		break;
	case 2:
		data->dst_port = cpu_to_be16(val);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sf19a2890_hnat_mangle_ipv4(const struct flow_action_entry *act,
				      struct sf19a2890_hnat_flow_data *data)
{
	__be32 *dest;

	switch (act->mangle.offset) {
	case offsetof(struct iphdr, saddr):
		dest = &data->src_ip;
		break;
	case offsetof(struct iphdr, daddr):
		dest = &data->dst_ip;
		break;
	default:
		return -EINVAL;
	}

	memcpy(dest, &act->mangle.val, sizeof(u32));

	return 0;
}

static int sf19a2890_hnat_napt_alloc(struct sf19a2890_hnat *h)
{
	int i;

	for (i = 0; i < NAPT_TABLE_MAX; i++)
		if (!h->napt_slots[i])
			return i;
	return -ENOSPC;
}

/* Each direction of a masqueraded flow arrives as an independent
 * FLOW_CLS_REPLACE (nf_flow_offload_tuple(), one per
 * FLOW_OFFLOAD_DIR_ORIGINAL/REPLY), with nothing linking the pair. The
 * engine's single-row NAPT design needs both directions' hash chains to
 * point at the same napt_index (see sf19a2890_hnat_napt_add_hash()). So
 * a second lookup is keyed on the "public-side" tuple (remote ip:port +
 * our WAN ip:port), which is identical in both directions' match/mangle
 * data for a plain SNAT/masquerade flow.
 *
 * Scope: exactly one of (src ip/port) or (dst ip/port) may be mangled,
 * i.e. plain SNAT/masquerade only: no DNAT/port forwarding, no
 * simultaneous src+dst rewrite. Anything else returns -EOPNOTSUPP.
 */
static int sf19a2890_hnat_commit_entry(struct sf19a2890_hnat *h, struct sf19a2890_hnat_entry *entry,
				       const struct sf19a2890_hnat_flow_data *data)
{
	bool src_mangled = data->src_ip != data->orig.src_ip ||
			   data->src_port != data->orig.src_port;
	bool dst_mangled = data->dst_ip != data->orig.dst_ip ||
			   data->dst_port != data->orig.dst_port;
	struct sf19a2890_hnat_pubkey key = {};
	struct sf19a2890_hnat_napt *napt;
	bool is_inat;
	__be32 sip, dip, router_pub_ip;
	__be16 sport, dport, router_port;
	int ret, dip_index;
	u8 rt_mask;

	if (src_mangled == dst_mangled)
		return -EOPNOTSUPP; /* both or neither -- not plain masquerade */

	is_inat = dst_mangled;
	if (!is_inat) {
		/* ENAT-shaped: LAN client's own send, SNAT'd on the way out. */
		sip = data->orig.src_ip;
		sport = data->orig.src_port;
		dip = data->orig.dst_ip;
		dport = data->orig.dst_port;
		router_pub_ip = data->src_ip;
		router_port = data->src_port;
	} else {
		/* INAT-shaped: reply arriving from the remote host, un-SNAT'd
		 * back to the LAN client on the way in.
		 */
		dip = data->orig.src_ip;
		dport = data->orig.src_port;
		router_pub_ip = data->orig.dst_ip;
		router_port = data->orig.dst_port;
		sip = data->dst_ip;
		sport = data->dst_port;
	}

	key.dip = dip;
	key.router_pub_ip = router_pub_ip;
	key.dport = dport;
	key.router_port = router_port;
	key.proto = data->proto;

	sf_hnat_lan_subnet_ensure(h, sip);

	rt_mask = sf_hnat_local_prefix(router_pub_ip);
	if (rt_mask >= 32)
		rt_mask = 31;

	spin_lock_bh(&h->lock);

	napt = rhashtable_lookup_fast(&h->napt_table, &key,
				      sf19a2890_hnat_napt_ht_params);
	if (napt && ((is_inat && napt->have_inat) || (!is_inat && napt->have_enat))) {
		ret = -EEXIST;
		goto out_unlock;
	}

	if (!napt) {
		napt = kzalloc(sizeof(*napt), GFP_ATOMIC);
		if (!napt) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		napt->key = key;
		napt->sip = sip;
		napt->sport = sport;
		napt->src_dip_index = -1;
		napt->dest_dip_index = -1;
		napt->ppphd_index = -1;

		napt->napt_index = sf19a2890_hnat_napt_alloc(h);
		if (napt->napt_index < 0) {
			ret = napt->napt_index;
			goto err_free_napt;
		}
		/* either direction carries both VLANs: ingress tag + pushed tag */
		napt->lan_vlan_index = sf_hnat_vlan_get(h, is_inat ? data->out_vid : data->in_vid);
		if (napt->lan_vlan_index < 0) {
			ret = napt->lan_vlan_index;
			goto err_free_napt;
		}
		napt->wan_vlan_index = sf_hnat_vlan_get(h, is_inat ? data->in_vid : data->out_vid);
		if (napt->wan_vlan_index < 0) {
			ret = napt->wan_vlan_index;
			goto err_lan_vlan;
		}
		napt->rt_pub_net_index = sf19a2890_hnat_rt_pub_net_get(h, router_pub_ip,
								       napt->wan_vlan_index,
								       rt_mask);
		if (napt->rt_pub_net_index < 0) {
			ret = napt->rt_pub_net_index;
			goto err_wan_vlan;
		}
		if (data->pppoe) {
			napt->ppphd_index = sf_hnat_ppphd_get(h, data->pppoe_sid);
			if (napt->ppphd_index < 0) {
				ret = napt->ppphd_index;
				goto err_rt_pub_net;
			}
			napt->pppoe = true;
		}
		ret = sf_hnat_napt_write_row(h, napt);
		if (ret)
			goto err_ppphd;
		ret = sf_hnat_napt_vld_cfg(h, napt->napt_index, true);
		if (ret)
			goto err_napt_row;

		ret = rhashtable_insert_fast(&h->napt_table, &napt->node,
					     sf19a2890_hnat_napt_ht_params);
		if (ret)
			goto err_napt_vld;

		h->napt_slots[napt->napt_index] = napt;
	} else if (data->pppoe && !napt->pppoe) {
		/* the INAT direction carries no PPPoE information; when it
		 * created the row, add the session now that ENAT shows it
		 */
		ret = sf_hnat_ppphd_get(h, data->pppoe_sid);
		if (ret < 0)
			goto out_unlock;
		napt->ppphd_index = ret;
		napt->pppoe = true;
		sf_hnat_napt_write_row(h, napt);
	}

	/* DIP entry for THIS direction's own L2 pair: ENAT reveals the
	 * public/gateway side (dest_dip); INAT reveals the private/client
	 * side (src_dip). Whichever direction arrives second fills in
	 * whatever the first one couldn't. With USE_DIP the engine always
	 * looks the ENAT destination itself up, so an off-link destination's
	 * entry carries the gateway's MAC (the flow's h_dest).
	 */
	dip_index = sf19a2890_hnat_dip_get(h, is_inat ? sip : dip,
					   is_inat ? napt->lan_vlan_index : napt->wan_vlan_index,
					   data->eth.h_dest, data->eth.h_source);
	if (dip_index < 0) {
		ret = dip_index;
		if (hnat_debug)
			dev_info(h->dev, "  dip_get failed: %d (inat %d)\n", ret, is_inat);
		goto err_maybe_free_napt;
	}
	if (is_inat)
		napt->src_dip_index = dip_index;
	else
		napt->dest_dip_index = dip_index;

	ret = sf_hnat_napt_hash_add(h, napt, is_inat);
	if (ret < 0)
		goto err_dip;

	if (is_inat)
		napt->have_inat = true;
	else
		napt->have_enat = true;
	napt->refcount++;

	entry->napt = napt;
	entry->is_inat = is_inat;
	spin_unlock_bh(&h->lock);
	return 0;

err_dip:
	if (hnat_debug)
		dev_info(h->dev, "  napt hash add failed (inat %d)\n", is_inat);
	sf_hnat_dip_del(h, dip_index);
	if (is_inat)
		napt->src_dip_index = -1;
	else
		napt->dest_dip_index = -1;
err_maybe_free_napt:
	if (napt->refcount > 0)
		goto out_unlock; /* sibling direction already live, leave it alone */
	rhashtable_remove_fast(&h->napt_table, &napt->node,
			       sf19a2890_hnat_napt_ht_params);
	h->napt_slots[napt->napt_index] = NULL;
err_napt_vld:
	sf_hnat_napt_vld_cfg(h, napt->napt_index, false);
err_napt_row:
	sf_hnat_napt_clear_row(h, napt->napt_index);
err_ppphd:
	sf_hnat_ppphd_put(h, napt->ppphd_index);
err_rt_pub_net:
	sf_hnat_rt_pub_net_put(h, napt->rt_pub_net_index);
err_wan_vlan:
	sf_hnat_vlan_put(h, napt->wan_vlan_index);
err_lan_vlan:
	sf_hnat_vlan_put(h, napt->lan_vlan_index);
err_free_napt:
	kfree(napt);
out_unlock:
	spin_unlock_bh(&h->lock);
	return ret;
}

static void sf19a2890_hnat_clear_entry(struct sf19a2890_hnat *h, struct sf19a2890_hnat_entry *entry)
{
	struct sf19a2890_hnat_napt *napt = entry->napt;

	spin_lock_bh(&h->lock);

	sf19a2890_hnat_napt_del_hash(h, &napt->crc, entry->is_inat);
	if (entry->is_inat) {
		sf_hnat_dip_del(h, napt->src_dip_index);
		napt->src_dip_index = -1;
		napt->have_inat = false;
	} else {
		sf_hnat_dip_del(h, napt->dest_dip_index);
		napt->dest_dip_index = -1;
		napt->have_enat = false;
	}

	if (--napt->refcount == 0) {
		rhashtable_remove_fast(&h->napt_table, &napt->node,
				       sf19a2890_hnat_napt_ht_params);
		h->napt_slots[napt->napt_index] = NULL;
		sf_hnat_napt_vld_cfg(h, napt->napt_index, false);
		sf_hnat_napt_clear_row(h, napt->napt_index);
		sf_hnat_rt_pub_net_put(h, napt->rt_pub_net_index);
		sf_hnat_ppphd_put(h, napt->ppphd_index);
		sf_hnat_vlan_put(h, napt->wan_vlan_index);
		sf_hnat_vlan_put(h, napt->lan_vlan_index);
		kfree(napt);
	}

	spin_unlock_bh(&h->lock);
}

/*
 * With the switch under DSA, flow rules name the DSA user ports (lan*,
 * wan) as ingress/egress and carry no VLAN information: path discovery
 * stops at the user port. The engine only parses 802.1Q, which it gets
 * from the mtk-8021q tagger, so translate each user port into the VID its
 * frames carry between the switch and this GMAC: the port's tag_8021q
 * standalone VID, or its bridge's VID under a VLAN-unaware bridge.
 */
static int sf19a2890_hnat_dsa_vid(struct net_device *dev,
				  const struct net_device *ndev, u16 *vid)
{
	struct dsa_port *dp;

	if (!dev || !dsa_user_dev_check(dev))
		return -EOPNOTSUPP;
	dp = dsa_port_from_netdev(dev);
	if (IS_ERR(dp) || dsa_port_to_conduit(dp) != ndev)
		return -EOPNOTSUPP;
	if (dp->cpu_dp->tag_ops->proto != DSA_TAG_PROTO_MTK_8021Q)
		return -EOPNOTSUPP;	/* MediaTek tag: engine can't parse it */

	if (dsa_port_bridge_dev_get(dp)) {
		if (dsa_port_is_vlan_filtering(dp))
			return -EOPNOTSUPP;
		*vid = dsa_tag_8021q_bridge_vid(dsa_port_bridge_num_get(dp));
	} else {
		*vid = dsa_tag_8021q_standalone_vid(dp);
	}
	return 0;
}

static int sf19a2890_hnat_flow_replace(struct sf19a2890_hnat *h, struct flow_cls_offload *f,
				       struct net_device *ndev)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct sf19a2890_hnat_flow_data data = {};
	struct sf19a2890_hnat_entry *entry;
	struct flow_action_entry *act;
	u16 addr_type = 0;
	int err, i;

	if (rhashtable_lookup(&h->flow_table, &f->cookie,
			      sf19a2890_hnat_ht_params))
		return -EEXIST;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL))
		return -EOPNOTSUPP;

	{
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
		if (flow_rule_has_control_flags(match.mask->flags, f->common.extack))
			return -EOPNOTSUPP;
	}

	/* IPv4 NAPT only -- no bridge, no IPv6 (the engine has none), no QinQ. */
	if (addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS)
		return -EOPNOTSUPP;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return -EOPNOTSUPP;

	{
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		if (match.key->ip_proto != IPPROTO_TCP &&
		    match.key->ip_proto != IPPROTO_UDP)
			return -EOPNOTSUPP;
		data.proto = match.key->ip_proto;
	}

	/* nf_flow_table's match dissector carries no
	 * FLOW_DISSECTOR_KEY_ETH_ADDRS: the L2 addresses only arrive as
	 * FLOW_ACTION_MANGLE(HDR_TYPE_ETH) actions, handled by
	 * sf19a2890_hnat_mangle_eth() below. data is zero-initialized above.
	 */

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN))
		return -EOPNOTSUPP;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		if (match.key->vlan_tpid != htons(ETH_P_8021Q))
			return -EOPNOTSUPP;
		data.in_vid = match.key->vlan_id;
	}

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS))
		return -EOPNOTSUPP;

	{
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);
		data.src_ip = match.key->src;
		data.orig.src_ip = match.key->src;
		data.dst_ip = match.key->dst;
		data.orig.dst_ip = match.key->dst;
	}

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS))
		return -EOPNOTSUPP;

	{
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);
		data.src_port = match.key->src;
		data.orig.src_port = match.key->src;
		data.dst_port = match.key->dst;
		data.orig.dst_port = match.key->dst;
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				sf19a2890_hnat_mangle_eth(act, &data.eth);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
				err = sf19a2890_hnat_mangle_ports(act, &data);
				if (err)
					return err;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				err = sf19a2890_hnat_mangle_ipv4(act, &data);
				if (err)
					return err;
				break;
			default:
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_REDIRECT:
			data.odev = act->dev;
			break;
		case FLOW_ACTION_VLAN_POP:
			/* the engine re-tags from the DIP entry's vlan index */
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (data.out_vid || act->vlan.proto != htons(ETH_P_8021Q))
				return -EOPNOTSUPP;
			data.out_vid = act->vlan.vid;
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			if (data.pppoe)
				return -EOPNOTSUPP;
			data.pppoe = true;
			data.pppoe_sid = act->pppoe.sid;
			break;
		case FLOW_ACTION_CSUM:
			/* checksum recompute is implicit in the hardware
			 * rewrite path, nothing to record here.
			 */
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (!is_valid_ether_addr(data.eth.h_source) ||
	    !is_valid_ether_addr(data.eth.h_dest))
		return -EINVAL;

	if (hnat_debug) {
		dev_info(h->dev,
			 "replace keys %#llx %pI4:%u->%pI4:%u nat %pI4:%u->%pI4:%u vid %u->%u odev %s\n",
			 rule->match.dissector->used_keys, &data.orig.src_ip,
			 ntohs(data.orig.src_port), &data.orig.dst_ip, ntohs(data.orig.dst_port),
			 &data.src_ip, ntohs(data.src_port), &data.dst_ip, ntohs(data.dst_port),
			 data.in_vid, data.out_vid, data.odev ? data.odev->name : "-");
		flow_action_for_each(i, act, &rule->action)
			dev_info(h->dev, "  action %d\n", act->id);
	}

	if (data.odev && dsa_user_dev_check(data.odev)) {
		struct flow_match_meta meta;
		struct net_device *idev;

		/* no stacked VLANs on top of DSA user ports */
		if (data.in_vid || data.out_vid ||
		    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META))
			return -EOPNOTSUPP;
		flow_rule_match_meta(rule, &meta);

		idev = dev_get_by_index(dev_net(ndev), meta.key->ingress_ifindex);
		err = sf19a2890_hnat_dsa_vid(idev, ndev, &data.in_vid);
		dev_put(idev);
		if (err)
			return err;
		err = sf19a2890_hnat_dsa_vid(data.odev, ndev, &data.out_vid);
		if (err)
			return err;
		data.odev = ndev;	/* the engine transmits through the conduit */
	}

	if (hnat_debug)
		dev_info(h->dev, "  wire vid %u->%u\n", data.in_vid, data.out_vid);

	/* the engine can only send the rewritten frame back out of this
	 * GMAC; flows leaving through anything else (WiFi) stay in software
	 */
	if (data.odev != ndev)
		return -EOPNOTSUPP;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->cookie = f->cookie;

	err = sf19a2890_hnat_commit_entry(h, entry, &data);
	if (hnat_debug && err)
		dev_info(h->dev, "  commit failed: %d\n", err);
	if (err)
		goto free;

	err = rhashtable_insert_fast(&h->flow_table, &entry->node,
				     sf19a2890_hnat_ht_params);
	if (err)
		goto clear;

	return 0;

clear:
	sf19a2890_hnat_clear_entry(h, entry);
free:
	kfree(entry);
	return err;
}

static int sf19a2890_hnat_flow_destroy(struct sf19a2890_hnat *h, struct flow_cls_offload *f)
{
	struct sf19a2890_hnat_entry *entry;

	entry = rhashtable_lookup(&h->flow_table, &f->cookie,
				  sf19a2890_hnat_ht_params);
	if (!entry)
		return -ENOENT;

	sf19a2890_hnat_clear_entry(h, entry);
	rhashtable_remove_fast(&h->flow_table, &entry->node,
			       sf19a2890_hnat_ht_params);
	kfree(entry);

	return 0;
}

static int sf19a2890_hnat_flow_cmd(struct sf19a2890_hnat *h, struct flow_cls_offload *f,
				   struct net_device *ndev)
{
	int err;

	mutex_lock(&h->flow_mutex);
	switch (f->command) {
	case FLOW_CLS_REPLACE:
		err = sf19a2890_hnat_flow_replace(h, f, ndev);
		break;
	case FLOW_CLS_DESTROY:
		err = sf19a2890_hnat_flow_destroy(h, f);
		break;
	case FLOW_CLS_STATS:
		/* The engine has no per-flow hit counters or timestamps. */
		err = -EOPNOTSUPP;
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&h->flow_mutex);

	return err;
}

static void sf19a2890_hnat_csr_init(struct sf19a2890_hnat *h);

/* stmmac_open()'s DMA soft reset returns every engine CSR to its reset
 * default (TB_CONFIG reads back 0x4) and empties the tables
 */
static bool sf19a2890_hnat_engine_wiped(struct sf19a2890_hnat *h)
{
	return !(hnat_r(h, HWNAT_REG2_TB_CONFIG) & HWNAT_REG2_TB_CONFIG_EN_BIG_ENDIAN);
}

/*
 * Re-enable the engine and rewrite every table from the software state:
 * the flows nf_flow_table still holds as offloaded keep forwarding in
 * hardware instead of silently falling back to the host until they age
 * out. Caller holds h->flow_mutex.
 */
static void sf19a2890_hnat_restore(struct sf19a2890_hnat *h)
{
	int i, napts = 0;

	spin_lock_bh(&h->lock);

	sf19a2890_hnat_csr_init(h); /* also re-commits the LAN subnets */

	for (i = 0; i < VLAN_TABLE_MAX; i++)
		if (h->vlan_table[i].valid)
			sf_hnat_vlan_write(h, i);
	for (i = 0; i < ROUTER_MAC_TABLE_MAX; i++)
		if (h->rmac_table[i].valid)
			sf_hnat_router_mac_write(h, i);
	for (i = 0; i < DMAC_TABLE_MAX; i++)
		if (h->dmac_table[i].valid)
			sf_hnat_dmac_write(h, i);
	for (i = 0; i < RT_PUB_NET_TABLE_MAX; i++)
		if (h->rt_pub_net_table[i].valid)
			sf_hnat_rt_pub_net_write(h, i);
	for (i = 0; i < PPPHD_TABLE_MAX; i++)
		if (h->ppphd_table[i].valid)
			sf_hnat_ppphd_write(h, i);
	for (i = 0; i < DIP_TABLE_MAX; i++) {
		if (!h->dip_table[i].valid)
			continue;
		sf_hnat_dip_write_row(h, i);
		sf19a2890_hnat_dip_add_hash(h, &h->dip_crc[i], i);
		sf_hnat_dip_vld_cfg(h, i, true);
	}
	for (i = 0; i < NAPT_TABLE_MAX; i++) {
		struct sf19a2890_hnat_napt *n = h->napt_slots[i];

		if (!n)
			continue;
		sf_hnat_napt_write_row(h, n);
		sf_hnat_napt_vld_cfg(h, i, true);
		if (n->have_enat)
			sf_hnat_napt_hash_add(h, n, false);
		if (n->have_inat)
			sf_hnat_napt_hash_add(h, n, true);
		napts++;
	}

	spin_unlock_bh(&h->lock);
	dev_info(h->dev, "engine was reset, restored %d flows\n", napts);
}

void sf19a2890_hnat_hw_setup_done(struct sf19a2890_hnat *h)
{
	mutex_lock(&h->flow_mutex);
	if (h->csr_ready && sf19a2890_hnat_engine_wiped(h))
		sf19a2890_hnat_restore(h);
	mutex_unlock(&h->flow_mutex);
}
EXPORT_SYMBOL_GPL(sf19a2890_hnat_hw_setup_done);

static int sf19a2890_hnat_setup_tc_block_cb(enum tc_setup_type type,
					    void *type_data, void *cb_priv)
{
	struct sf19a2890_hnat *h = cb_priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	return sf19a2890_hnat_flow_cmd(h, type_data, h->ndev);
}

int sf19a2890_hnat_setup_tc_ft(struct sf19a2890_hnat *h,
			       struct net_device *ndev, void *type_data)
{
	struct flow_block_offload *f = type_data;
	struct flow_block_cb *block_cb;
	flow_setup_cb_t *cb = sf19a2890_hnat_setup_tc_block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &h->block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, cb, h);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		h->ndev = ndev;
		block_cb = flow_block_cb_alloc(cb, h, h, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		/* The engine is programmed on the first bind, not at probe: stmmac's
		 * reset in stmmac_open() returns REG2_TB_CONFIG to its power-on
		 * default.
		 *
		 * Only once: userspace recreates the flowtable on every firewall
		 * reload, so binds repeat, and REG18's flush pulse on a live engine
		 * with table writes in flight wedges it. Later binds only restore the
		 * tables if a GMAC reset wiped them. h->flow_mutex
		 * serialises this against flow commits.
		 */
		mutex_lock(&h->flow_mutex);
		if (!h->csr_ready) {
			sf19a2890_hnat_csr_init(h);
			h->csr_ready = true;
		} else if (sf19a2890_hnat_engine_wiped(h)) {
			sf19a2890_hnat_restore(h);
		}
		mutex_unlock(&h->flow_mutex);

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &h->block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, cb, h);
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
EXPORT_SYMBOL_GPL(sf19a2890_hnat_setup_tc_ft);

/* Ask the engine to look a 5-tuple up in the ENAT tables, as the vendor
 * driver's sf_hnat_cam_lookup() does: wdata0-2 in NAPT-row encoding,
 * wdata3 = is_udp, opcode 2. Returns the raw status (bit 1 = hit) and
 * RDDATA5 through *res.
 */
static u32 hnat_cam_lookup(struct sf19a2890_hnat *h, const u32 row[3], bool udp, u32 *res)
{
	u32 st;

	hnat_wait_idle(h);
	hnat_w(h, HWNAT_REG5_TB_WRDATA0, row[0]);
	hnat_w(h, HWNAT_REG6_TB_WRDATA1, row[1]);
	hnat_w(h, HWNAT_REG7_TB_WRDATA2, row[2]);
	hnat_w(h, HWNAT_REG8_TB_WRDATA3, udp);
	hnat_w(h, HWNAT_REG4_TB_OP_CODE, 2);
	hnat_wait_idle(h);
	st = hnat_r(h, HWNAT_REG1_TB_STATUS);
	*res = hnat_r(h, HWNAT_REG15_TB_RDDATA5);
	return st;
}

/* software view of the programmed tables plus a hardware readback */
static int hnat_tables_show(struct seq_file *s, void *unused)
{
	struct sf19a2890_hnat *h = s->private;
	u32 d[HWNAT_TB_RDDATA_NO];
	int i;

	mutex_lock(&h->flow_mutex);
	spin_lock_bh(&h->lock);
	for (i = 0; i < VLAN_TABLE_MAX; i++)
		if (h->vlan_table[i].valid)
			seq_printf(s, "vlan[%d] vid %u ref %u\n", i,
				   h->vlan_table[i].vid, h->vlan_table[i].ref_count);
	for (i = 0; i < RT_PUB_NET_TABLE_MAX; i++)
		if (h->rt_pub_net_table[i].valid)
			seq_printf(s, "rt[%d] %pI4/%u vlan %u ref %u\n", i,
				   &h->rt_pub_net_table[i].router_pub_ip,
				   h->rt_pub_net_table[i].mask,
				   h->rt_pub_net_table[i].pub_vlan_index,
				   h->rt_pub_net_table[i].ref_count);
	for (i = 0; i < PPPHD_TABLE_MAX; i++)
		if (h->ppphd_table[i].valid)
			seq_printf(s, "ppphd[%d] sid 0x%04x ref %u\n", i,
				   h->ppphd_table[i].sid, h->ppphd_table[i].ref_count);
	for (i = 0; i < DIP_TABLE_MAX; i++)
		if (h->dip_table[i].valid) {
			hnat_table_read(h, DIP_NO, i, d);
			seq_printf(s, "dip[%d] %pI4 vlan %u dmac %u ref %u hw %08x %08x\n",
				   i, &h->dip_table[i].dip, h->dip_table[i].vlan_index,
				   h->dip_table[i].dmac_index, h->dip_table[i].ref_count,
				   d[0], d[1]);
		}
	for (i = 0; i < NAPT_TABLE_MAX; i++) {
		struct sf19a2890_hnat_napt *n = h->napt_slots[i];

		if (!n)
			continue;
		hnat_table_read(h, NAPT_NO, i, d);
		seq_printf(s, "napt[%d] %pI4:%u -> %pI4:%u via %pI4:%u p%u lanv %d wanv %d rt %d ppphd %d dip s%d/d%d enat %d(%u) inat %d(%u) hw %08x %08x %08x %08x\n",
			   i, &n->sip, ntohs(n->sport), &n->key.dip, ntohs(n->key.dport),
			   &n->key.router_pub_ip, ntohs(n->key.router_port), n->key.proto,
			   n->lan_vlan_index, n->wan_vlan_index, n->rt_pub_net_index,
			   n->pppoe ? n->ppphd_index : -1,
			   n->src_dip_index, n->dest_dip_index,
			   n->have_enat, n->crc.enat_ptr_index,
			   n->have_inat, n->crc.inat_ptr_index, d[0], d[1], d[2], d[3]);
		{
			u32 st, res;

			st = hnat_cam_lookup(h, d, n->key.proto == IPPROTO_UDP, &res);
			seq_printf(s, "  cam_lookup(enat) status %08x rd5 %08x\n", st, res);
		}
	}
	spin_unlock_bh(&h->lock);
	mutex_unlock(&h->flow_mutex);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hnat_tables);

/*
 * The engine's table RAM is not cleared by reset: hash buckets and valid
 * bitmaps come up holding garbage with "way in use" bits set, so inserts
 * fail with -ENOSPC and stale pointers could match. Zero every hash and
 * valid-bit table (the rows themselves are only reachable through them).
 */
static void sf_hnat_tables_clear(struct sf19a2890_hnat *h)
{
	static const struct { u8 tab; u16 depth; } tabs[] = {
		{ INAT_NAPT_HASH1_NO, 512 }, { INAT_NAPT_HASH2_NO, 256 },
		{ INAT_NAPT_HASH3_NO, 32 },  { ENAT_NAPT_HASH1_NO, 512 },
		{ ENAT_NAPT_HASH2_NO, 256 }, { ENAT_NAPT_HASH3_NO, 32 },
		{ NAPT_VLD_NO, NAPT_TABLE_MAX / 32 },
		{ DIP_HASH1_NO, 256 }, { DIP_HASH2_NO, 64 },
		{ DIP_VLD_NO, DIP_TABLE_MAX / 32 },
	};
	u32 zero[HWNAT_TB_WRDATA_NO] = { 0 };
	int t, i;

	for (t = 0; t < ARRAY_SIZE(tabs); t++)
		for (i = 0; i < tabs[t].depth; i++)
			hnat_table_write(h, tabs[t].tab, i, zero);
}

/*
 * Master enable sequence, from the vendor sf_hnat_csr_init(), with
 * USE_DIP and big-endian tables. Empty tables never match, so the engine
 * can be enabled before any flow is offloaded.
 */
static void sf19a2890_hnat_csr_init(struct sf19a2890_hnat *h)
{
	int i;

	sf_hnat_tables_clear(h);

	/* 0x40004f as the vendor driver binary writes it; the SDK source has
	 * 0x40024f (bit 9 set).
	 */
	hnat_w(h, HWNAT_REG1_CSR, 0x0040004f | HNAT_MODE_SYMMETRIC);
	hnat_w(h, HWNAT_REG4_CSR, 0x64);
	hnat_w(h, HWNAT_REG3_CSR, 0x400000);
	hnat_w(h, HWNAT_REG2_TB_CONFIG, HWNAT_REG2_TB_CONFIG_ENTB |
				     HWNAT_REG2_TB_CONFIG_USE_DIP |
				     HWNAT_REG2_TB_CONFIG_EN_BIG_ENDIAN |
				     HWNAT_REG2_TB_CONFIG_CRC3_SEL |
				     HWNAT_REG2_TB_CONFIG_READ_CLR);
	hnat_w(h, HWNAT_REG19_REG_TB_RFC_TIMER, 0x1500);
	hnat_w(h, HWNAT_REG21_REG_TB_TFC_TIMER, 0x1500);
	hnat_w(h, HWNAT_REG18_REG_TB_FC_CFG, 0x3);
	hnat_w(h, HWNAT_REG18_REG_TB_FC_CFG, 0x0);
	hnat_w(h, HWNAT_REG17_REG_BUF_THRESH, 0x20f020f);
	/* REG33-36 (aging timers) keep their reset defaults, as in the vendor
	 * driver binary.
	 */

	for (i = 0; i < SUBNET_TABLE_MAX; i++)
		if (h->lan_subnets[i].valid)
			sf_hnat_lan_subnet_write(h, i);
}

static void sf19a2890_hnat_teardown(void *data)
{
	struct sf19a2890_hnat *h = data;

	debugfs_remove_recursive(h->debugfs);
	if (h->csr_ready)
		hnat_w(h, HWNAT_REG2_TB_CONFIG, 0);
	rhashtable_destroy(&h->napt_table);
	rhashtable_destroy(&h->flow_table);
}

/*
 * Set up the engine's software state. gmac_base is the GMAC's own
 * mapping, which covers the HNAT block; the hardware is only touched
 * once the GMAC is up (first flow block bind, or hw_setup_done), when
 * its clocks are on. Freed with @dev.
 */
struct sf19a2890_hnat *sf19a2890_hnat_probe(struct device *dev, void __iomem *gmac_base)
{
	struct sf19a2890_hnat *h;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return ERR_PTR(-ENOMEM);

	h->dev = dev;
	h->base = gmac_base + HNAT_MAP_OFFSET;
	spin_lock_init(&h->lock);
	mutex_init(&h->flow_mutex);
	INIT_LIST_HEAD(&h->block_cb_list);

	ret = rhashtable_init(&h->flow_table, &sf19a2890_hnat_ht_params);
	if (ret)
		return ERR_PTR(ret);
	ret = rhashtable_init(&h->napt_table, &sf19a2890_hnat_napt_ht_params);
	if (ret) {
		rhashtable_destroy(&h->flow_table);
		return ERR_PTR(ret);
	}

	h->debugfs = debugfs_create_dir("sf19a2890_hnat", NULL);
	debugfs_create_file("regs", 0444, h->debugfs, h, &hnat_regs_fops);
	debugfs_create_file("tables", 0444, h->debugfs, h, &hnat_tables_fops);

	ret = devm_add_action_or_reset(dev, sf19a2890_hnat_teardown, h);
	return ret ? ERR_PTR(ret) : h;
}
EXPORT_SYMBOL_GPL(sf19a2890_hnat_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Siflower SF19A2890 GMAC hardware NAT offload");
