// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/memregion.h>
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

/**
 * DOC: cxl core region
 *
 * CXL Regions represent mapped memory capacity in system physical address
 * space. Whereas the CXL Root Decoders identify the bounds of potential CXL
 * Memory ranges, Regions represent the active mapped capacity by the HDM
 * Decoder Capability structures throughout the Host Bridges, Switches, and
 * Endpoints in the topology.
 *
 * Region configuration has some ordering constraints:
 * - Size: Must be set after all targets
 * - Targets: Must be set after interleave ways
 * - Interleave ways: Must be set after Interleave Granularity
 *
 * UUID may be set at any time before binding the driver to the region.
 */

static const struct attribute_group region_interleave_group;

static void remove_target(struct cxl_region *cxlr, int target)
{
	struct cxl_endpoint_decoder *cxled;

	mutex_lock(&cxlr->remove_lock);
	cxled = cxlr->targets[target];
	if (cxled) {
		cxled->cxlr = NULL;
		put_device(&cxled->base.dev);
	}
	cxlr->targets[target] = NULL;
	mutex_unlock(&cxlr->remove_lock);
}

static void cxl_region_release(struct device *dev)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	int i;

	memregion_free(cxlr->id);
	for (i = 0; i < cxlr->interleave_ways; i++)
		remove_target(cxlr, i);
	kfree(cxlr);
}

static ssize_t interleave_ways_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%d\n", cxlr->interleave_ways);
}

static ssize_t interleave_ways_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_decoder *rootd;
	int rc, val;

	rc = kstrtoint(buf, 0, &val);
	if (rc)
		return rc;

	cxl_device_lock(dev);

	if (dev->driver) {
		cxl_device_unlock(dev);
		return -EBUSY;
	}

	if (cxlr->interleave_ways) {
		cxl_device_unlock(dev);
		return -EEXIST;
	}

	if (!cxlr->interleave_granularity) {
		dev_dbg(&cxlr->dev, "IG must be set before IW\n");
		cxl_device_unlock(dev);
		return -EILSEQ;
	}

	rootd = to_cxl_decoder(cxlr->dev.parent);
	if (!cxl_is_interleave_ways_valid(cxlr, rootd, val)) {
		cxl_device_unlock(dev);
		return -EINVAL;
	}

	cxlr->interleave_ways = val;
	cxl_device_unlock(dev);

	rc = sysfs_update_group(&cxlr->dev.kobj, &region_interleave_group);
	if (rc < 0) {
		cxlr->interleave_ways = 0;
		return rc;
	}

	return len;
}
static DEVICE_ATTR_RW(interleave_ways);

static ssize_t interleave_granularity_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%d\n", cxlr->interleave_granularity);
}

static ssize_t interleave_granularity_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_decoder *rootd;
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	cxl_device_lock(dev);

	if (dev->driver) {
		cxl_device_unlock(dev);
		return -EBUSY;
	}

	if (cxlr->interleave_granularity) {
		cxl_device_unlock(dev);
		return -EEXIST;
	}

	rootd = to_cxl_decoder(cxlr->dev.parent);
	if (!cxl_is_interleave_granularity_valid(rootd, val)) {
		cxl_device_unlock(dev);
		return -EINVAL;
	}

	cxlr->interleave_granularity = val;
	cxl_device_unlock(dev);

	return len;
}
static DEVICE_ATTR_RW(interleave_granularity);

static ssize_t offset_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	if (!cxlr->res)
		return sysfs_emit(buf, "\n");

	return sysfs_emit(buf, "%pa\n", &cxlr->res->start);
}
static DEVICE_ATTR_RO(offset);

static ssize_t size_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%#llx\n", resource_size(cxlr->res));
}
static DEVICE_ATTR_RO(size);

static ssize_t uuid_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%pUb\n", &cxlr->uuid);
}

static int is_dupe(struct device *match, void *_cxlr)
{
	struct cxl_region *c, *cxlr = _cxlr;

	if (!is_cxl_region(match))
		return 0;

	if (&cxlr->dev == match)
		return 0;

	c = to_cxl_region(match);
	if (uuid_equal(&c->uuid, &cxlr->uuid))
		return -EEXIST;

	return 0;
}

static ssize_t uuid_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	ssize_t rc;
	uuid_t temp;

	if (len != UUID_STRING_LEN + 1)
		return -EINVAL;

	rc = uuid_parse(buf, &temp);
	if (rc)
		return rc;

	cxl_device_lock(dev);

	if (dev->driver) {
		cxl_device_unlock(dev);
		return -EBUSY;
	}

	if (!uuid_is_null(&cxlr->uuid)) {
		cxl_device_unlock(dev);
		return -EEXIST;
	}

	rc = bus_for_each_dev(&cxl_bus_type, NULL, cxlr, is_dupe);
	if (rc < 0) {
		cxl_device_unlock(dev);
		return false;
	}

	cxlr->uuid = temp;
	cxl_device_unlock(dev);
	return len;
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
	if (!cxlr->targets[n])
		return sysfs_emit(buf, "\n");

	return sysfs_emit(buf, "%s\n", dev_name(&cxlr->targets[n]->base.dev));
}

static size_t store_targetN(struct cxl_region *cxlr, const char *buf, int n,
			    size_t len)
{
	struct cxl_endpoint_decoder *cxled;
	struct cxl_decoder *cxld;
	struct device *cxld_dev;

	cxl_device_lock(&cxlr->dev);

	if (cxlr->dev.driver) {
		cxl_device_unlock(&cxlr->dev);
		return -EBUSY;
	}

	if (cxlr->targets[n]) {
		cxl_device_unlock(&cxlr->dev);
		return -EEXIST;
	}

	cxld_dev = bus_find_device_by_name(&cxl_bus_type, NULL, buf);
	if (!cxld_dev)
		return -ENOENT;

	if (!is_cxl_decoder(cxld_dev)) {
		put_device(cxld_dev);
		return -EINVAL;
	}

	if (!is_cxl_endpoint(to_cxl_port(cxld_dev->parent))) {
		put_device(cxld_dev);
		return -EINVAL;
	}

	cxld = to_cxl_decoder(cxld_dev);
	if (cxld->flags & CXL_DECODER_F_ENABLE) {
		put_device(cxld_dev);
		return -EBUSY;
	}

	/* decoder reference is held until teardown */
	cxled = to_cxl_endpoint_decoder(cxld);
	cxlr->targets[n] = cxled;
	cxled->cxlr = cxlr;

	cxl_device_unlock(&cxlr->dev);

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
		return store_targetN(to_cxl_region(dev), buf, (n), len);       \
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

	if (n < cxlr->interleave_ways)
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
	&cxl_base_attribute_group,
	NULL,
};

static const struct device_type cxl_region_type = {
	.name = "cxl_region",
	.release = cxl_region_release,
	.groups = region_groups
};

bool is_cxl_region(struct device *dev)
{
	return dev->type == &cxl_region_type;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_region, CXL);

struct cxl_region *to_cxl_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, dev->type != &cxl_region_type,
			  "not a cxl_region device\n"))
		return NULL;

	return container_of(dev, struct cxl_region, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_region, CXL);

static void unregister_region(struct work_struct *work)
{
	struct cxl_region *cxlr;

	cxlr = container_of(work, typeof(*cxlr), detach_work);
	device_unregister(&cxlr->dev);
}

static void schedule_unregister(void *cxlr)
{
	schedule_cxl_region_unregister(cxlr);
}

static struct cxl_region *cxl_region_alloc(struct cxl_decoder *cxld)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(cxld);
	struct cxl_region *cxlr;
	struct device *dev;
	int rc;

	lockdep_assert_held(&cxlrd->id_lock);

	rc = memregion_alloc(GFP_KERNEL);
	if (rc < 0) {
		dev_dbg(dev, "Failed to get next cached id (%d)\n", rc);
		return ERR_PTR(rc);
	}

	cxlr = kzalloc(sizeof(*cxlr), GFP_KERNEL);
	if (!cxlr) {
		memregion_free(rc);
		return ERR_PTR(-ENOMEM);
	}

	cxlr->id = cxlrd->next_region_id;
	cxlrd->next_region_id = rc;

	dev = &cxlr->dev;
	device_initialize(dev);
	dev->parent = &cxld->dev;
	device_set_pm_not_required(dev);
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_region_type;
	INIT_WORK(&cxlr->detach_work, unregister_region);
	mutex_init(&cxlr->remove_lock);

	return cxlr;
}

/**
 * devm_cxl_add_region - Adds a region to a decoder
 * @cxld: Parent decoder.
 *
 * This is the second step of region initialization. Regions exist within an
 * address space which is mapped by a @cxld. That @cxld must be a root decoder,
 * and it enforces constraints upon the region as it is configured.
 *
 * Return: 0 if the region was added to the @cxld, else returns negative error
 * code. The region will be named "regionX.Y.Z" where X is the port, Y is the
 * decoder id, and Z is the region number.
 */
static struct cxl_region *devm_cxl_add_region(struct cxl_decoder *cxld)
{
	struct cxl_port *port = to_cxl_port(cxld->dev.parent);
	struct cxl_region *cxlr;
	struct device *dev;
	int rc;

	cxlr = cxl_region_alloc(cxld);
	if (IS_ERR(cxlr))
		return cxlr;

	dev = &cxlr->dev;

	rc = dev_set_name(dev, "region%d", cxlr->id);
	if (rc)
		goto err_out;

	rc = device_add(dev);
	if (rc)
		goto err_put;

	rc = devm_add_action_or_reset(port->uport, schedule_unregister, cxlr);
	if (rc)
		goto err_put;

	return cxlr;

err_put:
	put_device(&cxld->dev);

err_out:
	put_device(dev);
	return ERR_PTR(rc);
}

static ssize_t create_pmem_region_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(cxld);
	size_t rc;

	/*
	 * There's no point in returning known bad answers when the lock is held
	 * on the store side, even though the answer given here may be
	 * immediately invalidated as soon as the lock is dropped it's still
	 * useful to throttle readers in the presence of writers.
	 */
	rc = mutex_lock_interruptible(&cxlrd->id_lock);
	if (rc)
		return rc;
	rc = sysfs_emit(buf, "%d\n", cxlrd->next_region_id);
	mutex_unlock(&cxlrd->id_lock);

	return rc;
}

static ssize_t create_pmem_region_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(cxld);
	struct cxl_region *cxlr;
	size_t id, rc;

	rc = kstrtoul(buf, 10, &id);
	if (rc)
		return rc;

	rc = mutex_lock_interruptible(&cxlrd->id_lock);
	if (rc)
		return rc;

	if (cxlrd->next_region_id != id) {
		rc = -EINVAL;
		goto out;
	}

	cxlr = devm_cxl_add_region(cxld);
	rc = 0;
	dev_dbg(dev, "Created %s\n", dev_name(&cxlr->dev));

out:
	mutex_unlock(&cxlrd->id_lock);
	if (rc)
		return rc;
	return len;
}
DEVICE_ATTR_RW(create_pmem_region);

static struct cxl_region *cxl_find_region_by_name(struct cxl_decoder *cxld,
						  const char *name)
{
	struct device *region_dev;

	region_dev = device_find_child_by_name(&cxld->dev, name);
	if (!region_dev)
		return ERR_PTR(-ENOENT);

	return to_cxl_region(region_dev);
}

static ssize_t delete_region_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	struct cxl_port *port = to_cxl_port(dev->parent);
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	struct cxl_region *cxlr;

	cxlr = cxl_find_region_by_name(cxld, buf);
	if (IS_ERR(cxlr))
		return PTR_ERR(cxlr);

	/* Reference held for wq */
	devm_release_action(port->uport, schedule_unregister, cxlr);

	return len;
}
DEVICE_ATTR_WO(delete_region);
