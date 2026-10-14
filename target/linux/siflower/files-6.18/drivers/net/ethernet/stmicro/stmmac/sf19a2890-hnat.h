/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SF19A2890_HNAT_H_
#define _SF19A2890_HNAT_H_

#include <linux/err.h>

struct device;
struct net_device;
struct sf19a2890_hnat;

#if IS_REACHABLE(CONFIG_SF19A2890_HNAT)
struct sf19a2890_hnat *sf19a2890_hnat_probe(struct device *dev, void __iomem *gmac_base);
/* ndo_setup_tc for TC_SETUP_FT; type_data is a struct flow_block_offload */
int sf19a2890_hnat_setup_tc_ft(struct sf19a2890_hnat *h,
			       struct net_device *ndev, void *type_data);
/* after every MAC/DMA (re)initialisation: the DMA soft reset wipes the
 * engine, so put back whatever was programmed before it
 */
void sf19a2890_hnat_hw_setup_done(struct sf19a2890_hnat *h);
#else
static inline struct sf19a2890_hnat *
sf19a2890_hnat_probe(struct device *dev, void __iomem *gmac_base)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int sf19a2890_hnat_setup_tc_ft(struct sf19a2890_hnat *h,
					     struct net_device *ndev,
					     void *type_data)
{
	return -EOPNOTSUPP;
}

static inline void sf19a2890_hnat_hw_setup_done(struct sf19a2890_hnat *h)
{
}
#endif

#endif /* _SF19A2890_HNAT_H_ */
