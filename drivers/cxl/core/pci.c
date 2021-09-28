// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/device.h>
#include <linux/pci.h>
#include <pci.h>

/**
 * DOC: cxl pci
 *
 * Compute Express Link protocols are layered on top of PCIe. CXL core provides
 * a set of helpers for CXL interactions which occur via PCIe.
 */

/**
 * is_cxl_mem_enabled() - Does the device understand CXL.mem protocol
 * @pdev: The PCI device for which to determine CXL enablement
 *
 * This is the most discrete determination as to whether a device supports
 * CXL.mem protocol. At a minimum, a CXL device must advertise it is capable of
 * negotiating the CXL.mem protocol while operating in Flex Bus.CXL mode. There
 * are other determining factors as to whether CXL.mem protocol is supported in
 * the path from root port to endpoint. Those other factors require a more
 * comprehensive survey of the CXL topology and would use is_cxl_mem_enabled()
 * as a cursory check.
 *
 * If the PCI device is enabled for CXL.mem protocol return true; otherwise
 * return false.
 *
 * TODO: Is there other architecturally visible state that can be used to infer
 *       CXL.mem protocol support?
 */
bool is_cxl_mem_enabled(struct pci_dev *pdev)
{
	int pcie_dvsec;
	u16 dvsec_ctrl;

	pcie_dvsec = pci_find_dvsec_capability(pdev, PCI_DVSEC_VENDOR_ID_CXL,
					       CXL_DVSEC_PCIE_DEVICE);
	if (!pcie_dvsec) {
		dev_info(&pdev->dev,
			 "Unable to determine CXL protocol support");
		return false;
	}

	pci_read_config_word(pdev,
			     pcie_dvsec + DVSEC_PCIE_DEVICE_CONTROL_OFFSET,
			     &dvsec_ctrl);
	if (!(dvsec_ctrl & DVSEC_PCIE_DEVICE_MEM_ENABLE)) {
		dev_info(&pdev->dev, "CXL.mem protocol not enabled on device");
		return false;
	}

	return true;
}
EXPORT_SYMBOL_GPL(is_cxl_mem_enabled);

/**
 * is_cxl_switch_usp() - Is the device a CXL.mem enabled switch
 * @dev: Device to query for switch type
 *
 * If the device is a CXL.mem capable upstream switch port return true;
 * otherwise return false.
 */
bool is_cxl_switch_usp(struct device *dev)
{
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return false;

	pdev = to_pci_dev(dev);

	return pci_is_pcie(pdev) &&
	       pci_pcie_type(pdev) == PCI_EXP_TYPE_UPSTREAM &&
	       is_cxl_mem_enabled(pdev);
}
EXPORT_SYMBOL_GPL(is_cxl_switch_usp);

/**
 * is_cxl_switch_dsp() - Is the device a CXL.mem enabled switch
 * @dev: Device to query for switch type
 *
 * If the device is a CXL.mem capable downstream switch port return true;
 * otherwise return false.
 */
bool is_cxl_switch_dsp(struct device *dev)
{
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return false;

	pdev = to_pci_dev(dev);

	return pci_is_pcie(pdev) &&
	       pci_pcie_type(pdev) == PCI_EXP_TYPE_DOWNSTREAM &&
	       is_cxl_mem_enabled(pdev);
}
EXPORT_SYMBOL_GPL(is_cxl_switch_dsp);
