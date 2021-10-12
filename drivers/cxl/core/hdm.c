// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/device.h>
#include <linux/delay.h>

#include "cxlmem.h"
#include "core.h"

/**
 * DOC: cxl core hdm
 *
 * Compute Express Link Host Managed Device Memory, starting with the
 * CXL 2.0 specification, is managed by an array of HDM Decoder register
 * instances per CXL port and per CXL endpoint. Define common helpers
 * for enumerating these registers and capabilities.
 */

static int add_hdm_decoder(struct cxl_port *port, struct cxl_decoder *cxld,
			   int *target_map)
{
	int rc;

	rc = cxl_decoder_add_locked(cxld, target_map);
	if (rc) {
		put_device(&cxld->dev);
		dev_err(&port->dev, "Failed to add decoder\n");
		return rc;
	}

	rc = cxl_decoder_autoremove(&port->dev, cxld);
	if (rc)
		return rc;

	dev_dbg(&cxld->dev, "Added to port %s\n", dev_name(&port->dev));

	return 0;
}

/*
 * Per the CXL specification (8.2.5.12 CXL HDM Decoder Capability Structure)
 * single ported host-bridges need not publish a decoder capability when a
 * passthrough decode can be assumed, i.e. all transactions that the uport sees
 * are claimed and passed to the single dport. Disable the range until the first
 * CXL region is enumerated / activated.
 */
int devm_cxl_add_passthrough_decoder(struct cxl_port *port)
{
	struct cxl_decoder *cxld;
	struct cxl_dport *dport;
	int single_port_map[1];

	cxld = cxl_switch_decoder_alloc(port, 1);
	if (IS_ERR(cxld))
		return PTR_ERR(cxld);

	device_lock_assert(&port->dev);

	dport = list_first_entry(&port->dports, typeof(*dport), list);
	single_port_map[0] = dport->port_id;

	return add_hdm_decoder(port, cxld, single_port_map);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_passthrough_decoder, CXL);

static void parse_hdm_decoder_caps(struct cxl_hdm *cxlhdm)
{
	u32 hdm_cap;

	hdm_cap = readl(cxlhdm->regs.hdm_decoder + CXL_HDM_DECODER_CAP_OFFSET);
	cxlhdm->decoder_count = cxl_hdm_decoder_count(hdm_cap);
	cxlhdm->target_count =
		FIELD_GET(CXL_HDM_DECODER_TARGET_COUNT_MASK, hdm_cap);
	if (FIELD_GET(CXL_HDM_DECODER_INTERLEAVE_11_8, hdm_cap))
		cxlhdm->interleave_mask |= GENMASK(11, 8);
	if (FIELD_GET(CXL_HDM_DECODER_INTERLEAVE_14_12, hdm_cap))
		cxlhdm->interleave_mask |= GENMASK(14, 12);
}

static void __iomem *map_hdm_decoder_regs(struct cxl_port *port,
					  void __iomem *crb)
{
	struct cxl_register_map map;
	struct cxl_component_reg_map *comp_map = &map.component_map;

	cxl_probe_component_regs(&port->dev, crb, comp_map);
	if (!comp_map->hdm_decoder.valid) {
		dev_err(&port->dev, "HDM decoder registers invalid\n");
		return IOMEM_ERR_PTR(-ENXIO);
	}

	return crb + comp_map->hdm_decoder.offset;
}

/**
 * devm_cxl_setup_hdm - map HDM decoder component registers
 * @port: cxl_port to map
 */
struct cxl_hdm *devm_cxl_setup_hdm(struct cxl_port *port)
{
	struct device *dev = &port->dev;
	void __iomem *crb, *hdm;
	struct cxl_hdm *cxlhdm;

	cxlhdm = devm_kzalloc(dev, sizeof(*cxlhdm), GFP_KERNEL);
	if (!cxlhdm)
		return ERR_PTR(-ENOMEM);

	cxlhdm->port = port;
	crb = devm_cxl_iomap_block(dev, port->component_reg_phys,
				   CXL_COMPONENT_REG_BLOCK_SIZE);
	if (!crb) {
		dev_err(dev, "No component registers mapped\n");
		return ERR_PTR(-ENXIO);
	}

	hdm = map_hdm_decoder_regs(port, crb);
	if (IS_ERR(hdm))
		return ERR_CAST(hdm);
	cxlhdm->regs.hdm_decoder = hdm;

	parse_hdm_decoder_caps(cxlhdm);
	if (cxlhdm->decoder_count == 0) {
		dev_err(dev, "Spec violation. Caps invalid\n");
		return ERR_PTR(-ENXIO);
	}

	return cxlhdm;
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_setup_hdm, CXL);

static int to_interleave_granularity(u32 ctrl)
{
	int val = FIELD_GET(CXL_HDM_DECODER0_CTRL_IG_MASK, ctrl);

	return cxl_to_interleave_granularity(val);
}

static int to_interleave_ways(u32 ctrl)
{
	int val = FIELD_GET(CXL_HDM_DECODER0_CTRL_IW_MASK, ctrl);

	return cxl_to_interleave_ways(val);
}

static int init_hdm_decoder(struct cxl_port *port, struct cxl_decoder *cxld,
			    int *target_map, void __iomem *hdm, int which)
{
	u64 size, base;
	u32 ctrl;
	int i;
	union {
		u64 value;
		unsigned char target_id[8];
	} target_list;

	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(which));
	base = ioread64_hi_lo(hdm + CXL_HDM_DECODER0_BASE_LOW_OFFSET(which));
	size = ioread64_hi_lo(hdm + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(which));

	if (!(ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED))
		size = 0;
	if (base == U64_MAX || size == U64_MAX) {
		dev_warn(&port->dev, "decoder%d.%d: Invalid resource range\n",
			 port->id, cxld->id);
		return -ENXIO;
	}

	cxld->decoder_range = (struct range) {
		.start = base,
		.end = base + size - 1,
	};

	/* switch decoders are always enabled if committed */
	if (ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED) {
		cxld->flags |= CXL_DECODER_F_ENABLE;
		if (ctrl & CXL_HDM_DECODER0_CTRL_LOCK)
			cxld->flags |= CXL_DECODER_F_LOCK;
	}
	cxld->interleave_ways = to_interleave_ways(ctrl);
	if (!cxld->interleave_ways) {
		dev_warn(&port->dev,
			 "decoder%d.%d: Invalid interleave ways (ctrl: %#x)\n",
			 port->id, cxld->id, ctrl);
		return -ENXIO;
	}
	cxld->interleave_granularity = to_interleave_granularity(ctrl);

	if (FIELD_GET(CXL_HDM_DECODER0_CTRL_TYPE, ctrl))
		cxld->target_type = CXL_DECODER_EXPANDER;
	else
		cxld->target_type = CXL_DECODER_ACCELERATOR;

	if (is_cxl_endpoint(to_cxl_port(cxld->dev.parent)))
		return 0;

	target_list.value =
		ioread64_hi_lo(hdm + CXL_HDM_DECODER0_TL_LOW(which));
	for (i = 0; i < cxld->interleave_ways; i++)
		target_map[i] = target_list.target_id[i];

	return 0;
}

/**
 * devm_cxl_enumerate_decoders - add decoder objects per HDM register set
 * @cxlhdm: Structure to populate with HDM capabilities
 */
int devm_cxl_enumerate_decoders(struct cxl_hdm *cxlhdm)
{
	void __iomem *hdm = cxlhdm->regs.hdm_decoder;
	struct cxl_port *port = cxlhdm->port;
	int i, committed, failed;
	u32 ctrl;

	/*
	 * Since the register resource was recently claimed via request_region()
	 * be careful about trusting the "not-committed" status until the commit
	 * timeout has elapsed.  The commit timeout is 10ms (CXL 2.0
	 * 8.2.5.12.20), but double it to be tolerant of any clock skew between
	 * host and target.
	 */
	for (i = 0, committed = 0; i < cxlhdm->decoder_count; i++) {
		ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(i));
		if (ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED)
			committed++;
	}

	/* ensure that future checks of committed can be trusted */
	if (committed != cxlhdm->decoder_count)
		msleep(20);

	for (i = 0, failed = 0; i < cxlhdm->decoder_count; i++) {
		int target_map[CXL_DECODER_MAX_INTERLEAVE] = { 0 };
		int rc, target_count = cxlhdm->target_count;
		struct cxl_decoder *cxld;

		if (is_cxl_endpoint(port))
			cxld = cxl_endpoint_decoder_alloc(port);
		else
			cxld = cxl_switch_decoder_alloc(port, target_count);
		if (IS_ERR(cxld)) {
			dev_warn(&port->dev,
				 "Failed to allocate the decoder\n");
			return PTR_ERR(cxld);
		}

		rc = init_hdm_decoder(port, cxld, target_map,
				      cxlhdm->regs.hdm_decoder, i);
		if (rc) {
			put_device(&cxld->dev);
			failed++;
			continue;
		}
		rc = add_hdm_decoder(port, cxld, target_map);
		if (rc) {
			dev_warn(&port->dev,
				 "Failed to add decoder to port\n");
			return rc;
		}
	}

	if (failed == cxlhdm->decoder_count) {
		dev_err(&port->dev, "No valid decoders found\n");
		return -ENXIO;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_enumerate_decoders, CXL);

#define COMMIT_TIMEOUT_MS 10
static int wait_for_commit(struct cxl_decoder *cxld)
{
	const unsigned long end = jiffies + msecs_to_jiffies(COMMIT_TIMEOUT_MS);
	struct cxl_port *port = to_cxl_port(cxld->dev.parent);
	void __iomem *hdm_decoder;
	struct cxl_hdm *cxlhdm;
	u32 ctrl;

	cxlhdm = dev_get_drvdata(&port->dev);
	hdm_decoder = cxlhdm->regs.hdm_decoder;

	while (1) {
		ctrl = readl(hdm_decoder +
			     CXL_HDM_DECODER0_CTRL_OFFSET(cxld->id));
		if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
			break;

		if (time_after(jiffies, end)) {
			dev_err(&cxld->dev, "HDM decoder commit timeout %x\n",
				ctrl);
			return -ETIMEDOUT;
		}
		if ((ctrl & CXL_HDM_DECODER0_CTRL_COMMIT_ERROR) != 0) {
			dev_err(&cxld->dev, "HDM decoder commit error %x\n",
				ctrl);
			return -ENXIO;
		}
	}

	return 0;
}

/**
 * cxl_commit_decoder() - Program a configured cxl_decoder
 * @cxld: The preconfigured cxl decoder.
 *
 * A cxl decoder that is to be committed should have been earmarked as enabled.
 * This mechanism acts as a soft reservation on the decoder.
 *
 * Returns 0 if commit was successful, negative error code otherwise.
 */
int cxl_commit_decoder(struct cxl_decoder *cxld)
{
	u32 ctrl, tl_lo, tl_hi, base_lo, base_hi, size_lo, size_hi;
	struct cxl_port *port = to_cxl_port(cxld->dev.parent);
	void __iomem *hdm_decoder;
	struct cxl_hdm *cxlhdm;
	int rc;

	/*
	 * Decoder flags are entirely software controlled and therefore this
	 * case is purely a driver bug.
	 */
	if (dev_WARN_ONCE(&port->dev, (cxld->flags & CXL_DECODER_F_ENABLE) != 0,
			  "Invalid %s enable state\n", dev_name(&cxld->dev)))
		return -ENXIO;

	cxlhdm = dev_get_drvdata(&port->dev);
	hdm_decoder = cxlhdm->regs.hdm_decoder;
	ctrl = readl(hdm_decoder + CXL_HDM_DECODER0_CTRL_OFFSET(cxld->id));

	/*
	 * A decoder that's currently active cannot be changed without the
	 * system being quiesced. While the driver should prevent against this,
	 * for a variety of reasons the hardware might not be in sync with the
	 * hardware and so, do not splat on error.
	 */
	size_hi = readl(hdm_decoder +
			CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(cxld->id));
	size_lo =
		readl(hdm_decoder + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(cxld->id));
	if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl) &&
	    (size_lo + size_hi)) {
		dev_err(&port->dev, "Tried to change an active decoder (%s)\n",
			dev_name(&cxld->dev));
		return -EBUSY;
	}

	u32p_replace_bits(&ctrl, cxl_to_ig(cxld->interleave_granularity),
			  CXL_HDM_DECODER0_CTRL_IG_MASK);
	u32p_replace_bits(&ctrl, cxl_to_eniw(cxld->interleave_ways),
			  CXL_HDM_DECODER0_CTRL_IW_MASK);
	u32p_replace_bits(&ctrl, 1, CXL_HDM_DECODER0_CTRL_COMMIT);

	/* TODO: set based on type */
	u32p_replace_bits(&ctrl, 1, CXL_HDM_DECODER0_CTRL_TYPE);

	base_lo = GENMASK(31, 28) & lower_32_bits(cxld->decoder_range.start);
	base_hi = upper_32_bits(cxld->decoder_range.start);

	size_lo = GENMASK(31, 28) & (u32)(range_len(&cxld->decoder_range));
	size_hi = upper_32_bits(range_len(&cxld->decoder_range) >> 32);

	if (cxld->nr_targets > 0) {
		tl_hi = 0;

		tl_lo = FIELD_PREP(GENMASK(7, 0), cxld->target[0]->port_id);

		if (cxld->interleave_ways > 1)
			tl_lo |= FIELD_PREP(GENMASK(15, 8),
					    cxld->target[1]->port_id);
		if (cxld->interleave_ways > 2)
			tl_lo |= FIELD_PREP(GENMASK(23, 16),
					    cxld->target[2]->port_id);
		if (cxld->interleave_ways > 3)
			tl_lo |= FIELD_PREP(GENMASK(31, 24),
					    cxld->target[3]->port_id);
		if (cxld->interleave_ways > 4)
			tl_hi |= FIELD_PREP(GENMASK(7, 0),
					    cxld->target[4]->port_id);
		if (cxld->interleave_ways > 5)
			tl_hi |= FIELD_PREP(GENMASK(15, 8),
					    cxld->target[5]->port_id);
		if (cxld->interleave_ways > 6)
			tl_hi |= FIELD_PREP(GENMASK(23, 16),
					    cxld->target[6]->port_id);
		if (cxld->interleave_ways > 7)
			tl_hi |= FIELD_PREP(GENMASK(31, 24),
					    cxld->target[7]->port_id);

		writel(tl_hi, hdm_decoder + CXL_HDM_DECODER0_TL_HIGH(cxld->id));
		writel(tl_lo, hdm_decoder + CXL_HDM_DECODER0_TL_LOW(cxld->id));
	} else {
		/* Zero out skip list for devices */
		writel(0, hdm_decoder + CXL_HDM_DECODER0_TL_HIGH(cxld->id));
		writel(0, hdm_decoder + CXL_HDM_DECODER0_TL_LOW(cxld->id));
	}

	writel(size_hi,
	       hdm_decoder + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(cxld->id));
	writel(size_lo,
	       hdm_decoder + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(cxld->id));
	writel(base_hi,
	       hdm_decoder + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(cxld->id));
	writel(base_lo,
	       hdm_decoder + CXL_HDM_DECODER0_BASE_LOW_OFFSET(cxld->id));
	writel(ctrl, hdm_decoder + CXL_HDM_DECODER0_CTRL_OFFSET(cxld->id));

	rc = wait_for_commit(cxld);
	if (rc)
		return rc;

	cxld->flags |= CXL_DECODER_F_ENABLE;

#define DPORT_TL_STR "%d %d %d %d %d %d %d %d"
#define DPORT(i)                                                               \
	(cxld->nr_targets && cxld->interleave_ways > (i)) ?                    \
		cxld->target[(i)]->port_id :                                   \
		      -1
#define DPORT_TL                                                               \
	DPORT(0), DPORT(1), DPORT(2), DPORT(3), DPORT(4), DPORT(5), DPORT(6),  \
		DPORT(7)

	dev_dbg(&cxld->dev,
		"%s (depth %d)\n\tBase %pa\n\tSize %llu\n\tIG %u (%ub)\n\tENIW %u (x%u)\n\tTargetList: \n" DPORT_TL_STR,
		dev_name(&port->dev), port->depth, &cxld->decoder_range.start,
		range_len(&cxld->decoder_range),
		cxl_to_ig(cxld->interleave_granularity),
		cxld->interleave_granularity,
		cxl_to_eniw(cxld->interleave_ways), cxld->interleave_ways,
		DPORT_TL);
#undef DPORT_TL
#undef DPORT
#undef DPORT_TL_STR
	return 0;
}
EXPORT_SYMBOL_GPL(cxl_commit_decoder);

/**
 * cxl_disable_decoder() - Disables a decoder
 * @cxld: The active cxl decoder.
 *
 * CXL decoders (as of 2.0 spec) have no way to deactivate them other than to
 * set the size of the HDM to 0. This function will clear all registers, and if
 * the decoder is active, commit the 0'd out registers.
 */
void cxl_disable_decoder(struct cxl_decoder *cxld)
{
	struct cxl_port *port = to_cxl_port(cxld->dev.parent);
	void __iomem *hdm_decoder;
	struct cxl_hdm *cxlhdm;
	u32 ctrl;

	cxlhdm = dev_get_drvdata(&port->dev);
	hdm_decoder = cxlhdm->regs.hdm_decoder;
	ctrl = readl(hdm_decoder + CXL_HDM_DECODER0_CTRL_OFFSET(cxld->id));

	if (dev_WARN_ONCE(&port->dev, (cxld->flags & CXL_DECODER_F_ENABLE) == 0,
			  "Invalid decoder enable state\n"))
		return;

	cxld->flags &= ~CXL_DECODER_F_ENABLE;

	/* There's no way to "uncommit" a committed decoder, only 0 size it */
	writel(0, hdm_decoder + CXL_HDM_DECODER0_TL_HIGH(cxld->id));
	writel(0, hdm_decoder + CXL_HDM_DECODER0_TL_LOW(cxld->id));
	writel(0, hdm_decoder + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(cxld->id));
	writel(0, hdm_decoder + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(cxld->id));
	writel(0, hdm_decoder + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(cxld->id));
	writel(0, hdm_decoder + CXL_HDM_DECODER0_BASE_LOW_OFFSET(cxld->id));

	/* If the device isn't actually active, just zero out all the fields */
	if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
		writel(CXL_HDM_DECODER0_CTRL_COMMIT,
		       hdm_decoder + CXL_HDM_DECODER0_CTRL_OFFSET(cxld->id));
}
EXPORT_SYMBOL_GPL(cxl_disable_decoder);
