/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2015 6WIND S.A.
 * Copyright 2015 Mellanox Technologies, Ltd
 */

#include <stddef.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdalign.h>
#include <sys/un.h>
#include <time.h>

#include <ethdev_driver.h>
#include <bus_pci_driver.h>
#include <rte_mbuf.h>
#include <rte_common.h>
#include <rte_eal_paging.h>
#include <rte_interrupts.h>
#include <rte_malloc.h>
#include <rte_string_fns.h>
#include <rte_rwlock.h>
#include <rte_cycles.h>

#include <mlx5_glue.h>
#include <mlx5_devx_cmds.h>
#include <mlx5_common.h>
#include <mlx5_malloc.h>

#include "mlx5.h"
#include "mlx5_rxtx.h"
#include "mlx5_utils.h"

/**
 * Get interface name from private structure.
 *
 * There are no representors or bonding here, so the interface is always the
 * one belonging to the RDMA device itself.
 *
 * @param[in] dev
 *   Pointer to Ethernet device.
 * @param[out] ifname
 *   Interface name output buffer.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_get_ifname(const struct rte_eth_dev *dev, char ifname[MLX5_NAMESIZE])
{
	struct mlx5_priv *priv = dev->data->dev_private;
	unsigned int ifindex;

	MLX5_ASSERT(priv);
	MLX5_ASSERT(priv->sh);
	ifindex = mlx5_ifindex(dev);
	if (!ifindex)
		return mlx5_get_ifname_sysfs(priv->sh->ibdev_path, ifname);
	if (if_indextoname(ifindex, ifname))
		return 0;
	rte_errno = errno;
	return -rte_errno;
}

/**
 * Perform ifreq ioctl() on associated netdev ifname.
 *
 * @param[in] ifname
 *   Pointer to netdev name.
 * @param req
 *   Request number to pass to ioctl().
 * @param[out] ifr
 *   Interface request structure output buffer.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
static int
mlx5_ifreq_by_ifname(const char *ifname, int req, struct ifreq *ifr)
{
	int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	int ret = 0;

	if (sock == -1) {
		rte_errno = errno;
		return -rte_errno;
	}
	rte_strscpy(ifr->ifr_name, ifname, sizeof(ifr->ifr_name));
	ret = ioctl(sock, req, ifr);
	if (ret == -1) {
		rte_errno = errno;
		goto error;
	}
	close(sock);
	return 0;
error:
	close(sock);
	return -rte_errno;
}

/**
 * Perform ifreq ioctl() on associated Ethernet device.
 *
 * @param[in] dev
 *   Pointer to Ethernet device.
 * @param req
 *   Request number to pass to ioctl().
 * @param[out] ifr
 *   Interface request structure output buffer.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
static int
mlx5_ifreq(const struct rte_eth_dev *dev, int req, struct ifreq *ifr)
{
	char ifname[MLX5_NAMESIZE];
	int ret;

	ret = mlx5_get_ifname(dev, ifname);
	if (ret)
		return -rte_errno;
	return mlx5_ifreq_by_ifname(ifname, req, ifr);
}

/**
 * Get device minimum and maximum allowed MTU values.
 *
 * @param dev
 *   Pointer to Ethernet device.
 * @param[out] min_mtu
 *   Minimum MTU value output buffer.
 * @param[out] max_mtu
 *   Maximum MTU value output buffer.
 *
 * @return
 *   (-ENOTSUP) - the bounds are reported over rtnetlink, which is absent here,
 *   so the caller falls back to the driver's own limits.
 */
int
mlx5_os_get_mtu_bounds(struct rte_eth_dev *dev, uint16_t *min_mtu, uint16_t *max_mtu)
{
	RTE_SET_USED(dev);
	RTE_SET_USED(min_mtu);
	RTE_SET_USED(max_mtu);

	rte_errno = ENOTSUP;
	return -rte_errno;
}

/**
 * Get device MTU.
 *
 * @param dev
 *   Pointer to Ethernet device.
 * @param[out] mtu
 *   MTU value output buffer.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_get_mtu(struct rte_eth_dev *dev, uint16_t *mtu)
{
	struct ifreq request;
	int ret = mlx5_ifreq(dev, SIOCGIFMTU, &request);

	if (ret)
		return ret;
	*mtu = request.ifr_mtu;
	return 0;
}

/**
 * Set device MTU.
 *
 * @param dev
 *   Pointer to Ethernet device.
 * @param mtu
 *   MTU value to set.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_set_mtu(struct rte_eth_dev *dev, uint16_t mtu)
{
	struct ifreq request = { .ifr_mtu = mtu, };

	return mlx5_ifreq(dev, SIOCSIFMTU, &request);
}

/**
 * Set device flags.
 *
 * @param dev
 *   Pointer to Ethernet device.
 * @param keep
 *   Bitmask for flags that must remain untouched.
 * @param flags
 *   Bitmask for flags to modify.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
static int
mlx5_set_flags(struct rte_eth_dev *dev, unsigned int keep, unsigned int flags)
{
	struct ifreq request;
	int ret = mlx5_ifreq(dev, SIOCGIFFLAGS, &request);

	if (ret)
		return ret;
	request.ifr_flags &= keep;
	request.ifr_flags |= flags & ~keep;
	return mlx5_ifreq(dev, SIOCSIFFLAGS, &request);
}

/**
 * Get device current raw clock counter
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] time
 *   Current raw clock counter of the device.
 *
 * @return
 *   0 if the clock has correctly been read
 *   The value of errno in case of error
 */
int
mlx5_read_clock(struct rte_eth_dev *dev, uint64_t *clock)
{
	struct mlx5_priv *priv = dev->data->dev_private;
	struct ibv_context *ctx = priv->sh->cdev->ctx;
	struct ibv_values_ex values;
	int err = 0;

	values.comp_mask = IBV_VALUES_MASK_RAW_CLOCK;
	err = mlx5_glue->query_rt_values_ex(ctx, &values);
	if (err != 0) {
		DRV_LOG(WARNING, "Could not query the clock !");
		return err;
	}
	*clock = values.raw_clock.tv_nsec;
	return 0;
}

static const struct ifmedia_baudrate mlx5_ifmedia_baudrate_desc[] =
	IFM_BAUDRATE_DESCRIPTIONS;

/**
 * Translate an ifmedia word into a bit rate.
 *
 * @param mword
 *   Media word as reported by SIOCGIFXMEDIA.
 *
 * @return
 *   Bit rate in bits per second, 0 if the word is not a known Ethernet media.
 */
static uint64_t
mlx5_ifmedia_baudrate(int mword)
{
	unsigned int i;

	for (i = 0; mlx5_ifmedia_baudrate_desc[i].ifmb_word != 0; i++) {
		if (IFM_TYPE_MATCH(mword,
				   mlx5_ifmedia_baudrate_desc[i].ifmb_word))
			return mlx5_ifmedia_baudrate_desc[i].ifmb_baudrate;
	}
	return 0;
}

/**
 * Translate a bit rate in Mbps into the matching link speed capability bit.
 *
 * @param mbps
 *   Bit rate in Mbps.
 *
 * @return
 *   Link speed capability bit, 0 if the rate has no representation.
 */
static uint32_t
mlx5_link_speed_capa(uint64_t mbps)
{
	switch (mbps) {
	case 800000:
		return RTE_ETH_LINK_SPEED_800G;
	case 400000:
		return RTE_ETH_LINK_SPEED_400G;
	case 200000:
		return RTE_ETH_LINK_SPEED_200G;
	case 100000:
		return RTE_ETH_LINK_SPEED_100G;
	case 56000:
		return RTE_ETH_LINK_SPEED_56G;
	case 50000:
		return RTE_ETH_LINK_SPEED_50G;
	case 40000:
		return RTE_ETH_LINK_SPEED_40G;
	case 25000:
		return RTE_ETH_LINK_SPEED_25G;
	case 20000:
		return RTE_ETH_LINK_SPEED_20G;
	case 10000:
		return RTE_ETH_LINK_SPEED_10G;
	case 5000:
		return RTE_ETH_LINK_SPEED_5G;
	case 2500:
		return RTE_ETH_LINK_SPEED_2_5G;
	case 1000:
		return RTE_ETH_LINK_SPEED_1G;
	case 100:
		return RTE_ETH_LINK_SPEED_100M;
	case 10:
		return RTE_ETH_LINK_SPEED_10M;
	default:
		return 0;
	}
}

/**
 * DPDK callback to retrieve physical link information.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] link
 *   Storage for current link status.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
static int
mlx5_link_update_bsd(struct rte_eth_dev *dev, struct rte_eth_link *link)
{
	struct mlx5_priv *priv = dev->data->dev_private;
	struct rte_eth_link dev_link = { 0 };
	struct ifmediareq ifmr = { 0 };
	char ifname[MLX5_NAMESIZE];
	uint32_t speed_capa = 0;
	int *media_list = NULL;
	uint64_t mbps;
	int sock;
	int ret;
	int i;

	ret = mlx5_get_ifname(dev, ifname);
	if (ret)
		return ret;
	sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock == -1) {
		rte_errno = errno;
		DRV_LOG(ERR, "port %u failed to open socket for link update: %s",
			dev->data->port_id, strerror(rte_errno));
		return -rte_errno;
	}
	rte_strscpy(ifmr.ifm_name, ifname, sizeof(ifmr.ifm_name));
	/* First call reports how many media words the interface has. */
	if (ioctl(sock, SIOCGIFXMEDIA, (caddr_t)&ifmr) < 0) {
		rte_errno = errno;
		DRV_LOG(ERR, "ioctl(SIOCGIFXMEDIA) on %s: %s",
			ifname, strerror(rte_errno));
		ret = -rte_errno;
		goto out;
	}
	if (ifmr.ifm_count > 0) {
		media_list = mlx5_malloc(MLX5_MEM_ZERO,
					 ifmr.ifm_count * sizeof(*media_list),
					 alignof(int), SOCKET_ID_ANY);
		if (media_list == NULL) {
			rte_errno = ENOMEM;
			ret = -rte_errno;
			goto out;
		}
		ifmr.ifm_ulist = media_list;
		if (ioctl(sock, SIOCGIFXMEDIA, (caddr_t)&ifmr) < 0) {
			rte_errno = errno;
			DRV_LOG(ERR, "ioctl(SIOCGIFXMEDIA) on %s: %s",
				ifname, strerror(rte_errno));
			ret = -rte_errno;
			goto out;
		}
	}
	dev_link.link_status = ((ifmr.ifm_status & IFM_AVALID) &&
				(ifmr.ifm_status & IFM_ACTIVE)) ?
			       RTE_ETH_LINK_UP : RTE_ETH_LINK_DOWN;
	mbps = (ifmr.ifm_status & IFM_AVALID) ?
	       mlx5_ifmedia_baudrate(ifmr.ifm_active) / (1000 * 1000) : 0;
	dev_link.link_speed = (mbps == 0) ? RTE_ETH_SPEED_NUM_NONE : mbps;
	/* The media word carries no half duplex variant above 100M. */
	dev_link.link_duplex = (ifmr.ifm_active & IFM_HDX) ?
			       RTE_ETH_LINK_HALF_DUPLEX :
			       RTE_ETH_LINK_FULL_DUPLEX;
	dev_link.link_autoneg = !(dev->data->dev_conf.link_speeds &
				  RTE_ETH_LINK_SPEED_FIXED);
	/*
	 * The supported media list is returned as pairs, the media word being
	 * the second element of each pair.
	 */
	for (i = 1; i < ifmr.ifm_count; i += 2) {
		mbps = mlx5_ifmedia_baudrate(media_list[i]) / (1000 * 1000);
		speed_capa |= mlx5_link_speed_capa(mbps);
	}
	/* Speed is negotiated by the kernel driver, not selected here. */
	priv->link_speed_capa = speed_capa | RTE_ETH_LINK_SPEED_AUTONEG;
	*link = dev_link;
	ret = 0;
out:
	mlx5_free(media_list);
	close(sock);
	return ret;
}

/**
 * DPDK callback to retrieve physical link information.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param wait_to_complete
 *   Wait for request completion.
 *
 * @return
 *   0 if link status was not updated, positive if it was, a negative errno
 *   value otherwise and rte_errno is set.
 */
int
mlx5_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	int ret;
	struct rte_eth_link dev_link = { 0 };
	time_t start_time = time(NULL);
	int retry = MLX5_GET_LINK_STATUS_RETRY_COUNT;

	do {
		ret = mlx5_link_update_bsd(dev, &dev_link);
		if (ret == 0)
			break;
		/* Handle wait to complete situation. */
		if ((wait_to_complete || retry) && ret == -EAGAIN) {
			if (abs((int)difftime(time(NULL), start_time)) <
			    MLX5_LINK_STATUS_TIMEOUT) {
				usleep(0);
				continue;
			} else {
				rte_errno = EBUSY;
				return -rte_errno;
			}
		} else if (ret < 0) {
			return ret;
		}
	} while (wait_to_complete || retry-- > 0);
	ret = !!memcmp(&dev->data->dev_link, &dev_link,
		       sizeof(struct rte_eth_link));
	dev->data->dev_link = dev_link;
	return ret;
}

/**
 * DPDK callback to get flow control status.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] fc_conf
 *   Flow control output buffer.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_dev_get_flow_ctrl(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	RTE_SET_USED(fc_conf);
	DRV_LOG(WARNING,
		"port %u get flow control status operation not supported",
		dev->data->port_id);
	rte_errno = ENOTSUP;
	return -rte_errno;
}

/**
 * DPDK callback to modify flow control parameters.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[in] fc_conf
 *   Flow control parameters.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_dev_set_flow_ctrl(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	RTE_SET_USED(fc_conf);
	DRV_LOG(WARNING,
		"port %u set flow control status operation not supported",
		dev->data->port_id);
	rte_errno = ENOTSUP;
	return -rte_errno;
}

/**
 * Handle asynchronous removal event for entire multiport device.
 *
 * @param sh
 *   Infiniband device shared context.
 */
static void
mlx5_dev_interrupt_device_fatal(struct mlx5_dev_ctx_shared *sh)
{
	uint32_t i;

	for (i = 0; i < sh->max_port; ++i) {
		struct rte_eth_dev *dev;
		struct mlx5_priv *priv;

		if (sh->port[i].ih_port_id >= RTE_MAX_ETHPORTS) {
			/*
			 * Or not existing port either no
			 * handler installed for this port.
			 */
			continue;
		}
		dev = &rte_eth_devices[sh->port[i].ih_port_id];
		MLX5_ASSERT(dev);
		priv = dev->data->dev_private;
		MLX5_ASSERT(priv);
		if (!priv->rmv_notified && dev->data->dev_conf.intr_conf.rmv) {
			/* Notify driver about removal only once. */
			priv->rmv_notified = 1;
			rte_eth_dev_callback_process
				(dev, RTE_ETH_EVENT_INTR_RMV, NULL);
		}
	}
}

/*
 * Link status changes are delivered over rtnetlink, which has no counterpart
 * here, so no interrupt source is ever registered and this handler is never
 * installed. Applications asking for LSC fall back to polling
 * mlx5_link_update().
 */
void
mlx5_dev_interrupt_handler_nl(void *arg)
{
	RTE_SET_USED(arg);
}

/**
 * Handle shared asynchronous events the NIC (removal event
 * and link status change). Supports multiport IB device.
 *
 * @param cb_arg
 *   Callback argument.
 */
void
mlx5_dev_interrupt_handler(void *cb_arg)
{
	struct mlx5_dev_ctx_shared *sh = cb_arg;
	struct ibv_async_event event;

	/* Read all message from the IB device and acknowledge them. */
	for (;;) {
		struct rte_eth_dev *dev;
		uint32_t tmp;

		if (mlx5_glue->get_async_event(sh->cdev->ctx, &event)) {
			if (errno == EIO) {
				DRV_LOG(DEBUG,
					"IBV async event queue closed on: %s",
					sh->ibdev_name);
				mlx5_dev_interrupt_device_fatal(sh);
			}
			break;
		}
		if (event.event_type == IBV_EVENT_DEVICE_FATAL) {
			/*
			 * The DEVICE_FATAL event can be called by kernel
			 * twice - from mlx5 and uverbs layers, and port
			 * index is not applicable. We should notify all
			 * existing ports.
			 */
			mlx5_dev_interrupt_device_fatal(sh);
			mlx5_glue->ack_async_event(&event);
			continue;
		}
		/* Retrieve and check IB port index. */
		tmp = (uint32_t)event.element.port_num;
		MLX5_ASSERT(tmp <= sh->max_port);
		if (!tmp) {
			/* Unsupported device level event. */
			mlx5_glue->ack_async_event(&event);
			DRV_LOG(DEBUG,
				"unsupported common event (type %d)",
				event.event_type);
			continue;
		}
		if (tmp > sh->max_port) {
			/* Invalid IB port index. */
			mlx5_glue->ack_async_event(&event);
			DRV_LOG(DEBUG,
				"cannot handle an event (type %d)"
				"due to invalid IB port index (%u)",
				event.event_type, tmp);
			continue;
		}
		if (sh->port[tmp - 1].ih_port_id >= RTE_MAX_ETHPORTS) {
			/* No handler installed. */
			mlx5_glue->ack_async_event(&event);
			DRV_LOG(DEBUG,
				"cannot handle an event (type %d)"
				"due to no handler installed for port %u",
				event.event_type, tmp);
			continue;
		}
		/* Retrieve ethernet device descriptor. */
		tmp = sh->port[tmp - 1].ih_port_id;
		dev = &rte_eth_devices[tmp];
		MLX5_ASSERT(dev);
		DRV_LOG(DEBUG,
			"port %u cannot handle an unknown event (type %d)",
			dev->data->port_id, event.event_type);
		mlx5_glue->ack_async_event(&event);
	}
}

/**
 * Handle DEVX interrupts from the NIC.
 * This function is probably called from the DPDK host thread.
 *
 * @param cb_arg
 *   Callback argument.
 */
void
mlx5_dev_interrupt_handler_devx(void *cb_arg)
{
#ifndef HAVE_IBV_DEVX_ASYNC
	(void)cb_arg;
	return;
#else
	struct mlx5_dev_ctx_shared *sh = cb_arg;
	union {
		struct mlx5dv_devx_async_cmd_hdr cmd_resp;
		uint8_t buf[MLX5_ST_SZ_BYTES(query_flow_counter_out) +
			    MLX5_ST_SZ_BYTES(traffic_counter) +
			    sizeof(struct mlx5dv_devx_async_cmd_hdr)];
	} out;
	uint8_t *buf = out.buf + sizeof(out.cmd_resp);

	while (!mlx5_glue->devx_get_async_cmd_comp(sh->devx_comp,
						   &out.cmd_resp,
						   sizeof(out.buf)))
		mlx5_flow_async_pool_query_handle
			(sh, (uint64_t)out.cmd_resp.wr_id,
			 mlx5_devx_get_out_command_status(buf));
#endif /* HAVE_IBV_DEVX_ASYNC */
}

/*
 * RDMA netdev attach and detach events are reported by the RDMA netlink
 * monitor, which is absent here, so this handler is never installed.
 */
void
mlx5_dev_interrupt_handler_ib(void *arg)
{
	RTE_SET_USED(arg);
}

/**
 * DPDK callback to bring the link DOWN.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_set_link_down(struct rte_eth_dev *dev)
{
	return mlx5_set_flags(dev, ~IFF_UP, ~IFF_UP);
}

/**
 * DPDK callback to bring the link UP.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_set_link_up(struct rte_eth_dev *dev)
{
	return mlx5_set_flags(dev, ~IFF_UP, IFF_UP);
}

/**
 * Check if mlx5 device was removed.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   1 when device is removed, otherwise 0.
 */
int
mlx5_is_removed(struct rte_eth_dev *dev)
{
	struct ibv_device_attr device_attr;
	struct mlx5_priv *priv = dev->data->dev_private;

	if (mlx5_glue->query_device(priv->sh->cdev->ctx, &device_attr) == EIO)
		return 1;
	return 0;
}

/**
 * Get switch information associated with network interface.
 *
 * The E-Switch topology is described by the phys_port_name and phys_switch_id
 * sysfs attributes, which have no counterpart here, so neither masters nor
 * representors can be recognized and every port is probed on its own.
 *
 * @param ifindex
 *   Network interface index.
 * @param[out] info
 *   Switch information object, populated in case of success.
 *
 * @return
 *   (-ENOTSUP) - E-Switch topology cannot be queried.
 */
int
mlx5_sysfs_switch_info(unsigned int ifindex, struct mlx5_switch_info *info)
{
	RTE_SET_USED(ifindex);
	RTE_SET_USED(info);

	rte_errno = ENOTSUP;
	return -rte_errno;
}

/**
 * Get bond information associated with network interface.
 *
 * @param pf_ifindex
 *   Network interface index of bond slave interface
 * @param[out] ifindex
 *   Pointer to bond ifindex.
 * @param[out] ifname
 *   Pointer to bond ifname.
 *
 * @return
 *   (-ENOTSUP) - bonded devices are not probed here.
 */
int
mlx5_sysfs_bond_info(unsigned int pf_ifindex, unsigned int *ifindex,
		     char *ifname)
{
	RTE_SET_USED(pf_ifindex);
	RTE_SET_USED(ifindex);
	RTE_SET_USED(ifname);

	rte_errno = ENOTSUP;
	return -rte_errno;
}

/**
 * DPDK callback to retrieve plug-in module EEPROM information (type and size).
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] modinfo
 *   Storage for plug-in module EEPROM information.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_get_module_info(struct rte_eth_dev *dev,
		     struct rte_eth_dev_module_info *modinfo)
{
	RTE_SET_USED(dev);
	RTE_SET_USED(modinfo);

	rte_errno = ENOTSUP;
	return -rte_errno;
}

/**
 * DPDK callback to retrieve plug-in module EEPROM data.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] info
 *   Storage for plug-in module EEPROM data.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int mlx5_get_module_eeprom(struct rte_eth_dev *dev,
			   struct rte_dev_eeprom_info *info)
{
	RTE_SET_USED(dev);
	RTE_SET_USED(info);

	rte_errno = ENOTSUP;
	return -rte_errno;
}

/*
 * Read device counters.
 *
 * Only the counters that come from the RDMA device are available, the netdev
 * counters are reported over ethtool and have no counterpart here.
 *
 * @param dev
 *   Pointer to Ethernet device.
 * @param bond_master
 *   Indicate if the device is a bond master.
 * @param stats
 *   Counters table output buffer.
 *
 * @return
 *   0 on success and stats is filled, negative errno value otherwise and
 *   rte_errno is set.
 */
int
mlx5_os_read_dev_counters(struct rte_eth_dev *dev, bool bond_master, uint64_t *stats)
{
	struct mlx5_priv *priv = dev->data->dev_private;
	struct mlx5_xstats_ctrl *xstats_ctrl = &priv->xstats_ctrl;
	uint16_t i;

	RTE_SET_USED(bond_master);
	memset(stats, 0, sizeof(*stats) * xstats_ctrl->mlx5_stats_n);
	for (i = xstats_ctrl->dev_cnt_start; i < xstats_ctrl->mlx5_stats_n; i++) {
		if (!xstats_ctrl->info[i].dev)
			continue;
		/* Return the last read value if the counter is unavailable. */
		if (mlx5_os_read_dev_stat(priv, xstats_ctrl->info[i].ctr_name,
					  &stats[i]) == 0)
			xstats_ctrl->xstats[i] = stats[i];
		else
			stats[i] = xstats_ctrl->xstats[i];
	}
	return 0;
}

/*
 * Query the number of statistics provided by ETHTOOL.
 *
 * There is no ethtool string set to enumerate, so no netdev counter is
 * reported. Answering with zero rather than an error keeps
 * mlx5_xstats_reset() working over the counters that are available.
 *
 * @param dev
 *   Pointer to Ethernet device.
 * @param bond_master
 *   Indicate if the device is a bond master.
 * @param n_stats
 *   Pointer to number of stats to store.
 * @param n_stats_sec
 *   Pointer to number of stats to store for the 2nd port of the bond.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx5_os_get_stats_n(struct rte_eth_dev *dev, bool bond_master,
		    uint16_t *n_stats, uint16_t *n_stats_sec)
{
	RTE_SET_USED(dev);
	RTE_SET_USED(bond_master);

	*n_stats = 0;
	*n_stats_sec = 0;
	return 0;
}

static const struct mlx5_counter_ctrl mlx5_counters_init[] = {
	{
		.dpdk_name = "rx_unicast_bytes",
		.ctr_name = "rx_vport_unicast_bytes",
	},
	{
		.dpdk_name = "rx_multicast_bytes",
		.ctr_name = "rx_vport_multicast_bytes",
	},
	{
		.dpdk_name = "rx_broadcast_bytes",
		.ctr_name = "rx_vport_broadcast_bytes",
	},
	{
		.dpdk_name = "rx_unicast_packets",
		.ctr_name = "rx_vport_unicast_packets",
	},
	{
		.dpdk_name = "rx_multicast_packets",
		.ctr_name = "rx_vport_multicast_packets",
	},
	{
		.dpdk_name = "rx_broadcast_packets",
		.ctr_name = "rx_vport_broadcast_packets",
	},
	{
		.dpdk_name = "tx_unicast_bytes",
		.ctr_name = "tx_vport_unicast_bytes",
	},
	{
		.dpdk_name = "tx_multicast_bytes",
		.ctr_name = "tx_vport_multicast_bytes",
	},
	{
		.dpdk_name = "tx_broadcast_bytes",
		.ctr_name = "tx_vport_broadcast_bytes",
	},
	{
		.dpdk_name = "tx_unicast_packets",
		.ctr_name = "tx_vport_unicast_packets",
	},
	{
		.dpdk_name = "tx_multicast_packets",
		.ctr_name = "tx_vport_multicast_packets",
	},
	{
		.dpdk_name = "tx_broadcast_packets",
		.ctr_name = "tx_vport_broadcast_packets",
	},
	{
		.dpdk_name = "rx_wqe_errors",
		.ctr_name = "rx_wqe_err",
	},
	{
		.dpdk_name = "rx_phy_crc_errors",
		.ctr_name = "rx_crc_errors_phy",
	},
	{
		.dpdk_name = "rx_phy_in_range_len_errors",
		.ctr_name = "rx_in_range_len_errors_phy",
	},
	{
		.dpdk_name = "rx_phy_symbol_errors",
		.ctr_name = "rx_symbol_err_phy",
	},
	{
		.dpdk_name = "tx_phy_errors",
		.ctr_name = "tx_errors_phy",
	},
	{
		.dpdk_name = "rx_out_of_buffer",
		.ctr_name = "out_of_buffer",
		.dev = 1,
	},
	{
		.dpdk_name = "hairpin_out_of_buffer",
		.ctr_name = "hairpin_out_of_buffer",
		.dev = 1,
		.ctrl = {
			.enable = mlx5_enable_port_level_hairpin_counter,
			.disable = mlx5_disable_port_level_hairpin_counter,
			.enabled = 0,
		}
	},
	{
		.dpdk_name = "dev_internal_queue_oob",
		.ctr_name = "dev_internal_queue_oob",
	},
	{
		.dpdk_name = "tx_phy_packets",
		.ctr_name = "tx_packets_phy",
	},
	{
		.dpdk_name = "rx_phy_packets",
		.ctr_name = "rx_packets_phy",
	},
	{
		.dpdk_name = "tx_phy_discard_packets",
		.ctr_name = "tx_discards_phy",
	},
	{
		.dpdk_name = "rx_phy_discard_packets",
		.ctr_name = "rx_discards_phy",
	},
	{
		.dpdk_name = "rx_prio0_buf_discard_packets",
		.ctr_name = "rx_prio0_buf_discard",
	},
	{
		.dpdk_name = "rx_prio1_buf_discard_packets",
		.ctr_name = "rx_prio1_buf_discard",
	},
	{
		.dpdk_name = "rx_prio2_buf_discard_packets",
		.ctr_name = "rx_prio2_buf_discard",
	},
	{
		.dpdk_name = "rx_prio3_buf_discard_packets",
		.ctr_name = "rx_prio3_buf_discard",
	},
	{
		.dpdk_name = "rx_prio4_buf_discard_packets",
		.ctr_name = "rx_prio4_buf_discard",
	},
	{
		.dpdk_name = "rx_prio5_buf_discard_packets",
		.ctr_name = "rx_prio5_buf_discard",
	},
	{
		.dpdk_name = "rx_prio6_buf_discard_packets",
		.ctr_name = "rx_prio6_buf_discard",
	},
	{
		.dpdk_name = "rx_prio7_buf_discard_packets",
		.ctr_name = "rx_prio7_buf_discard",
	},
	{
		.dpdk_name = "rx_prio0_cong_discard_packets",
		.ctr_name = "rx_prio0_cong_discard",
	},
	{
		.dpdk_name = "rx_prio1_cong_discard_packets",
		.ctr_name = "rx_prio1_cong_discard",
	},
	{
		.dpdk_name = "rx_prio2_cong_discard_packets",
		.ctr_name = "rx_prio2_cong_discard",
	},
	{
		.dpdk_name = "rx_prio3_cong_discard_packets",
		.ctr_name = "rx_prio3_cong_discard",
	},
	{
		.dpdk_name = "rx_prio4_cong_discard_packets",
		.ctr_name = "rx_prio4_cong_discard",
	},
	{
		.dpdk_name = "rx_prio5_cong_discard_packets",
		.ctr_name = "rx_prio5_cong_discard",
	},
	{
		.dpdk_name = "rx_prio6_cong_discard_packets",
		.ctr_name = "rx_prio6_cong_discard",
	},
	{
		.dpdk_name = "rx_prio7_cong_discard_packets",
		.ctr_name = "rx_prio7_cong_discard",
	},
	{
		.dpdk_name = "tx_phy_bytes",
		.ctr_name = "tx_bytes_phy",
	},
	{
		.dpdk_name = "rx_phy_bytes",
		.ctr_name = "rx_bytes_phy",
	},
	/* Representor only */
	{
		.dpdk_name = "rx_vport_packets",
		.ctr_name = "vport_rx_packets",
	},
	{
		.dpdk_name = "rx_vport_bytes",
		.ctr_name = "vport_rx_bytes",
	},
	{
		.dpdk_name = "tx_vport_packets",
		.ctr_name = "vport_tx_packets",
	},
	{
		.dpdk_name = "tx_vport_bytes",
		.ctr_name = "vport_tx_bytes",
	},
	/**
	 * Device counters: These counters are for the
	 * entire PCI device (NIC). These counters are
	 * not counting on a per port/queue basis.
	 */
	{
		.dpdk_name = "rx_pci_signal_integrity",
		.ctr_name = "rx_pci_signal_integrity",
	},
	{
		.dpdk_name = "tx_pci_signal_integrity",
		.ctr_name = "tx_pci_signal_integrity",
	},
	{
		.dpdk_name = "outbound_pci_buffer_overflow",
		.ctr_name = "outbound_pci_buffer_overflow",
	},
	{
		.dpdk_name = "outbound_pci_stalled_rd",
		.ctr_name = "outbound_pci_stalled_rd",
	},
	{
		.dpdk_name = "outbound_pci_stalled_wr",
		.ctr_name = "outbound_pci_stalled_wr",
	},
	{
		.dpdk_name = "outbound_pci_stalled_rd_events",
		.ctr_name = "outbound_pci_stalled_rd_events",
	},
	{
		.dpdk_name = "outbound_pci_stalled_wr_events",
		.ctr_name = "outbound_pci_stalled_wr_events",
	},
	{
		.dpdk_name = "dev_out_of_buffer",
		.ctr_name = "dev_out_of_buffer",
	},
};

const unsigned int xstats_n = RTE_DIM(mlx5_counters_init);

/**
 * Init the structures to read device counters.
 *
 * Only the counters marked as belonging to the RDMA device are registered,
 * the rest are read over ethtool on Linux and have no source here.
 *
 * @param dev
 *   Pointer to Ethernet device.
 */
void
mlx5_os_stats_init(struct rte_eth_dev *dev)
{
	struct mlx5_priv *priv = dev->data->dev_private;
	struct mlx5_xstats_ctrl *xstats_ctrl = &priv->xstats_ctrl;
	struct mlx5_stats_ctrl *stats_ctrl = &priv->stats_ctrl;
	unsigned int j;
	uint16_t idx;
	int ret;

	/* Ensure no out of bounds access before. */
	MLX5_ASSERT(xstats_n <= MLX5_MAX_XSTATS);
	/* So that it won't aggregate for each init. */
	xstats_ctrl->mlx5_stats_n = 0;
	xstats_ctrl->stats_n = 0;
	xstats_ctrl->stats_n_2nd = 0;
	xstats_ctrl->dev_cnt_start = 0;
	for (j = 0; j != xstats_n; j++) {
		xstats_ctrl->dev_table_idx[j] = UINT16_MAX;
		xstats_ctrl->dev_table_idx_2nd[j] = UINT16_MAX;
		if (!mlx5_counters_init[j].dev)
			continue;
		idx = xstats_ctrl->mlx5_stats_n++;
		xstats_ctrl->info[idx] = mlx5_counters_init[j];
		xstats_ctrl->hw_stats[idx] = 0;
	}
	/* Copy to base at first time. */
	ret = mlx5_os_read_dev_counters(dev, false, xstats_ctrl->base);
	if (ret)
		DRV_LOG(ERR, "port %u cannot read device counters: %s",
			dev->data->port_id, strerror(rte_errno));
	mlx5_os_read_dev_stat(priv, "out_of_buffer", &stats_ctrl->imissed_base);
	stats_ctrl->imissed = 0;
}

/**
 * Get MAC address by querying netdevice.
 *
 * @param[in] dev
 *   Pointer to Ethernet device.
 * @param[out] mac
 *   MAC address output buffer.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_get_mac(struct rte_eth_dev *dev, uint8_t (*mac)[RTE_ETHER_ADDR_LEN])
{
	struct ifaddrs *ifap, *ifa;
	char ifname[MLX5_NAMESIZE];
	int ret;

	ret = mlx5_get_ifname(dev, ifname);
	if (ret)
		return ret;
	/* There is no ioctl for the link level address, walk the list. */
	if (getifaddrs(&ifap) != 0) {
		rte_errno = errno;
		return -rte_errno;
	}
	ret = -ENXIO;
	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		const struct sockaddr_dl *sdl;

		if (ifa->ifa_addr == NULL ||
		    ifa->ifa_addr->sa_family != AF_LINK ||
		    strcmp(ifa->ifa_name, ifname) != 0)
			continue;
		sdl = (const struct sockaddr_dl *)(const void *)ifa->ifa_addr;
		if (sdl->sdl_alen != RTE_ETHER_ADDR_LEN)
			continue;
		memcpy(mac, LLADDR(sdl), RTE_ETHER_ADDR_LEN);
		ret = 0;
		break;
	}
	freeifaddrs(ifap);
	if (ret) {
		rte_errno = -ret;
		return -rte_errno;
	}
	return 0;
}

/*
 * Query dropless_rq private flag value provided by ETHTOOL.
 *
 * The flag lives in the ethtool private flag set, which has no counterpart
 * here.
 *
 * @param dev
 *   Pointer to Ethernet device.
 *
 * @return
 *   (-ENOTSUP) - the flag cannot be queried.
 */
int mlx5_get_flag_dropless_rq(struct rte_eth_dev *dev)
{
	RTE_SET_USED(dev);

	rte_errno = ENOTSUP;
	return -rte_errno;
}

/**
 * Unmaps HCA PCI BAR from the current process address space.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
void mlx5_txpp_unmap_hca_bar(struct rte_eth_dev *dev)
{
	struct mlx5_proc_priv *ppriv = dev->process_private;

	if (ppriv && ppriv->hca_bar) {
		rte_mem_unmap(ppriv->hca_bar, MLX5_ST_SZ_BYTES(initial_seg));
		ppriv->hca_bar = NULL;
	}
}

/**
 * Maps HCA PCI BAR to the current process address space.
 * Stores pointer in the process private structure allowing
 * to read internal and real time counter directly from the HW.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success and not NULL pointer to mapped area in process structure.
 *   negative otherwise and NULL pointer
 */
int mlx5_txpp_map_hca_bar(struct rte_eth_dev *dev)
{
	RTE_SET_USED(dev);

	/*
	 * The initialize segment is mapped through the sysfs PCI resource
	 * file, which does not exist here, so the real time counter cannot be
	 * read directly from the HCA BAR.
	 */
	rte_errno = ENOTSUP;
	return -rte_errno;
}
