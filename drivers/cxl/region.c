// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/platform_device.h>
#include <linux/genalloc.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/sort.h>
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
#define region_eniw(region) (cxl_to_eniw(region_ways(region)))
#define region_ig(region) (cxl_to_ig(region_granularity(region)))

#define for_each_cxl_endpoint(ep, region, idx)                                 \
	for (idx = 0, ep = (region)->config.targets[idx];                      \
	     idx < region_ways(region); ep = (region)->config.targets[++idx])

#define for_each_cxl_endpoint_hb(ep, region, hb, idx)                          \
	for (idx = 0, (ep) = (region)->config.targets[idx];                    \
	     idx < region_ways(region);                                        \
	     idx++, (ep) = (region)->config.targets[idx])                      \
		if (get_hostbridge(ep) == (hb))

#define for_each_cxl_decoder_target(dport, decoder, idx)                       \
	for (idx = 0, dport = (decoder)->target[idx];                          \
	     idx < (decoder)->nr_targets - 1;                                  \
	     dport = (decoder)->target[++idx])

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

static int get_unique_hostbridges(const struct cxl_region *cxlr,
				  struct cxl_port **hbs)
{
	struct cxl_memdev *ep;
	int i, hb_count = 0;

	for_each_cxl_endpoint(ep, cxlr, i) {
		struct cxl_port *hb = get_hostbridge(ep);
		bool found = false;
		int j;

		BUG_ON(!hb);

		for (j = 0; j < hb_count; j++) {
			if (hbs[j] == hb)
				found = true;
		}
		if (!found)
			hbs[hb_count++] = hb;
	}

	return hb_count;
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
	const int rootd_eniw = cxl_to_eniw(rootd->interleave_ways);
	const int rootd_ig = cxl_to_ig(rootd->interleave_granularity);
	const int cxlr_ig = region_ig(cxlr);
	const int cxlr_iw = region_ways(cxlr);
	struct cxl_port *hbs[CXL_DECODER_MAX_INTERLEAVE];
	struct cxl_dport *target;
	int i;

	i = get_unique_hostbridges(cxlr, hbs);
	if (dev_WARN_ONCE(&cxlr->dev, i == 0, "Cannot find a valid host bridge\n"))
		return false;

	/* Are all devices in this region on the same CXL host bridge */
	if (i == 1)
		return true;

	/* CFMWS.HBIG >= Device.Label.IG */
	if (rootd_ig < cxlr_ig) {
		dev_dbg(&cxlr->dev,
			"%s HBIG must be greater than region IG (%d < %d)\n",
			dev_name(&rootd->dev), rootd_ig, cxlr_ig);
		return false;
	}

	/*
	 * ((2^(CFMWS.HBIG - Device.RLabel.IG) * (2^CFMWS.ENIW)) > Device.RLabel.NLabel)
	 *
	 * XXX: 2^CFMWS.ENIW is trying to decode the NIW. Instead, use the look
	 * up function which supports non power of 2 interleave configurations.
	 */
	if (((1 << (rootd_ig - cxlr_ig)) * (1 << rootd_eniw)) > cxlr_iw) {
		dev_dbg(&cxlr->dev,
			"granularity ratio requires a larger number of devices (%d) than currently configured (%d)\n",
			((1 << (rootd_ig - cxlr_ig)) * (1 << rootd_eniw)),
			cxlr_iw);
		return false;
	}

	/*
	 * CFMWS.InterleaveTargetList[n] must contain all devices, x where:
	 *	(Device[x],RegionLabel.Position >> (CFMWS.HBIG -
	 *	Device[x].RegionLabel.InterleaveGranularity)) &
	 *	((2^CFMWS.ENIW) - 1) = n
	 */
	for_each_cxl_decoder_target(target, rootd, i) {
		if (((i >> (rootd_ig - cxlr_ig))) &
		    (((1 << rootd_eniw) - 1) != target->port_id)) {
			dev_dbg(&cxlr->dev,
				"One or more devices are not connected to the correct hostbridge.\n");
			return false;
		}
	}

	return true;
}

static struct cxl_dport *get_rp(struct cxl_memdev *ep)
{
	struct cxl_port *port, *parent_port = port = ep->port;
	struct cxl_dport *dport;

	while (!is_cxl_root(port)) {
		parent_port = to_cxl_port(port->dev.parent);
		if (parent_port->depth == 1)
			list_for_each_entry(dport, &parent_port->dports, list)
				if (dport->dport == port->uport->parent->parent)
					return dport;
		port = parent_port;
	}

	BUG();
	return NULL;
}

static int get_num_root_ports(const struct cxl_region *cxlr)
{
	struct cxl_memdev *endpoint;
	struct cxl_dport *dport, *tmp;
	int num_root_ports = 0;
	LIST_HEAD(root_ports);
	int idx;

	for_each_cxl_endpoint(endpoint, cxlr, idx) {
		struct cxl_dport *root_port = get_rp(endpoint);

		if (list_empty(&root_port->verify_link)) {
			list_add_tail(&root_port->verify_link, &root_ports);
			num_root_ports++;
		}
	}

	list_for_each_entry_safe(dport, tmp, &root_ports, verify_link)
		list_del_init(&dport->verify_link);

	return num_root_ports;
}

static bool has_switch(const struct cxl_region *cxlr)
{
	struct cxl_memdev *ep;
	int i;

	for_each_cxl_endpoint(ep, cxlr, i)
		if (ep->port->depth > 2)
			return true;

	return false;
}

static struct cxl_decoder *get_decoder(struct cxl_region *cxlr,
				       struct cxl_port *p)
{
	struct cxl_decoder *cxld;

	cxld = cxl_get_decoder(p);
	if (IS_ERR(cxld)) {
		dev_dbg(&cxlr->dev, "Couldn't get decoder for %s\n",
			dev_name(&p->dev));
		return cxld;
	}

	cxld->decoder_range = (struct range){ .start = cxlr->res->start,
					      .end = cxlr->res->end };

	list_add_tail(&cxld->region_link,
		      (struct list_head *)&cxlr->staged_list);

	return cxld;
}

static bool simple_config(struct cxl_region *cxlr, struct cxl_port *hb)
{
	struct cxl_decoder *cxld;

	cxld = get_decoder(cxlr, hb);
	if (IS_ERR(cxld))
		return false;

	cxld->interleave_ways = 1;
	cxld->interleave_granularity = region_granularity(cxlr);
	cxld->target[0] = get_rp(cxlr->config.targets[0]);
	return true;
}

/**
 * region_hb_rp_config_valid() - determine root port ordering is correct
 * @cxlr: Region to validate
 * @rootd: root decoder for this @cxlr
 * @state_update: Whether or not to update port state
 *
 * The algorithm is outlined in 2.13.15 "Verify HB root port configuration
 * sequence" of the CXL Memory Device SW Guide (Rev1p0).
 *
 * Returns true if the configuration is valid.
 */
static bool region_hb_rp_config_valid(struct cxl_region *cxlr,
				      const struct cxl_decoder *rootd,
				      bool state_update)
{
	const int num_root_ports = get_num_root_ports(cxlr);
	struct cxl_port *hbs[CXL_DECODER_MAX_INTERLEAVE];
	struct cxl_decoder *cxld, *c;
	int hb_count, i;

	hb_count = get_unique_hostbridges(cxlr, hbs);

	/* TODO: Switch support */
	if (has_switch(cxlr))
		return false;

	/*
	 * Are all devices in this region on the same CXL Host Bridge
	 * Root Port?
	 */
	if (num_root_ports == 1 && !has_switch(cxlr) && state_update)
		return simple_config(cxlr, hbs[0]);

	for (i = 0; i < hb_count; i++) {
		int idx, position_mask;
		struct cxl_dport *rp;
		struct cxl_port *hb;

		/* Get next CXL Host Bridge this region spans */
		hb = hbs[i];

		if (state_update) {
			cxld = get_decoder(cxlr, hb);
			if (IS_ERR(cxld)) {
				dev_dbg(&cxlr->dev,
					"Couldn't get decoder for %s\n",
					dev_name(&hb->dev));
				goto err;
			}
			cxld->interleave_ways = 0;
			cxld->interleave_granularity = region_granularity(cxlr);
		} else {
			cxld = NULL;
		}

		/*
		 * Calculate the position mask: NumRootPorts = 2^PositionMask
		 * for this region.
		 *
		 * XXX: pos_mask is actually (1 << PositionMask)  - 1
		 */
		position_mask = (1 << (ilog2(num_root_ports))) - 1;

		/*
		 * Calculate the PortGrouping for each device on this CXL Host
		 * Bridge Root Port:
		 * PortGrouping = RegionLabel.Position & PositionMask
		 *
		 * The following nest iterators effectively iterate over each
		 * root port in the region.
		 *   for_each_unique_rootport(rp, cxlr)
		 */
		list_for_each_entry(rp, &hb->dports, list) {
			struct cxl_memdev *ep;
			int port_grouping = -1;

			for_each_cxl_endpoint_hb(ep, cxlr, hb, idx) {
				if (get_rp(ep) != rp)
					continue;

				if (port_grouping == -1)
					port_grouping = idx & position_mask;

				/*
				 * Do all devices in the region connected to this CXL
				 * Host Bridge Root Port have the same PortGrouping?
				 */
				if ((idx & position_mask) != port_grouping) {
					dev_dbg(&cxlr->dev,
						"One or more devices are not connected to the correct Host Bridge Root Port\n");
					goto err;
				}
			}
		}
	}

	return true;

err:
	dev_dbg(&cxlr->dev, "Couldn't get decoder for region\n");
	list_for_each_entry_safe(cxld, c, &cxlr->staged_list, region_link)
		cxl_put_decoder(cxld);

	return false;
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
			const struct cxl_decoder *rootd, bool state_update)
{
	const struct cxl_memdev *endpoint = cxlr->config.targets[0];

	if (!qtg_match(rootd, endpoint))
		return false;

	if (!cxl_is_pmem_t3(rootd->flags))
		return false;

	if (!region_xhb_config_valid(cxlr, rootd))
		return false;

	if (!region_hb_rp_config_valid((struct cxl_region *)cxlr, rootd,
				       state_update))
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

	return !!rootd_valid(cxlr, to_cxl_decoder(dev), false);
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

static void cleanup_staged_decoders(struct cxl_region *cxlr)
{
	struct cxl_decoder *cxld, *d;

	list_for_each_entry_safe(cxld, d, &cxlr->staged_list, region_link) {
		cxl_put_decoder(cxld);
		list_del_init(&cxld->region_link);
	}
}

static int collect_ep_decoders(struct cxl_region *cxlr)
{
	struct cxl_memdev *ep;
	int i, rc = 0;

	for_each_cxl_endpoint(ep, cxlr, i) {
		struct cxl_decoder *cxld;

		cxld = get_decoder(cxlr, ep->port);
		if (IS_ERR(cxld)) {
			rc = PTR_ERR(cxld);
			goto err;
		}

		cxld->interleave_granularity = region_granularity(cxlr);
		cxld->interleave_ways = region_ways(cxlr);
	}

	return 0;

err:
	cleanup_staged_decoders(cxlr);
	return rc;
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

	if (!rootd_valid(cxlr, rootd, true)) {
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
		goto err;

	ret = bind_region(cxlr);
	if (ret)
		goto err;

	cxlr->active = true;
	dev_info(dev, "Bound");
	return 0;

err:
	cleanup_staged_decoders(cxlr);
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
