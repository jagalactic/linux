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
 */
struct cxl_region {
	struct device dev;
	int id;
	unsigned long flags;
#define REGION_DEAD 0
	struct work_struct detach_work;
	struct mutex remove_lock; /* serialize region removal */
};

bool schedule_cxl_region_unregister(struct cxl_region *cxlr);

#endif
