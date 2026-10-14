/* SPDX-License-Identifier: GPL-2.0 */
/* Register layout of the HNAT (hardware NAPT) engine in the SF19A2890
 * GMAC's MMIO window, as offsets from the GMAC base. From the vendor
 * 4.14 SDK (sf_hnat_reg.h).
 */
#ifndef _SF19A2890_HNAT_REG_H_
#define _SF19A2890_HNAT_REG_H_

#include <linux/bits.h>

#define HNAT_CSR_BASE			0x4000
#define HNAT_TABLE_BASE			0x6000

#define NAPT_TABLE_MAX			1024
#define VLAN_TABLE_MAX			128
#define DIP_TABLE_MAX			512
#define RT_PUB_NET_TABLE_MAX		16
#define PPPHD_TABLE_MAX			8
#define DMAC_TABLE_MAX			128
#define ROUTER_MAC_TABLE_MAX		8

/* table numbers, passed in HWNAT_REG3_TB_ADDRESS bits [31:16] */
#define INAT_NAPT_HASH1_NO		0
#define INAT_NAPT_HASH2_NO		1
#define INAT_NAPT_HASH3_NO		2
#define ENAT_NAPT_HASH1_NO		3
#define ENAT_NAPT_HASH2_NO		4
#define ENAT_NAPT_HASH3_NO		5
#define NAPT_NO				6
#define NAPT_VLD_NO			7
#define RT_PUB_NET_NO			8
#define DIP_HASH1_NO			16
#define DIP_HASH2_NO			17
#define DIP_NO				18
#define DIP_VLD_NO			19
#define DMAC_NO				20
#define PPPOE_HD_NO			21
#define ROUTER_MAC_NO			22
#define VLAN_ID_NO			23
#define STATICS_NO			24
#define HNAT_TB_NO			25 /* 0-8,16-24 valid; 9-15,25-31 reserved */

#define HWNAT_REG1_TB_STATUS_BUSY	0x00000001
#define HWNAT_REG4_TB_OP_CODE_WR	1
#define HWNAT_REG4_TB_OP_CODE_RD	0
#define HWNAT_REG3_TB_ADD_NO_OFF	16
#define HWNAT_TB_WRDATA_NO		5
#define HWNAT_TB_RDDATA_NO		7
#define HWNAT_REG2_TB_CONFIG_ENTB		0x0fff0000
#define HWNAT_REG2_TB_CONFIG_USE_DIP		0x00000001
#define HWNAT_REG2_TB_CONFIG_EN_BIG_ENDIAN	0x00000010
#define HWNAT_REG2_TB_CONFIG_CRC3_SEL		0x00000400
#define HWNAT_REG2_TB_CONFIG_READ_CLR		0x00000004

/*
 * NAPT/DIP hash-chain slot numbering: sf_hnat_napt_add_crc_hash_entry()
 * tries HASH1 (2-way, returns raw index 0-1) first, then HASH2 (4-way,
 * returns raw index 0-3, offset added), then HASH3 (2-way, offset
 * added) -- the combined index tells del_crc_hash_entry() which table
 * and which packed sub-field to touch. Same idea for DIP's HASH1(3-way,
 * skips its own index 2 -- "RM#9392")/HASH2(4-way).
 */
#define NAPT_HASH2_INDEX_OFFSET		2
#define NAPT_HASH3_INDEX_OFFSET		6
#define DIP_HASH2_INDEX_OFFSET		4

/* hnat_mode, OR'd into HWNAT_REG1_CSR (sf_hnat_common.h in the vendor SDK) */
#define HNAT_MODE_BASIC			(0 << 25)
#define HNAT_MODE_SYMMETRIC		BIT(25)
#define HNAT_MODE_FULL_CONE		(2 << 25)
#define HNAT_MODE_RESTRICT_CONE		(3 << 25)
#define HNAT_MODE_PORT_RESTRICT_CONE	BIT(27)

/* CSR block (HNAT_CSR_BASE-relative) */
#define HWNAT_REG0_CSR			(HNAT_CSR_BASE + 0x0000)
#define HWNAT_REG1_CSR			(HNAT_CSR_BASE + 0x0004)
#define HWNAT_REG2_CSR			(HNAT_CSR_BASE + 0x0008)
#define  HWNAT_REG2_CSR_SUBNET_UPDATE	BIT(0)	/* 0->1 commits REG16-31 */
#define  HWNAT_REG2_CSR_CNT_LATCH	BIT(4)	/* 0->1 latches the counters */
#define HWNAT_REG16_CSR			(HNAT_CSR_BASE + 0x0040)	/* LAN subnet ip[0..7] */
#define HWNAT_REG30_CSR			(HNAT_CSR_BASE + 0x0080)	/* LAN subnet mask[0..3] */
#define HWNAT_REG31_CSR			(HNAT_CSR_BASE + 0x0084)	/* LAN subnet mask[4..7] */
#define SUBNET_TABLE_MAX		8
#define HWNAT_REG3_CSR			(HNAT_CSR_BASE + 0x000c)
#define HWNAT_REG4_CSR			(HNAT_CSR_BASE + 0x0010)
#define HWNAT_RX_ENTER_SOF_CNT_L	(HNAT_CSR_BASE + 0x0100)
#define HWNAT_RX_ENTER_SOF_CNT_H	(HNAT_CSR_BASE + 0x0104)
#define HWNAT_RX_ENTER_EOF_CNT_L	(HNAT_CSR_BASE + 0x0108)
#define HWNAT_RX_ENTER_EOF_CNT_H	(HNAT_CSR_BASE + 0x010c)
#define HWNAT_RX_ENTER_DROP_CNT		(HNAT_CSR_BASE + 0x0110)
#define HWNAT_TX_ENAT_CNT		(HNAT_CSR_BASE + 0x016c)
#define HWNAT_RX_ENAT_CNT		(HNAT_CSR_BASE + 0x0170)
#define HWNAT_RX_INAT_CNT		(HNAT_CSR_BASE + 0x0174)
#define HWNAT_RX_TOTAL_ERR_CNT		(HNAT_CSR_BASE + 0x0178)
#define HWNAT_RX2TX_DROP_CNT		(HNAT_CSR_BASE + 0x0194)
/* engine-enable/tuning registers written once at init (sf_hnat_csr_init) */
#define HWNAT_REG33_CSR			(HNAT_CSR_BASE + 0x008c)
#define HWNAT_REG34_CSR			(HNAT_CSR_BASE + 0x0090)
#define HWNAT_REG35_CSR			(HNAT_CSR_BASE + 0x0094)
#define HWNAT_REG36_CSR			(HNAT_CSR_BASE + 0x0098)
/* last CSR counter, defines the end of the block this driver maps */
#define HNAT_CSR_BLOCK_SIZE		0x0198

/* table access block (HNAT_TABLE_BASE-relative) */
#define HWNAT_REG1_TB_STATUS		(HNAT_TABLE_BASE + 0x0000)
#define HWNAT_REG2_TB_CONFIG		(HNAT_TABLE_BASE + 0x0004)
#define HWNAT_REG3_TB_ADDRESS		(HNAT_TABLE_BASE + 0x0008)
#define HWNAT_REG4_TB_OP_CODE		(HNAT_TABLE_BASE + 0x000c)
#define HWNAT_REG5_TB_WRDATA0		(HNAT_TABLE_BASE + 0x0010)
#define HWNAT_REG6_TB_WRDATA1		(HNAT_TABLE_BASE + 0x0014)
#define HWNAT_REG7_TB_WRDATA2		(HNAT_TABLE_BASE + 0x0018)
#define HWNAT_REG8_TB_WRDATA3		(HNAT_TABLE_BASE + 0x001c)
#define HWNAT_REG9_TB_WRDATA4		(HNAT_TABLE_BASE + 0x0020)
#define HWNAT_REG10_TB_RDDATA0		(HNAT_TABLE_BASE + 0x0030)
#define HWNAT_REG11_TB_RDDATA1		(HNAT_TABLE_BASE + 0x0034)
#define HWNAT_REG12_TB_RDDATA2		(HNAT_TABLE_BASE + 0x0038)
#define HWNAT_REG13_TB_RDDATA3		(HNAT_TABLE_BASE + 0x003c)
#define HWNAT_REG14_TB_RDDATA4		(HNAT_TABLE_BASE + 0x0040)
#define HWNAT_REG15_TB_RDDATA5		(HNAT_TABLE_BASE + 0x0044)
#define HWNAT_REG16_TB_RDDATA6		(HNAT_TABLE_BASE + 0x0048)
#define HWNAT_REG17_REG_BUF_THRESH	(HNAT_TABLE_BASE + 0x0050)
#define HWNAT_REG18_REG_TB_FC_CFG	(HNAT_TABLE_BASE + 0x0060)
#define HWNAT_REG19_REG_TB_RFC_TIMER	(HNAT_TABLE_BASE + 0x0064)
#define HWNAT_REG20_REG_TB_RFC_AGECNT	(HNAT_TABLE_BASE + 0x0068)
#define HWNAT_REG21_REG_TB_TFC_TIMER	(HNAT_TABLE_BASE + 0x006c)
/* last table-block register, defines the end of the block this driver maps */
#define HNAT_TABLE_BLOCK_SIZE		0x0070

#endif /* _SF19A2890_HNAT_REG_H_ */
