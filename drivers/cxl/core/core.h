/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2020 Intel Corporation. */

#ifndef __CXL_CORE_H__
#define __CXL_CORE_H__

extern const struct device_type cxl_nvdimm_bridge_type;
extern const struct device_type cxl_nvdimm_type;

extern struct attribute_group cxl_base_attribute_group;

extern struct device_attribute dev_attr_create_pmem_region;
extern struct device_attribute dev_attr_delete_region;

struct cxl_send_command;
struct cxl_mem_query_commands;
int cxl_query_cmd(struct cxl_memdev *cxlmd,
		  struct cxl_mem_query_commands __user *q);
int cxl_send_cmd(struct cxl_memdev *cxlmd, struct cxl_send_command __user *s);
void __iomem *devm_cxl_iomap_block(struct device *dev, resource_size_t addr,
				   resource_size_t length);

int cxl_memdev_init(void);
void cxl_memdev_exit(void);
void cxl_mbox_init(void);
void cxl_mbox_exit(void);

#endif /* __CXL_CORE_H__ */
