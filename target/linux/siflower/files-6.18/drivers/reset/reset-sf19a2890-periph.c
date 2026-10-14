// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>

struct reset_sf19a28_periph_data {
	struct reset_controller_dev rcdev;
	void __iomem *base;
	spinlock_t lock;
	u32 reset_masks[];
};

static inline struct reset_sf19a28_periph_data *
to_reset_sf19a28_periph_data(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct reset_sf19a28_periph_data, rcdev);
}

static int reset_sf19a28_periph_update(struct reset_controller_dev *rcdev,
			       unsigned long id, bool assert)
{
	struct reset_sf19a28_periph_data *data = to_reset_sf19a28_periph_data(rcdev);
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(&data->lock, flags);
	reg = readl(data->base);
	if (assert)
		reg |= data->reset_masks[id];
	else
		reg &= ~data->reset_masks[id];
	writel(reg, data->base);
	spin_unlock_irqrestore(&data->lock, flags);
	return 0;
}

static int reset_sf19a28_periph_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return reset_sf19a28_periph_update(rcdev, id, true);
}

static int reset_sf19a28_periph_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return reset_sf19a28_periph_update(rcdev, id, false);
}

static int reset_sf19a28_periph_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct reset_sf19a28_periph_data *data = to_reset_sf19a28_periph_data(rcdev);
	u32 reg;

	reg = readl(data->base);
	return !!(reg & data->reset_masks[id]);
}

const struct reset_control_ops reset_sf19a28_periph_ops = {
	.assert		= reset_sf19a28_periph_assert,
	.deassert	= reset_sf19a28_periph_deassert,
	.status		= reset_sf19a28_periph_status,
};

/*
 * Registered from of_clk_init() rather than as a platform driver: the
 * DW APB timers are probed by timer_probe() right after of_clk_init(),
 * long before platform drivers, and the bootloader leaves the timer
 * block in reset. A late reset controller makes of_reset_control_get()
 * fail there and the timer stays held in reset.
 */
static void __init reset_sf19a28_periph_of_init(struct device_node *node)
{
	struct reset_sf19a28_periph_data *data;
	void __iomem *base;
	int nr_resets;
	int ret, i;
	u32 tmp;

	nr_resets = of_property_count_u32_elems(node, "siflower,reset-masks");

	if (nr_resets < 1) {
		ret = of_property_read_u32(node, "siflower,num-resets", &tmp);
		if (ret < 0 || tmp < 1) {
			pr_err("%pOF: no resets\n", node);
			return;
		}
		nr_resets = tmp;
	}

	if (nr_resets >= 32) {
		pr_err("%pOF: too many resets\n", node);
		return;
	}

	data = kzalloc(struct_size(data, reset_masks, nr_resets), GFP_KERNEL);
	if (!data)
		return;

	ret = of_property_read_u32_array(node, "siflower,reset-masks",
					 data->reset_masks, nr_resets);
	if (ret)
		for (i = 0; i < nr_resets; i++)
			data->reset_masks[i] = BIT(i);

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("%pOF: cannot map registers\n", node);
		goto err_free;
	}

	spin_lock_init(&data->lock);
	data->base = base;
	data->rcdev.owner = THIS_MODULE;
	data->rcdev.nr_resets = nr_resets;
	data->rcdev.ops = &reset_sf19a28_periph_ops;
	data->rcdev.of_node = node;

	ret = reset_controller_register(&data->rcdev);
	if (ret) {
		pr_err("%pOF: register failed: %d\n", node, ret);
		iounmap(base);
		goto err_free;
	}
	/* no struct device will appear: let fw_devlink treat it as ready */
	fwnode_dev_initialized(of_fwnode_handle(node), true);
	return;

err_free:
	kfree(data);
}

CLK_OF_DECLARE(sf19a2890_periph_reset, "siflower,sf19a2890-periph-reset",
	       reset_sf19a28_periph_of_init);
