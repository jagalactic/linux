// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "cxlmem.h"
#include "pci.h"

/**
 * DOC: cxl mem
 *
 * CXL memory endpoint devices and switches are CXL capable devices that are
 * participating in CXL.mem protocol. Their functionality builds on top of the
 * CXL.io protocol that allows enumerating and configuring components via
 * standard PCI mechanisms.
 *
 * The cxl_mem driver implements enumeration and control over these CXL
 * components.
 */

struct walk_ctx {
	struct cxl_dport *root_port;
	bool has_switch;
};

/**
 * walk_to_root_port() - Walk up to root port
 * @dev: Device to walk up from
 * @ctx: Information to populate while walking
 *
 * A platform specific driver such as cxl_acpi is responsible for scanning CXL
 * topologies in a top-down fashion. If the CXL memory device is directly
 * connected to the top level hostbridge, nothing else needs to be done. If
 * however there are CXL components (ie. a CXL switch) in between an endpoint
 * and a hostbridge the platform specific driver must be notified after all the
 * components are enumerated.
 */
static void walk_to_root_port(struct device *dev, struct walk_ctx *ctx)
{
	struct cxl_dport *root_port;

	if (!dev->parent)
		return;

	root_port = cxl_get_root_dport(dev);
	if (root_port)
		ctx->root_port = root_port;

	if (is_cxl_switch_usp(dev))
		ctx->has_switch = true;

	walk_to_root_port(dev->parent, ctx);
}

static void remove_endpoint(void *_cxlmd)
{
	struct cxl_memdev *cxlmd = _cxlmd;
	struct cxl_port *endpoint;

	if (cxlmd->root_port)
		sysfs_remove_link(&cxlmd->dev.kobj, "root_port");

	endpoint = dev_get_drvdata(&cxlmd->dev);

	devm_cxl_remove_port(endpoint);
}

static int create_endpoint(struct device *dev, struct cxl_port *parent,
			   struct cxl_dport *dport)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_port *endpoint;
	int rc;

	endpoint = devm_cxl_add_port(dev, cxlmd->component_reg_phys, parent);
	if (IS_ERR(endpoint))
		return PTR_ERR(endpoint);

	rc = sysfs_create_link(&cxlmd->dev.kobj, &dport->dport->kobj,
			       "root_port");
	if (rc) {
		device_del(&endpoint->dev);
		return rc;
	}
	dev_set_drvdata(dev, endpoint);
	dev_dbg(dev, "add: %s\n", dev_name(&endpoint->dev));

	return devm_add_action_or_reset(dev, remove_endpoint, cxlmd);
}

static int cxl_mem_probe(struct device *dev)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_port *hostbridge, *parent_port;
	struct walk_ctx ctx = { NULL, false };
	int rc;

	walk_to_root_port(dev, &ctx);

	/*
	 * Couldn't find a CXL capable root port. This may happen if cxl_acpi
	 * hasn't completed in which case cxl_acpi will rescan the bus.
	 */
	if (!ctx.root_port)
		return -ENODEV;

	/* FIXME: This lock is racy, and does it even need to be here? */
	hostbridge = ctx.root_port->port;
	device_lock(&hostbridge->dev);

	/* hostbridge has no port driver, the topology isn't enabled yet */
	if (!hostbridge->dev.driver) {
		device_unlock(&hostbridge->dev);
		return -ENODEV;
	}

	/* No switch + found root port means we're done */
	if (!ctx.has_switch) {
		parent_port = to_cxl_port(&hostbridge->dev);
		goto out;
	}

	/* Walk down from the root port and add all switches */
	cxl_scan_ports(ctx.root_port);

	/* If parent is a dport the endpoint is good to go. */
	parent_port = to_cxl_port(dev->parent->parent);
	if (!cxl_find_dport_by_dev(parent_port, dev->parent)) {
		rc = -ENODEV;
		goto err_out;
	}

out:

	rc = create_endpoint(dev, parent_port, ctx.root_port);
	if (rc)
		goto err_out;

	cxlmd->root_port = ctx.root_port;

err_out:
	device_unlock(&hostbridge->dev);
	return rc;
}

static struct cxl_driver cxl_mem_driver = {
	.name = "cxl_mem",
	.probe = cxl_mem_probe,
	.id = CXL_DEVICE_MEMORY_EXPANDER,
};

module_cxl_driver(cxl_mem_driver);

MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(CXL);
MODULE_ALIAS_CXL(CXL_DEVICE_MEMORY_EXPANDER);
MODULE_SOFTDEP("pre: cxl_port");
