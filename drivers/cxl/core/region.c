// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/memregion.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <region.h>
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
 */

static struct cxl_region *to_cxl_region(struct device *dev);

static void cxl_region_release(struct device *dev)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	memregion_free(cxlr->id);
	kfree(cxlr);
}

static const struct device_type cxl_region_type = {
	.name = "cxl_region",
	.release = cxl_region_release,
};

bool is_cxl_region(struct device *dev)
{
	return dev->type == &cxl_region_type;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_region, CXL);

static struct cxl_region *to_cxl_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, dev->type != &cxl_region_type,
			  "not a cxl_region device\n"))
		return NULL;

	return container_of(dev, struct cxl_region, dev);
}

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
