/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2021 Intel Corporation. */
#ifndef __CXL_REGION_H__
#define __CXL_REGION_H__

#include <linux/uuid.h>

#include "cxl.h"

/**
 * struct cxl_region - CXL region
 * @dev: This region's device.
 * @id: This region's id. Id is globally unique across all regions.
 * @flags: Flags representing the current state of the region.
 * @detach_work: Async unregister to allow attrs to take device_lock.
 * @remove_lock: Coordinates region removal against decoder removal
 * @list: Node in decoder's region list.
 * @res: Resource this region carves out of the platform decode range.
 * @size: Size of the region determined from LSA or userspace.
 * @uuid: The UUID for this region.
 * @interleave_ways: Number of interleave ways this region is configured for.
 * @interleave_granularity: Interleave granularity of region
 * @targets: The memory devices comprising the region.
 */
struct cxl_region {
	struct device dev;
	int id;
	unsigned long flags;
#define REGION_DEAD 0
	struct work_struct detach_work;
	struct mutex remove_lock; /* serialize region removal */

	struct list_head list;
	struct resource *res;

	u64 size;
	uuid_t uuid;
	int interleave_ways;
	int interleave_granularity;
	struct cxl_endpoint_decoder *targets[CXL_DECODER_MAX_INTERLEAVE];
};

bool is_cxl_region(struct device *dev);
struct cxl_region *to_cxl_region(struct device *dev);
bool schedule_cxl_region_unregister(struct cxl_region *cxlr);

static inline bool cxl_is_interleave_ways_valid(const struct cxl_region *cxlr,
						const struct cxl_decoder *rootd,
						u8 ways)
{
	int root_ig, region_ig, root_eniw;

	switch (ways) {
	case 0 ... 4:
	case 6:
	case 8:
	case 12:
	case 16:
		break;
	default:
		return false;
	}

	if (rootd->interleave_ways == 1)
		return true;

	root_ig = cxl_from_granularity(rootd->interleave_granularity);
	region_ig = cxl_from_granularity(cxlr->interleave_granularity);
	root_eniw = cxl_from_ways(rootd->interleave_ways);

	return ((1 << (root_ig - region_ig)) * (1 << root_eniw)) <= ways;
}

static inline bool
cxl_is_interleave_granularity_valid(const struct cxl_decoder *rootd, int ig)
{
	int rootd_hbig;

	if (!is_power_of_2(ig))
		return false;

	/* 16K is the max */
	if (ig >> 15)
		return false;

	rootd_hbig = cxl_from_granularity(rootd->interleave_granularity);
	if (rootd_hbig < cxl_from_granularity(ig))
		return false;

	return true;
}

#endif
