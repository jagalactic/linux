/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2021 Intel Corporation. */
#ifndef __CXL_REGION_H__
#define __CXL_REGION_H__

#include <linux/uuid.h>

#include "cxl.h"

/**
 * struct cxl_region - CXL region
 * @dev: This region's device.
 * @id: This regions id. Id is globally unique across all regions.
 * @list: Node in decoder's region list.
 * @res: Resource this region carves out of the platform decode range.
 * @active: If the region has been activated.
 * @staged_list: All decoders staged for programming.
 * @commit_list: All decoders programmed for this region's parameters.
 *
 * @config: HDM decoder program config
 * @config.size: Size of the region determined from LSA or userspace.
 * @config.uuid: The UUID for this region.
 * @config.interleave_ways: Number of interleave ways this region is configured for.
 * @config.interleave_granularity: Interleave granularity of region
 * @config.targets: The memory devices comprising the region.
 */
struct cxl_region {
	struct device dev;
	int id;
	struct list_head list;
	struct resource *res;
	bool active;
	struct list_head staged_list;
	struct list_head commit_list;

	struct {
		u64 size;
		uuid_t uuid;
		int interleave_ways;
		int interleave_granularity;
		struct cxl_memdev *targets[CXL_DECODER_MAX_INTERLEAVE];
	} config;
};

bool is_cxl_region_configured(const struct cxl_region *cxlr);

#endif
