// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/idr.h>
#include <region.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

#include "core.h"

/**
 * DOC: cxl core region
 *
 * Regions are managed through the Linux device model. Each region instance is a
 * unique struct device. CXL core provides functionality to create, destroy, and
 * configure regions. This is all implemented here. Binding a region
 * (programming the hardware) is handled by a separate region driver.
 */

struct cxl_region *to_cxl_region(struct device *dev);
static const struct attribute_group region_interleave_group;

static bool is_region_active(struct cxl_region *cxlr)
{
	return cxlr->active;
}

/*
 * Most sanity checking is left up to region binding. This does the most basic
 * check to determine whether or not the core should try probing the driver.
 */
bool is_cxl_region_configured(const struct cxl_region *cxlr)
{
	/* zero sized regions aren't a thing. */
	if (cxlr->config.size <= 0)
		return false;

	/* all regions have at least 1 target */
	if (!cxlr->config.targets[0])
		return false;

	return true;
}
EXPORT_SYMBOL_GPL(is_cxl_region_configured);

static void remove_target(struct cxl_region *cxlr, int target)
{
	struct cxl_memdev *cxlmd;

	cxlmd = cxlr->config.targets[target];
	if (cxlmd)
		put_device(&cxlmd->dev);
	cxlr->config.targets[target] = NULL;
}

static ssize_t interleave_ways_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%d\n", cxlr->config.interleave_ways);
}

static ssize_t interleave_ways_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	int ret, prev_iw;
	int val;

	prev_iw = cxlr->config.interleave_ways;
	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;
	if (ret < 0 || ret > CXL_DECODER_MAX_INTERLEAVE)
		return -EINVAL;

	cxlr->config.interleave_ways = val;

	ret = sysfs_update_group(&dev->kobj, &region_interleave_group);
	if (ret < 0)
		goto err;

	sysfs_notify(&dev->kobj, NULL, "target_interleave");

	while (prev_iw > cxlr->config.interleave_ways)
		remove_target(cxlr, --prev_iw);

	return len;

err:
	cxlr->config.interleave_ways = prev_iw;
	return ret;
}
static DEVICE_ATTR_RW(interleave_ways);

static ssize_t interleave_granularity_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%d\n", cxlr->config.interleave_granularity);
}

static ssize_t interleave_granularity_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;
	cxlr->config.interleave_granularity = val;

	return len;
}
static DEVICE_ATTR_RW(interleave_granularity);

static ssize_t offset_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev->parent);
	struct cxl_region *cxlr = to_cxl_region(dev);
	resource_size_t offset;

	if (!cxlr->res)
		return sysfs_emit(buf, "\n");

	offset = cxld->platform_res.start - cxlr->res->start;

	return sysfs_emit(buf, "%pa\n", &offset);
}
static DEVICE_ATTR_RO(offset);

static ssize_t size_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%llu\n", cxlr->config.size);
}

static ssize_t size_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	unsigned long long val;
	ssize_t rc;

	rc = kstrtoull(buf, 0, &val);
	if (rc)
		return rc;

	device_lock(&cxlr->dev);
	if (is_region_active(cxlr))
		rc = -EBUSY;
	else
		cxlr->config.size = val;
	device_unlock(&cxlr->dev);

	return rc ? rc : len;
}
static DEVICE_ATTR_RW(size);

static ssize_t uuid_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%pUb\n", &cxlr->config.uuid);
}

static ssize_t uuid_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	ssize_t rc;

	if (len != UUID_STRING_LEN + 1)
		return -EINVAL;

	device_lock(&cxlr->dev);
	if (is_region_active(cxlr))
		rc = -EBUSY;
	else
		rc = uuid_parse(buf, &cxlr->config.uuid);
	device_unlock(&cxlr->dev);

	return rc ? rc : len;
}
static DEVICE_ATTR_RW(uuid);

static struct attribute *region_attrs[] = {
	&dev_attr_interleave_ways.attr,
	&dev_attr_interleave_granularity.attr,
	&dev_attr_offset.attr,
	&dev_attr_size.attr,
	&dev_attr_uuid.attr,
	NULL,
};

static const struct attribute_group region_group = {
	.attrs = region_attrs,
};

static size_t show_targetN(struct cxl_region *cxlr, char *buf, int n)
{
	int ret;

	device_lock(&cxlr->dev);
	if (!cxlr->config.targets[n])
		ret = sysfs_emit(buf, "\n");
	else
		ret = sysfs_emit(buf, "%s\n",
				 dev_name(&cxlr->config.targets[n]->dev));
	device_unlock(&cxlr->dev);

	return ret;
}

static size_t set_targetN(struct cxl_region *cxlr, const char *buf, int n,
			  size_t len)
{
	struct device *memdev_dev;
	struct cxl_memdev *cxlmd;

	device_lock(&cxlr->dev);

	if (len == 1 || cxlr->config.targets[n])
		remove_target(cxlr, n);

	/* Remove target special case */
	if (len == 1) {
		device_unlock(&cxlr->dev);
		return len;
	}

	memdev_dev = bus_find_device_by_name(&cxl_bus_type, NULL, buf);
	if (!memdev_dev) {
		device_unlock(&cxlr->dev);
		return -ENOENT;
	}

	/* reference to memdev held until target is unset or region goes away */

	cxlmd = to_cxl_memdev(memdev_dev);
	cxlr->config.targets[n] = cxlmd;

	device_unlock(&cxlr->dev);

	return len;
}

#define TARGET_ATTR_RW(n)                                                      \
	static ssize_t target##n##_show(                                       \
		struct device *dev, struct device_attribute *attr, char *buf)  \
	{                                                                      \
		return show_targetN(to_cxl_region(dev), buf, (n));             \
	}                                                                      \
	static ssize_t target##n##_store(struct device *dev,                   \
					 struct device_attribute *attr,        \
					 const char *buf, size_t len)          \
	{                                                                      \
		return set_targetN(to_cxl_region(dev), buf, (n), len);         \
	}                                                                      \
	static DEVICE_ATTR_RW(target##n)

TARGET_ATTR_RW(0);
TARGET_ATTR_RW(1);
TARGET_ATTR_RW(2);
TARGET_ATTR_RW(3);
TARGET_ATTR_RW(4);
TARGET_ATTR_RW(5);
TARGET_ATTR_RW(6);
TARGET_ATTR_RW(7);
TARGET_ATTR_RW(8);
TARGET_ATTR_RW(9);
TARGET_ATTR_RW(10);
TARGET_ATTR_RW(11);
TARGET_ATTR_RW(12);
TARGET_ATTR_RW(13);
TARGET_ATTR_RW(14);
TARGET_ATTR_RW(15);

static struct attribute *interleave_attrs[] = {
	&dev_attr_target0.attr,
	&dev_attr_target1.attr,
	&dev_attr_target2.attr,
	&dev_attr_target3.attr,
	&dev_attr_target4.attr,
	&dev_attr_target5.attr,
	&dev_attr_target6.attr,
	&dev_attr_target7.attr,
	&dev_attr_target8.attr,
	&dev_attr_target9.attr,
	&dev_attr_target10.attr,
	&dev_attr_target11.attr,
	&dev_attr_target12.attr,
	&dev_attr_target13.attr,
	&dev_attr_target14.attr,
	&dev_attr_target15.attr,
	NULL,
};

static umode_t visible_targets(struct kobject *kobj, struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct cxl_region *cxlr = to_cxl_region(dev);

	if (n < cxlr->config.interleave_ways)
		return a->mode;
	return 0;
}

static const struct attribute_group region_interleave_group = {
	.attrs = interleave_attrs,
	.is_visible = visible_targets,
};

static const struct attribute_group *region_groups[] = {
	&region_group,
	&region_interleave_group,
	NULL,
};

static void cxl_region_release(struct device *dev);

const struct device_type cxl_region_type = {
	.name = "cxl_region",
	.release = cxl_region_release,
	.groups = region_groups
};

static ssize_t create_region_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cxl_port *port = to_cxl_port(dev->parent);
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	int rc;

	if (dev_WARN_ONCE(dev, !is_root_decoder(dev),
			  "Invalid decoder selected for region.")) {
		return -ENODEV;
	}

	rc = ida_alloc(&cxld->region_ida, GFP_KERNEL);
	if (rc < 0) {
		dev_err(&cxld->dev, "Couldn't get a new id\n");
		return rc;
	}

	return sysfs_emit(buf, "region%d.%d:%d\n", port->id, cxld->id, rc);
}

static ssize_t create_region_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	struct cxl_port *port = to_cxl_port(dev->parent);
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	int decoder_id, port_id, region_id;
	struct cxl_region *cxlr;
	ssize_t rc;

	if (sscanf(buf, "region%d.%d:%d", &port_id, &decoder_id, &region_id) != 3)
		return -EINVAL;

	if (decoder_id != cxld->id)
		return -EINVAL;

	if (port_id != port->id)
		return -EINVAL;

	cxlr = cxl_alloc_region(cxld, region_id);
	if (IS_ERR(cxlr))
		return PTR_ERR(cxlr);

	rc = cxl_add_region(cxld, cxlr);
	if (rc) {
		kfree(cxlr);
		return rc;
	}

	return len;
}
DEVICE_ATTR_RW(create_region);

static ssize_t delete_region_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	int rc;

	rc = cxl_delete_region(cxld, buf);
	if (rc)
		return rc;

	return len;
}
DEVICE_ATTR_WO(delete_region);

struct cxl_region *to_cxl_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, dev->type != &cxl_region_type,
			  "not a cxl_region device\n"))
		return NULL;

	return container_of(dev, struct cxl_region, dev);
}
EXPORT_SYMBOL_GPL(to_cxl_region);

static void cxl_region_release(struct device *dev)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev->parent);
	struct cxl_region *cxlr = to_cxl_region(dev);
	int i;

	ida_free(&cxld->region_ida, cxlr->id);
	for (i = 0; i < cxlr->config.interleave_ways; i++)
		remove_target(cxlr, i);
	kfree(cxlr);
}

struct cxl_region *cxl_alloc_region(struct cxl_decoder *cxld, int id)
{
	struct cxl_region *cxlr;

	cxlr = kzalloc(sizeof(*cxlr), GFP_KERNEL);
	if (!cxlr)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&cxlr->staged_list);
	INIT_LIST_HEAD(&cxlr->commit_list);
	cxlr->id = id;

	return cxlr;
}

/**
 * cxl_add_region - Adds a region to a decoder
 * @cxld: Parent decoder.
 * @cxlr: Region to be added to the decoder.
 *
 * This is the second step of region initialization. Regions exist within an
 * address space which is mapped by a @cxld. That @cxld must be a root decoder,
 * and it enforces constraints upon the region as it is configured.
 *
 * Return: 0 if the region was added to the @cxld, else returns negative error
 * code. The region will be named "regionX.Y.Z" where X is the port, Y is the
 * decoder id, and Z is the region number.
 */
int cxl_add_region(struct cxl_decoder *cxld, struct cxl_region *cxlr)
{
	struct cxl_port *port = to_cxl_port(cxld->dev.parent);
	struct device *dev = &cxlr->dev;
	int rc;

	device_initialize(dev);
	dev->parent = &cxld->dev;
	device_set_pm_not_required(dev);
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_region_type;
	rc = dev_set_name(dev, "region%d.%d:%d", port->id, cxld->id, cxlr->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	dev_dbg(dev, "Added to %s\n", dev_name(&cxld->dev));

	return 0;

err:
	put_device(dev);
	return rc;
}

static struct cxl_region *cxl_find_region_by_name(struct cxl_decoder *cxld,
						  const char *name)
{
	struct device *region_dev;

	region_dev = device_find_child_by_name(&cxld->dev, name);
	if (!region_dev)
		return ERR_PTR(-ENOENT);

	return to_cxl_region(region_dev);
}

/**
 * cxl_delete_region - Deletes a region
 * @cxld: Parent decoder
 * @region_name: Named region, ie. regionX.Y:Z
 */
int cxl_delete_region(struct cxl_decoder *cxld, const char *region_name)
{
	struct cxl_region *cxlr;

	device_lock(&cxld->dev);

	cxlr = cxl_find_region_by_name(cxld, region_name);
	if (IS_ERR(cxlr)) {
		device_unlock(&cxld->dev);
		return PTR_ERR(cxlr);
	}

	dev_dbg(&cxld->dev, "Requested removal of %s from %s\n",
		dev_name(&cxlr->dev), dev_name(&cxld->dev));

	device_unregister(&cxlr->dev);
	device_unlock(&cxld->dev);

	put_device(&cxlr->dev);

	return 0;
}
