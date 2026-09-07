/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2015 6WIND S.A.
 * Copyright 2015 Mellanox Technologies, Ltd
 */

#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

/*
 * Not needed by this file; included to work around the lack of off_t
 * definition for mlx5dv.h with unpatched rdma-core versions.
 */
#include <sys/types.h>

#include <ethdev_driver.h>
#include <rte_common.h>
#include <rte_malloc.h>

#include <mlx5.h>
#include <mlx5_malloc.h>

/*
 * The VLAN workaround for VF over VMware creates VLAN network devices through
 * rtnetlink, which has no counterpart here, so the whole workaround is
 * unavailable. It is only ever engaged for a VF, and mlx5_vlan_vmwa_init()
 * returning NULL leaves the rest of the driver on the path it takes whenever
 * the workaround is not needed.
 */

/*
 * Release VLAN network device, created for VM workaround.
 *
 * @param[in] dev
 *   Ethernet device object, Netlink context provider.
 * @param[in] vlan
 *   Object representing the network device to release.
 */
void
mlx5_vlan_vmwa_release(struct rte_eth_dev *dev,
			    struct mlx5_vf_vlan *vlan)
{
	RTE_SET_USED(dev);
	RTE_SET_USED(vlan);
}

/**
 * Acquire VLAN interface with specified tag for VM workaround.
 *
 * @param[in] dev
 *   Ethernet device object, Netlink context provider.
 * @param[in] vlan
 *   Object representing the network device to acquire.
 */
void
mlx5_vlan_vmwa_acquire(struct rte_eth_dev *dev,
			    struct mlx5_vf_vlan *vlan)
{
	RTE_SET_USED(dev);
	RTE_SET_USED(vlan);
}

/*
 * Create per ethernet device VLAN VM workaround context
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param ifindex
 *   Interface index.
 *
 * @Return
 *   Pointer to mlx5_nl_vlan_vmwa_context
 */
void *
mlx5_vlan_vmwa_init(struct rte_eth_dev *dev, uint32_t ifindex)
{
	RTE_SET_USED(dev);
	RTE_SET_USED(ifindex);
	return NULL;
}

/*
 * Destroy per ethernet device VLAN VM workaround context
 *
 * @param dev
 *   Pointer to VM context
 */
void
mlx5_vlan_vmwa_exit(void *vmctx)
{
	RTE_SET_USED(vmctx);
}
