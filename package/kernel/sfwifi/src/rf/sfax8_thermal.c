// SPDX-License-Identifier: GPL-2.0-only
/* sfax8_thermal.c - Siflower SFAx8 thermal glue
 *
 * Ported from the vendor 4.14 SDK (drivers/thermal/sfax8_thermal.c) to
 * the 6.12 thermal framework, using devm_thermal_of_zone_register().
 *
 * Original copyright (C) 2017 Shanghai Siflower Communication
 * Technology Co., Ltd. Author: Qi Zhang <qi.zhang@siflower.com.cn> */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include "sfax8_thermal.h"

struct sfax8_sensor_data {
	struct device *dev;
	struct thermal_zone_device *tzd;
	void *data;
	struct sfax8_zone_device_ops *ops;
	long last_temp;
	long temp;
	struct list_head node;
};

struct sfax8_cooling_device_data {
	struct device *dev;
	struct thermal_cooling_device *cdev;
	struct sfax8_cooling_device_ops *ops;
	void *data;
	struct list_head node;
};

static LIST_HEAD(sfax8_sensor_list);
static LIST_HEAD(sfax8_cdev_list);

static DEFINE_MUTEX(sfax8_sensor_list_lock);
static DEFINE_MUTEX(sfax8_cdev_list_lock);

static int __sfax8_get_temperature(struct thermal_zone_device *tz, int *temp)
{
	struct sfax8_sensor_data *sensor_data = thermal_zone_device_priv(tz);
	int ret;

	if (!sensor_data)
		return -EINVAL;

	ret = sensor_data->ops->get_temp(sensor_data->data, temp);
	sensor_data->last_temp = sensor_data->temp;
	sensor_data->temp = *temp;

	return ret;
}

static int __sfax8_get_trend(struct thermal_zone_device *tz,
			      const struct thermal_trip *trip,
			      enum thermal_trend *trend)
{
	struct sfax8_sensor_data *sensor_data = thermal_zone_device_priv(tz);
	long delta;

	if (!sensor_data)
		return -EINVAL;

	if (sensor_data->ops->get_trend)
		return sensor_data->ops->get_trend(sensor_data->data, trend);

	if ((sensor_data->temp == THERMAL_TEMP_INVALID) ||
	    (sensor_data->last_temp == THERMAL_TEMP_INVALID)) {
		*trend = THERMAL_TREND_STABLE;
		return 0;
	}

	delta = sensor_data->temp - sensor_data->last_temp;
	if (abs(delta) < 3)
		*trend = THERMAL_TREND_STABLE;
	else
		*trend = (delta > 0) ? THERMAL_TREND_RAISING : THERMAL_TREND_DROPPING;
	return 0;
}

static const struct thermal_zone_device_ops sfax8_of_thermal_ops = {
	.get_temp = __sfax8_get_temperature,
	.get_trend = __sfax8_get_trend,
};

int sfax8_thermal_sensor_register(struct device *dev,
				struct sfax8_zone_device_ops *ops, void *data)
{
	int sensor_id = 0;
	struct sfax8_sensor_data *sdata;
	struct thermal_zone_device *tzd;

	if (!ops->get_temp)
		return -EINVAL;

	sdata = kzalloc(sizeof(*sdata), GFP_KERNEL);
	if (!sdata)
		return -ENOMEM;

	sdata->dev = dev;
	sdata->data = data;
	sdata->ops = ops;

	tzd = devm_thermal_of_zone_register(dev, sensor_id, sdata,
					     &sfax8_of_thermal_ops);
	if (IS_ERR(tzd)) {
		kfree(sdata);
		return PTR_ERR(tzd);
	}

	sdata->tzd = tzd;

	mutex_lock(&sfax8_sensor_list_lock);
	list_add_tail(&sdata->node, &sfax8_sensor_list);
	mutex_unlock(&sfax8_sensor_list_lock);

	return 0;
}

static void __sfax8_thermal_sensor_unregister(struct sfax8_sensor_data *sdata)
{
	devm_thermal_of_zone_unregister(sdata->dev, sdata->tzd);
}

void sfax8_thermal_sensor_unregister(struct device *dev)
{
	struct sfax8_sensor_data *pos, *next;

	mutex_lock(&sfax8_sensor_list_lock);
	list_for_each_entry_safe(pos, next, &sfax8_sensor_list, node) {
		if (pos->dev != dev)
			continue;

		__sfax8_thermal_sensor_unregister(pos);
		list_del(&pos->node);
		mutex_unlock(&sfax8_sensor_list_lock);
		kfree(pos);
		return;
	}
	mutex_unlock(&sfax8_sensor_list_lock);
}

static int sfax8_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct sfax8_cooling_device_data *cdata = cdev->devdata;

	return cdata->ops->get_max_state(cdata->data, state);
}

static int sfax8_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct sfax8_cooling_device_data *cdata = cdev->devdata;

	return cdata->ops->get_cur_state(cdata->data, state);
}

static int sfax8_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct sfax8_cooling_device_data *cdata = cdev->devdata;

	return cdata->ops->set_cur_state(cdata->data, state);
}

static struct thermal_cooling_device_ops sfax8_thermal_ops = {
	.get_max_state = sfax8_get_max_state,
	.get_cur_state = sfax8_get_cur_state,
	.set_cur_state = sfax8_set_cur_state,
};

int sfax8_cooling_device_register(struct device *dev, void *devdata,
				  struct sfax8_cooling_device_ops *ops)
{
	struct device_node *np;
	struct thermal_cooling_device *cdev;
	struct sfax8_cooling_device_data *cdata;
	char type[THERMAL_NAME_LENGTH];

	if (!dev || !dev->of_node)
		return -ENODEV;

	if (!ops || !ops->get_max_state || !ops->get_cur_state ||
	    !ops->set_cur_state)
		return -EINVAL;

	cdata = kzalloc(sizeof(*cdata), GFP_KERNEL);
	if (!cdata)
		return -ENOMEM;

	np = dev->of_node;
	strscpy(type, np->name, sizeof(type));

	cdata->data = devdata;
	cdata->ops = ops;

	cdev = thermal_of_cooling_device_register(np, type, cdata, &sfax8_thermal_ops);
	if (IS_ERR(cdev)) {
		kfree(cdata);
		return PTR_ERR(cdev);
	}

	cdata->cdev = cdev;
	cdata->dev = dev;

	mutex_lock(&sfax8_cdev_list_lock);
	list_add_tail(&cdata->node, &sfax8_cdev_list);
	mutex_unlock(&sfax8_cdev_list_lock);

	return 0;
}

void sfax8_cooling_device_unregister(struct device *dev)
{
	struct sfax8_cooling_device_data *pos, *next;

	mutex_lock(&sfax8_cdev_list_lock);
	list_for_each_entry_safe(pos, next, &sfax8_cdev_list, node) {
		if (pos->dev != dev)
			continue;

		thermal_cooling_device_unregister(pos->cdev);
		list_del(&pos->node);
		kfree(pos);
		mutex_unlock(&sfax8_cdev_list_lock);
		return;
	}
	mutex_unlock(&sfax8_cdev_list_lock);
}
