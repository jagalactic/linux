// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "cxlmem.h"
#include "cxlpci.h"

/**
 * DOC: cxl port
 *
 * The port driver enumerates dport via PCI and scans for HDM
 * (Host-managed-Device-Memory) decoder resources via the
 * @component_reg_phys value passed in by the agent that registered the
 * port. All descendant ports of a CXL root port (described by platform
 * firmware) are managed in this drivers context. Each driver instance
 * is responsible for tearing down the driver context of immediate
 * descendant ports. The locking for this is validated by
 * CONFIG_PROVE_CXL_LOCKING.
 *
 * The primary service this driver provides is presenting APIs to other
 * drivers to utilize the decoders, and indicating to userspace (via bind
 * status) the connectivity of the CXL.mem protocol throughout the
 * PCIe topology.
 */

static void schedule_detach(void *cxlmd)
{
	schedule_cxl_memdev_detach(cxlmd);
}

static int count_decoders(struct device *dev, void *data)
{
	if (is_cxl_decoder(dev))
		(*(int *)data)++;

	return 0;
}

struct dec_init_ctx {
	struct cxl_hdm *cxlhdm;
	int ndx;
};

static int set_decoders(struct device *dev, void *data)
{
	struct cxl_decoder *cxld;
	struct dec_init_ctx *ctx;
	struct cxl_hdm *cxlhdm;
	int dec;

	if (!is_cxl_decoder(dev))
		return 0;

	cxld = to_cxl_decoder(dev);

	ctx = data;

	cxlhdm = ctx->cxlhdm;
	dec = ctx->ndx++;
	cxlhdm->decoders.cxld[dec] = cxld;

	if (cxld->flags & CXL_DECODER_F_ENABLE) {
		dev_dbg(dev, "Not adding to free decoders\n");
		return 0;
	}

	set_bit(dec, cxlhdm->decoders.free_mask);

	dev_dbg(dev, "Adding to free decoder list\n");

	return 0;
}

static int cxl_port_probe(struct device *dev)
{
	struct cxl_port *port = to_cxl_port(dev);
	int rc, decoder_count = 0;
	struct dec_init_ctx ctx;
	struct cxl_hdm *cxlhdm;

	if (is_cxl_endpoint(port)) {
		struct cxl_memdev *cxlmd = to_cxl_memdev(port->uport);

		get_device(&cxlmd->dev);
		rc = devm_add_action_or_reset(dev, schedule_detach, cxlmd);
		if (rc)
			return rc;
	} else {
		rc = devm_cxl_port_enumerate_dports(port);
		if (rc < 0)
			return rc;
		if (rc == 1)
			return devm_cxl_add_passthrough_decoder(port);
	}

	cxlhdm = devm_cxl_setup_hdm(port);
	if (IS_ERR(cxlhdm))
		return PTR_ERR(cxlhdm);

	rc = devm_cxl_enumerate_decoders(cxlhdm);
	if (rc) {
		dev_err(dev, "Couldn't enumerate decoders (%d)\n", rc);
		return rc;
	}

	device_for_each_child(dev, &decoder_count, count_decoders);

	cxlhdm->decoders.free_mask =
		devm_bitmap_zalloc(dev, decoder_count, GFP_KERNEL);
	cxlhdm->decoders.count = decoder_count;

	ctx.cxlhdm = cxlhdm;
	ctx.ndx = 0;
	if (device_for_each_child(dev, &ctx, set_decoders))
		return -ENXIO;

	dev_set_drvdata(dev, cxlhdm);

	dev_dbg(dev, "Setup complete. Free decoders %*pb\n",
		cxlhdm->decoders.count, &cxlhdm->decoders.free_mask);

	return 0;
}

static struct cxl_driver cxl_port_driver = {
	.name = "cxl_port",
	.probe = cxl_port_probe,
	.id = CXL_DEVICE_PORT,
};

module_cxl_driver(cxl_port_driver);
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(CXL);
MODULE_ALIAS_CXL(CXL_DEVICE_PORT);
