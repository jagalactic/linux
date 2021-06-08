// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/io-64-nonatomic-lo-hi.h>
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
 * Regions are managed through the Linux device model. Each region instance is a
 * unique struct device. CXL core provides functionality to create, destroy, and
 * configure regions. This is all implemented here. Binding a region
 * (programming the hardware) is handled by a separate region driver.
 */

static void cxl_region_release(struct device *dev);

static const struct device_type cxl_region_type = {
	.name = "cxl_region",
	.release = cxl_region_release,
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

	ida_free(&cxld->region_ida, cxlr->id);
	kfree(cxlr);
}

struct cxl_region *cxl_alloc_region(struct cxl_decoder *cxld, int id)
{
	struct cxl_region *cxlr;

	cxlr = kzalloc(sizeof(*cxlr), GFP_KERNEL);
	if (!cxlr)
		return ERR_PTR(-ENOMEM);

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
