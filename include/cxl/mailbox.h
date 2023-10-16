/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation. */
#ifndef __CXL_MBOX_H__
#define __CXL_MBOX_H__
#include <linux/rcuwait.h>
#include <uapi/linux/cxl_mem.h>

/* Device enabled DCD commands */
enum dcd_cmd_enabled_bits {
	CXL_DCD_ENABLED_GET_CONFIG,
	CXL_DCD_ENABLED_GET_EXTENT_LIST,
	CXL_DCD_ENABLED_ADD_RESPONSE,
	CXL_DCD_ENABLED_RELEASE,
	CXL_DCD_ENABLED_MAX
};
struct cxl_mbox_cmd;

/**
 * struct cxl_mailbox - context for CXL mailbox operations
 * @host: device that hosts the mailbox
 * @payload_size: Size of space for payload
 *                (CXL 3.1 8.2.8.4.3 Mailbox Capabilities Register)
 * @mbox_mutex: mutex protects device mailbox and firmware
 * @mbox_wait: rcuwait for mailbox
 * @dcd_cmds: List of DCD commands implemented by memory device
 * @enabled_cmds: Hardware commands found enabled in CEL.
 * @exclusive_cmds: Commands that are kernel-internal only
 * @special_irq: Callback to let a specific mailbox instance add a condition to
 *               whether to call the rcuwait_wake_up() on @mbox_wait to indicate
 *               we are done.
 * @get_status: Used to get a memory device status value to use in debug messages.
 * @can_run: Devices incorporating CXL mailboxes may have additional constraints
 *           on what commands may run at a given time. This lets that information
 *           be conveyed to the generic mailbox code.
 * @extra_cmds: When a mailbox is first enumerated the command effects log is
 *              parsed. Some commands detected are specific to particular
 *              CXL components and so are controlled and tracked at that level
 *              rather than in the generic code. This provides the component
 *              specific code with information on which op codes are supported.
 *		Returns if a command was part of such an 'extra' set.
 * @mbox_send: @dev specific transport for transmitting mailbox commands
 */
struct cxl_mailbox {
	struct device *host;
	size_t payload_size;
	struct mutex mbox_mutex; /* lock to protect mailbox context */
	struct rcuwait mbox_wait;
    DECLARE_BITMAP(dcd_cmds, CXL_DCD_ENABLED_MAX);
	DECLARE_BITMAP(enabled_cmds, CXL_MEM_COMMAND_ID_MAX);
	DECLARE_BITMAP(exclusive_cmds, CXL_MEM_COMMAND_ID_MAX);
	bool (*special_irq)(struct cxl_mailbox *mbox, u16 opcode);
	bool (*special_bg)(struct cxl_mailbox *mbox, u16 opcode);
	u64 (*get_status)(struct cxl_mailbox *mbox);
	bool (*can_run)(struct cxl_mailbox *mbox, u16 opcode);
	bool (*extra_cmds)(struct cxl_mailbox *mbox, u16 opcode);
	int (*mbox_send)(struct cxl_mailbox *cxl_mbox, struct cxl_mbox_cmd *cmd);
};

int cxl_mailbox_init(struct cxl_mailbox *cxl_mbox, struct device *host);

#endif
