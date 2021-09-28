// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "cxlmem.h"

/**
 * DOC: cxl port
 *
 * The port driver implements the set of functionality needed to allow full
 * decoder enumeration and routing. A CXL port is an abstraction of a CXL
 * component that implements some amount of CXL decoding of CXL.mem traffic.
 * As of the CXL 2.0 spec, this includes:
 *
 *	.. list-table:: CXL Components w/ Ports
 *		:widths: 25 25 50
 *		:header-rows: 1
 *
 *		* - component
 *		  - upstream
 *		  - downstream
 *		* - Hostbridge
 *		  - ACPI0016
 *		  - root port
 *		* - Switch
 *		  - Switch Upstream Port
 *		  - Switch Downstream Port
 *		* - Endpoint (not yet implemented)
 *		  - Endpoint Port
 *		  - N/A
 *
 * The primary service this driver provides is enumerating HDM decoders and
 * presenting APIs to other drivers to utilize the decoders.
 */

struct cxl_port_data {
	struct cxl_component_regs regs;

	struct port_caps {
		unsigned int count;
		unsigned int tc;
		unsigned int interleave11_8;
		unsigned int interleave14_12;
	} caps;
};

static inline int cxl_hdm_decoder_ig(u32 ctrl)
{
	int val = FIELD_GET(CXL_HDM_DECODER0_CTRL_IG_MASK, ctrl);

	return 8 + val;
}

static inline int cxl_hdm_decoder_iw(u32 ctrl)
{
	int val = FIELD_GET(CXL_HDM_DECODER0_CTRL_IW_MASK, ctrl);

	return 1 << val;
}

static void get_caps(struct cxl_port *port, struct cxl_port_data *cpd)
{
	void __iomem *hdm_decoder = cpd->regs.hdm_decoder;
	struct port_caps *caps = &cpd->caps;
	u32 hdm_cap;

	hdm_cap = readl(hdm_decoder + CXL_HDM_DECODER_CAP_OFFSET);

	caps->count = cxl_hdm_decoder_count(hdm_cap);
	caps->tc = FIELD_GET(CXL_HDM_DECODER_TARGET_COUNT_MASK, hdm_cap);
	caps->interleave11_8 =
		FIELD_GET(CXL_HDM_DECODER_INTERLEAVE_11_8, hdm_cap);
	caps->interleave14_12 =
		FIELD_GET(CXL_HDM_DECODER_INTERLEAVE_14_12, hdm_cap);
}

static int map_regs(struct cxl_port *port, void __iomem *crb,
		    struct cxl_port_data *cpd)
{
	struct cxl_register_map map;
	struct cxl_component_reg_map *comp_map = &map.component_map;

	cxl_probe_component_regs(&port->dev, crb, comp_map);
	if (!comp_map->hdm_decoder.valid) {
		dev_err(&port->dev, "HDM decoder registers invalid\n");
		return -ENXIO;
	}

	cpd->regs.hdm_decoder = crb + comp_map->hdm_decoder.offset;

	return 0;
}

static u64 get_decoder_size(void __iomem *hdm_decoder, int n)
{
	u32 ctrl = readl(hdm_decoder + CXL_HDM_DECODER0_CTRL_OFFSET(n));

	if (!!FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
		return 0;

	return ioread64_hi_lo(hdm_decoder +
			      CXL_HDM_DECODER0_SIZE_LOW_OFFSET(n));
}

static bool is_endpoint_port(struct cxl_port *port)
{
	if (!port->uport->driver)
		return false;

	return to_cxl_drv(port->uport->driver)->id ==
	       CXL_DEVICE_MEMORY_EXPANDER;
}

static int enumerate_hdm_decoders(struct cxl_port *port,
				  struct cxl_port_data *portdata)
{
	int i = 0;

	for (i = 0; i < portdata->caps.count; i++) {
		void __iomem *hdm_decoder = portdata->regs.hdm_decoder;
		int rc, target_count = portdata->caps.tc;
		struct cxl_decoder *cxld;
		int *target_map = NULL;
		u64 size;

		if (is_endpoint_port(port))
			target_count = 0;

		cxld = cxl_decoder_alloc(port, target_count);
		if (IS_ERR(cxld)) {
			dev_warn(&port->dev,
				 "Failed to allocate the decoder\n");
			return PTR_ERR(cxld);
		}

		cxld->decoder_res = NULL;
		cxld->target_type = CXL_DECODER_EXPANDER;
		cxld->interleave_ways = 1;
		cxld->interleave_granularity = 0;

		size = get_decoder_size(hdm_decoder, i);
		if (size != 0) {
			int temp[CXL_DECODER_MAX_INTERLEAVE];
			struct cxl_decoder *cfmws;
			u64 target_list, base;
			u32 ctrl;
			int j;

			target_map = temp;
			ctrl = readl(hdm_decoder + CXL_HDM_DECODER0_CTRL_OFFSET(i));
			base = ioread64_hi_lo(hdm_decoder + CXL_HDM_DECODER0_BASE_LOW_OFFSET(i));

			cfmws = cxl_find_cfmws(base, size);
			if (!cfmws) {
				dev_err(&port->dev,
					"No CFMWS entry found for decoder\n");
				put_device(&cxld->dev);
				continue;
			}

			cxld->decoder_res =
				__request_region(&cfmws->cfmws_res, base, size,
						 "cxld", IORESOURCE_MEM);
			if (!cxld->decoder_res) {
				dev_err(&port->dev,
					"Failed to request resource (%d)\n",
					rc);
				put_device(&cxld->dev);
				continue;
			}

			cxld->flags = CXL_DECODER_F_EN;
			cxld->interleave_ways = cxl_hdm_decoder_iw(ctrl);
			cxld->interleave_granularity = cxl_hdm_decoder_ig(ctrl);

			if (FIELD_GET(CXL_HDM_DECODER0_CTRL_TYPE, ctrl) == 0)
				cxld->target_type = CXL_DECODER_ACCELERATOR;

			target_list = ioread64_hi_lo(hdm_decoder + CXL_HDM_DECODER0_TL_LOW(i));
			for (j = 0; j < cxld->interleave_ways; j++)
				target_map[j] = (target_list >> (j * 8)) & 0xff;
		}

		rc = cxl_decoder_add_locked(cxld, target_map);
		if (rc)
			put_device(&cxld->dev);
		else
			rc = cxl_decoder_autoremove(&port->dev, cxld);
		if (rc)
			dev_err(&port->dev, "Failed to add decoder\n");
	}

	return 0;
}

static int cxl_port_probe(struct device *dev)
{
	struct cxl_port *port = to_cxl_port(dev);
	struct cxl_port_data *portdata;
	void __iomem *crb;
	u32 ctrl;
	int rc;

	if (port->component_reg_phys == CXL_RESOURCE_NONE)
		return 0;

	portdata = devm_kzalloc(dev, sizeof(*portdata), GFP_KERNEL);
	if (!portdata)
		return -ENOMEM;

	crb = devm_cxl_iomap_block(&port->dev, port->component_reg_phys,
				   CXL_COMPONENT_REG_BLOCK_SIZE);
	if (IS_ERR_OR_NULL(crb)) {
		dev_err(&port->dev, "No component registers mapped\n");
		return -ENXIO;
	}

	rc = map_regs(port, crb, portdata);
	if (rc)
		return rc;

	get_caps(port, portdata);
	if (portdata->caps.count == 0) {
		dev_err(&port->dev, "Spec violation. Caps invalid\n");
		return -ENXIO;
	}

	/*
	 * Enable HDM decoders for this port.
	 *
	 * FIXME: If the component was using DVSEC range registers for decode,
	 * this will destroy that.
	 */
	ctrl = readl(portdata->regs.hdm_decoder + CXL_HDM_DECODER_CTRL_OFFSET);
	ctrl |= CXL_HDM_DECODER_ENABLE;
	writel(ctrl, portdata->regs.hdm_decoder + CXL_HDM_DECODER_CTRL_OFFSET);

	rc = enumerate_hdm_decoders(port, portdata);
	if (rc) {
		dev_err(&port->dev, "Couldn't enumerate decoders (%d)\n", rc);
		return rc;
	}

	dev_set_drvdata(dev, portdata);
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
