// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/platform_device.h>
#include <linux/genalloc.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include "cxlmem.h"
#include "region.h"
#include "cxl.h"

/**
 * DOC: cxl region
 *
 * This module implements a region driver that is capable of programming CXL
 * hardware to setup regions.
 *
 * A CXL region encompasses a chunk of host physical address space that may be
 * consumed by a single device (x1 interleave aka linear) or across multiple
 * devices (xN interleaved). The region driver has the following
 * responsibilities:
 *
 * * Walk topology to obtain decoder resources for region configuration.
 * * Program decoder resources based on region configuration.
 * * Bridge CXL regions to LIBNVDIMM
 * * Initiates reading and configuring LSA regions
 * * Enumerates regions created by BIOS (typically volatile)
 */

#define region_ways(region) ((region)->config.interleave_ways)
#define region_granularity(region) ((region)->config.interleave_granularity)

static struct cxl_decoder *rootd_from_region(struct cxl_region *cxlr)
{
	struct device *d = cxlr->dev.parent;

	if (WARN_ONCE(!is_root_decoder(d),
		      "Corrupt topology for root region\n"))
		return NULL;

	return to_cxl_decoder(d);
}

static struct cxl_port *get_hostbridge(const struct cxl_memdev *ep)
{
	struct cxl_port *port = ep->port;

	while (!is_cxl_root(port)) {
		port = to_cxl_port(port->dev.parent);
		if (port->depth == 1)
			return port;
	}

	BUG();
	return NULL;
}

static struct cxl_port *get_root_decoder(const struct cxl_memdev *endpoint)
{
	struct cxl_port *hostbridge = get_hostbridge(endpoint);

	if (hostbridge)
		return to_cxl_port(hostbridge->dev.parent);

	return NULL;
}

static void release_cxl_region(void *r)
{
	struct cxl_region *cxlr = (struct cxl_region *)r;
	struct cxl_decoder *rootd = rootd_from_region(cxlr);
	struct resource *res = &rootd->platform_res;
	resource_size_t start, size;

	start = cxlr->res->start;
	size = resource_size(cxlr->res);

	__release_region(res, start, size);
	gen_pool_free(rootd->address_space, start, size);
}

/**
 * sanitize_region() - Check is region is reasonably configured
 * @cxlr: The region to check
 *
 * Determination as to whether or not a region can possibly be configured is
 * described in CXL Memory Device SW Guide. In order to implement the algorithms
 * described there, certain more basic configuration parameters must first need
 * to be validated. That is accomplished by this function.
 *
 * Returns 0 if the region is reasonably configured, else returns a negative
 * error code.
 */
static int sanitize_region(const struct cxl_region *cxlr)
{
	const int ig = region_granularity(cxlr);
	const int iw = region_ways(cxlr);
	int i;

	if (dev_WARN_ONCE(&cxlr->dev, !is_cxl_region_configured(cxlr),
			  "unconfigured regions can't be probed (race?)\n")) {
		return -ENXIO;
	}

	/*
	 * Interleave attributes should be caught by later math, but it's
	 * easiest to find those issues here, now.
	 */
	if (!cxl_is_interleave_ways_valid(iw)) {
		dev_dbg(&cxlr->dev, "Invalid number of ways\n");
		return -ENXIO;
	}

	if (!cxl_is_interleave_granularity_valid(ig)) {
		dev_dbg(&cxlr->dev, "Invalid interleave granularity\n");
		return -ENXIO;
	}

	if (cxlr->config.size % (SZ_256M * iw)) {
		dev_dbg(&cxlr->dev, "Invalid size. Must be multiple of %uM\n",
			256 * iw);
		return -ENXIO;
	}

	for (i = 0; i < iw; i++) {
		if (!cxlr->config.targets[i]) {
			dev_dbg(&cxlr->dev, "Missing memory device target%u",
				i);
			return -ENXIO;
		}
		if (!cxlr->config.targets[i]->dev.driver) {
			dev_dbg(&cxlr->dev, "%s isn't CXL.mem capable\n",
				dev_name(&cxlr->config.targets[i]->dev));
			return -ENODEV;
		}
	}

	return 0;
}

/**
 * allocate_address_space() - Gets address space for the region.
 * @cxlr: The region that will consume the address space
 */
static int allocate_address_space(struct cxl_region *cxlr)
{
	struct cxl_decoder *rootd = rootd_from_region(cxlr);
	unsigned long start;

	start = gen_pool_alloc(rootd->address_space, cxlr->config.size);
	if (!start) {
		dev_dbg(&cxlr->dev, "Couldn't allocate %lluM of address space",
			cxlr->config.size >> 20);
		return -ENOMEM;
	}

	cxlr->res =
		__request_region(&rootd->platform_res, start, cxlr->config.size,
				 dev_name(&cxlr->dev), IORESOURCE_MEM);
	if (!cxlr->res) {
		dev_dbg(&cxlr->dev, "Couldn't obtain region from %s (%pR)\n",
			dev_name(&rootd->dev), &rootd->platform_res);
		gen_pool_free(rootd->address_space, start, cxlr->config.size);
		return -ENOMEM;
	}

	dev_dbg(&cxlr->dev, "resource %pR", cxlr->res);

	return devm_add_action_or_reset(&cxlr->dev, release_cxl_region, cxlr);
}

/**
 * find_cdat_dsmas() - Find a valid DSMAS for the region
 * @cxlr: The region
 */
static bool find_cdat_dsmas(const struct cxl_region *cxlr)
{
	return true;
}

/**
 * qtg_match() - Does this root decoder have desirable QTG for the endpoint
 * @rootd: The root decoder for the region
 * @endpoint: Endpoint whose QTG is being compared
 *
 * Prior to calling this function, the caller should verify that all endpoints
 * in the region have the same QTG ID.
 *
 * Returns true if the QTG ID of the root decoder matches the endpoint
 */
static bool qtg_match(const struct cxl_decoder *rootd,
		      const struct cxl_memdev *endpoint)
{
	/* TODO: */
	return true;
}

/**
 * region_xhb_config_valid() - determine cross host bridge validity
 * @cxlr: The region being programmed
 * @rootd: The root decoder to check against
 *
 * The algorithm is outlined in 2.13.14 "Verify XHB configuration sequence" of
 * the CXL Memory Device SW Guide (Rev1p0).
 *
 * Returns true if the configuration is valid.
 */
static bool region_xhb_config_valid(const struct cxl_region *cxlr,
				    const struct cxl_decoder *rootd)
{
	/* TODO: */
	return true;
}

/**
 * region_hb_rp_config_valid() - determine root port ordering is correct
 * @cxlr: Region to validate
 * @rootd: root decoder for this @cxlr
 *
 * The algorithm is outlined in 2.13.15 "Verify HB root port configuration
 * sequence" of the CXL Memory Device SW Guide (Rev1p0).
 *
 * Returns true if the configuration is valid.
 */
static bool region_hb_rp_config_valid(const struct cxl_region *cxlr,
				      const struct cxl_decoder *rootd)
{
	/* TODO: */
	return true;
}

/**
 * rootd_contains() - determine if this region can exist in the root decoder
 * @rootd: root decoder that potentially decodes to this region
 * @cxlr: region to be routed by the @rootd
 */
static bool rootd_contains(const struct cxl_region *cxlr,
			   const struct cxl_decoder *rootd)
{
	/* TODO: */
	return true;
}

static bool rootd_valid(const struct cxl_region *cxlr,
			const struct cxl_decoder *rootd)
{
	const struct cxl_memdev *endpoint = cxlr->config.targets[0];

	if (!qtg_match(rootd, endpoint))
		return false;

	if (!cxl_is_pmem_t3(rootd->flags))
		return false;

	if (!region_xhb_config_valid(cxlr, rootd))
		return false;

	if (!region_hb_rp_config_valid(cxlr, rootd))
		return false;

	if (!rootd_contains(cxlr, rootd))
		return false;

	return true;
}

struct rootd_context {
	const struct cxl_region *cxlr;
	struct cxl_port *hbs[CXL_DECODER_MAX_INTERLEAVE];
	int count;
};

static int rootd_match(struct device *dev, void *data)
{
	struct rootd_context *ctx = (struct rootd_context *)data;
	const struct cxl_region *cxlr = ctx->cxlr;

	if (!is_root_decoder(dev))
		return 0;

	return !!rootd_valid(cxlr, to_cxl_decoder(dev));
}

/*
 * This is a roughly equivalent implementation to "Figure 45 - High-level
 * sequence: Finding CFMWS for region" from the CXL Memory Device SW Guide
 * Rev1p0.
 */
static struct cxl_decoder *find_rootd(const struct cxl_region *cxlr,
				      const struct cxl_port *root)
{
	struct rootd_context ctx;
	struct device *ret;

	ctx.cxlr = cxlr;

	ret = device_find_child((struct device *)&root->dev, &ctx, rootd_match);
	if (ret)
		return to_cxl_decoder(ret);

	return NULL;
}

static int collect_ep_decoders(const struct cxl_region *cxlr)
{
	/* TODO: */
	return 0;
}

static int bind_region(const struct cxl_region *cxlr)
{
	/* TODO: */
	return 0;
}

static int cxl_region_probe(struct device *dev)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_port *root_port;
	struct cxl_decoder *rootd, *ours;
	int ret;

	device_lock_assert(&cxlr->dev);

	if (cxlr->active)
		return 0;

	if (uuid_is_null(&cxlr->config.uuid))
		uuid_gen(&cxlr->config.uuid);

	/* TODO: What about volatile, and LSA generated regions? */

	ret = sanitize_region(cxlr);
	if (ret)
		return ret;

	ret = allocate_address_space(cxlr);
	if (ret)
		return ret;

	if (!find_cdat_dsmas(cxlr))
		return -ENXIO;

	rootd = rootd_from_region(cxlr);
	if (!rootd) {
		dev_err(dev, "Couldn't find root decoder\n");
		return -ENXIO;
	}

	if (!rootd_valid(cxlr, rootd)) {
		dev_err(dev, "Picked invalid rootd\n");
		return -ENXIO;
	}

	root_port = get_root_decoder(cxlr->config.targets[0]);
	ours = find_rootd(cxlr, root_port);
	if (ours != rootd)
		dev_dbg(dev, "Picked different rootd %s %s\n",
			dev_name(&rootd->dev), dev_name(&ours->dev));
	if (ours)
		put_device(&ours->dev);

	ret = collect_ep_decoders(cxlr);
	if (ret)
		return ret;

	ret = bind_region(cxlr);
	if (!ret) {
		cxlr->active = true;
		dev_info(dev, "Bound");
	}

	return ret;
}

static struct cxl_driver cxl_region_driver = {
	.name = "cxl_region",
	.probe = cxl_region_probe,
	.id = CXL_DEVICE_REGION,
};
module_cxl_driver(cxl_region_driver);

MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(CXL);
MODULE_ALIAS_CXL(CXL_DEVICE_REGION);
