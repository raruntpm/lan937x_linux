// SPDX-License-Identifier: GPL-2.0+
/*
 * net/dsa/tag_ksz.c - Microchip KSZ Switch tag format handling
 * Copyright (c) 2017 Microchip Technology
 */


#include <linux/dsa/ksz_common.h>
#include <linux/etherdevice.h>
#include <linux/etherdevice.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <net/dsa.h>
#include "dsa_priv.h"

/* Typically only one byte is used for tail tag. */
#define KSZ_EGRESS_TAG_LEN		1
#define KSZ_INGRESS_TAG_LEN		1

static struct sk_buff *ksz_common_rcv(struct sk_buff *skb,
				      struct net_device *dev,
				      unsigned int port, unsigned int len, u8 device)
{
	skb->dev = dsa_master_find_slave(dev, device, port);
	if (!skb->dev)
		return NULL;

	pskb_trim_rcsum(skb, skb->len - len);

	skb->offload_fwd_mark = true;

	return skb;
}

/*
 * For Ingress (Host -> KSZ8795), 1 byte is added before FCS.
 * ---------------------------------------------------------------------------
 * DA(6bytes)|SA(6bytes)|....|Data(nbytes)|tag(1byte)|FCS(4bytes)
 * ---------------------------------------------------------------------------
 * tag : each bit represents port (eg, 0x01=port1, 0x02=port2, 0x10=port5)
 *
 * For Egress (KSZ8795 -> Host), 1 byte is added before FCS.
 * ---------------------------------------------------------------------------
 * DA(6bytes)|SA(6bytes)|....|Data(nbytes)|tag0(1byte)|FCS(4bytes)
 * ---------------------------------------------------------------------------
 * tag0 : zero-based value represents port
 *	  (eg, 0x00=port1, 0x02=port3, 0x06=port7)
 */

#define KSZ8795_TAIL_TAG_OVERRIDE	BIT(6)
#define KSZ8795_TAIL_TAG_LOOKUP		BIT(7)

static struct sk_buff *ksz8795_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	u8 *tag;
	u8 *addr;

	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb))
		return NULL;

	/* Tag encoding */
	tag = skb_put(skb, KSZ_INGRESS_TAG_LEN);
	addr = skb_mac_header(skb);

	*tag = 1 << dp->index;
	if (is_link_local_ether_addr(addr))
		*tag |= KSZ8795_TAIL_TAG_OVERRIDE;

	return skb;
}

static struct sk_buff *ksz8795_rcv(struct sk_buff *skb, struct net_device *dev)
{
	u8 *tag = skb_tail_pointer(skb) - KSZ_EGRESS_TAG_LEN;

	return ksz_common_rcv(skb, dev, tag[0] & 7, KSZ_EGRESS_TAG_LEN, 0);
}

static const struct dsa_device_ops ksz8795_netdev_ops = {
	.name	= "ksz8795",
	.proto	= DSA_TAG_PROTO_KSZ8795,
	.xmit	= ksz8795_xmit,
	.rcv	= ksz8795_rcv,
	.needed_tailroom = KSZ_INGRESS_TAG_LEN,
};

DSA_TAG_DRIVER(ksz8795_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_KSZ8795);

/*
 * For Ingress (Host -> KSZ9477), 2 bytes are added before FCS.
 * ---------------------------------------------------------------------------
 * DA(6bytes)|SA(6bytes)|....|Data(nbytes)|tag0(1byte)|tag1(1byte)|FCS(4bytes)
 * ---------------------------------------------------------------------------
 * tag0 : Prioritization (not used now)
 * tag1 : each bit represents port (eg, 0x01=port1, 0x02=port2, 0x10=port5)
 *
 * For Egress (KSZ9477 -> Host), 1 byte is added before FCS.
 * ---------------------------------------------------------------------------
 * DA(6bytes)|SA(6bytes)|....|Data(nbytes)|tag0(1byte)|FCS(4bytes)
 * ---------------------------------------------------------------------------
 * tag0 : zero-based value represents port
 *	  (eg, 0x00=port1, 0x02=port3, 0x06=port7)
 */

#define KSZ9477_INGRESS_TAG_LEN		2
#define KSZ9477_PTP_TAG_LEN		4
#define KSZ9477_PTP_TAG_INDICATION	0x80

#define KSZ9477_TAIL_TAG_OVERRIDE	BIT(9)
#define KSZ9477_TAIL_TAG_LOOKUP		BIT(10)

static struct sk_buff *ksz9477_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	__be16 *tag;
	u8 *addr;
	u16 val;

	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb))
		return NULL;

	/* Tag encoding */
	tag = skb_put(skb, KSZ9477_INGRESS_TAG_LEN);
	addr = skb_mac_header(skb);

	val = BIT(dp->index);

	if (is_link_local_ether_addr(addr))
		val |= KSZ9477_TAIL_TAG_OVERRIDE;

	*tag = cpu_to_be16(val);

	return skb;
}

static struct sk_buff *ksz9477_rcv(struct sk_buff *skb, struct net_device *dev)
{
	/* Tag decoding */
	u8 *tag = skb_tail_pointer(skb) - KSZ_EGRESS_TAG_LEN;
	unsigned int port = tag[0] & 7;
	unsigned int len = KSZ_EGRESS_TAG_LEN;

	/* Extra 4-bytes PTP timestamp */
	if (tag[0] & KSZ9477_PTP_TAG_INDICATION)
		len += KSZ9477_PTP_TAG_LEN;

	return ksz_common_rcv(skb, dev, port, len, 0);
}

static const struct dsa_device_ops ksz9477_netdev_ops = {
	.name	= "ksz9477",
	.proto	= DSA_TAG_PROTO_KSZ9477,
	.xmit	= ksz9477_xmit,
	.rcv	= ksz9477_rcv,
	.needed_tailroom = KSZ9477_INGRESS_TAG_LEN,
};

DSA_TAG_DRIVER(ksz9477_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_KSZ9477);

#define KSZ9893_TAIL_TAG_OVERRIDE	BIT(5)
#define KSZ9893_TAIL_TAG_LOOKUP		BIT(6)

static struct sk_buff *ksz9893_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	u8 *addr;
	u8 *tag;

	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb))
		return NULL;

	/* Tag encoding */
	tag = skb_put(skb, KSZ_INGRESS_TAG_LEN);
	addr = skb_mac_header(skb);

	*tag = BIT(dp->index);

	if (is_link_local_ether_addr(addr))
		*tag |= KSZ9893_TAIL_TAG_OVERRIDE;

	return skb;
}

static const struct dsa_device_ops ksz9893_netdev_ops = {
	.name	= "ksz9893",
	.proto	= DSA_TAG_PROTO_KSZ9893,
	.xmit	= ksz9893_xmit,
	.rcv	= ksz9477_rcv,
	.needed_tailroom = KSZ_INGRESS_TAG_LEN,
};

DSA_TAG_DRIVER(ksz9893_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_KSZ9893);


/* For xmit, 2 bytes are added before FCS.
 * ---------------------------------------------------------------------------
 * DA(6bytes)|SA(6bytes)|....|Data(nbytes)|tag0(1byte)|tag1(1byte)|FCS(4bytes)
 * ---------------------------------------------------------------------------
 * tag0 : represents tag override, lookup and valid
 * tag1 : each bit represents port (eg, 0x01=port1, 0x02=port2, 0x80=port8)
 *
 * For rcv, 1 byte is added before FCS.
 * ---------------------------------------------------------------------------
 * DA(6bytes)|SA(6bytes)|....|Data(nbytes)|tag0(1byte)|FCS(4bytes)
 * ---------------------------------------------------------------------------
 * tag0 : zero-based value represents port
 *	  (eg, 0x00=port1, 0x02=port3, 0x07=port8)
 */
#define LAN937X_CASCADE_TAG_LEN		3
#define LAN937X_EGRESS_TAG_LEN		2
#define LAN937X_PTP_TAG_LEN		4

#define LAN937X_PTP_TAG_INDICATION	BIT(7)
#define LAN937X_CASCADE_CHIP            BIT(6)

#define LAN937X_TAIL_TAG_BLOCKING_OVERRIDE	BIT(11)
#define LAN937X_TAIL_TAG_LOOKUP			BIT(12)
#define LAN937X_TAIL_TAG_VALID			BIT(13)
#define LAN937X_TAIL_TAG_PORT_MASK		7

ktime_t ksz_tstamp_reconstruct(struct ksz_device_ptp_shared *ksz, ktime_t tstamp)
{
	struct timespec64 ts = ktime_to_timespec64(tstamp);
	struct timespec64 ptp_clock_time;
	struct timespec64 diff;

	spin_lock_bh(&ksz->ptp_clock_lock);
	ptp_clock_time = ksz->ptp_clock_time;
	spin_unlock_bh(&ksz->ptp_clock_lock);

	/* calculate full time from partial time stamp */
	ts.tv_sec = (ptp_clock_time.tv_sec & ~3) | ts.tv_sec;

	/* find nearest possible point in time */
	diff = timespec64_sub(ts, ptp_clock_time);
	if (diff.tv_sec > 2)
		ts.tv_sec -= 4;
	else if (diff.tv_sec < -2)
		ts.tv_sec += 4;

	return timespec64_to_ktime(ts);
}
EXPORT_SYMBOL(ksz_tstamp_reconstruct);

static void lan937x_xmit_timestamp(struct sk_buff *skb)
{
	u32 tstamp_raw = 0;

	put_unaligned_be32(tstamp_raw, skb_put(skb, LAN937X_PTP_TAG_LEN));
}

static struct sk_buff *lan937x_defer_xmit(struct dsa_port *dp,
					  struct sk_buff *skb)
{
	struct lan937x_port_ptp_shared *ptp_shared = dp->priv;
	struct sk_buff *clone = KSZ_SKB_CB(skb)->clone;
	u8 ptp_msg_type;

	if (!clone)
		return skb;  /* no deferred xmit for this packet */

	/* Use cached PTP msg type from ksz9477_ptp_port_txtstamp().  */
	ptp_msg_type = KSZ_SKB_CB(clone)->ptp_msg_type;
	switch(ptp_msg_type)
	{
	case PTP_MSGTYPE_SYNC:
		skb_queue_tail(&ptp_shared->sync_queue, skb_get(skb));
		kthread_queue_work(ptp_shared->sync_worker, &ptp_shared->sync_work);
		break;

	case PTP_MSGTYPE_PDELAY_REQ:
		skb_queue_tail(&ptp_shared->pdelayreq_queue, skb_get(skb));
		kthread_queue_work(ptp_shared->pdelayreq_worker, &ptp_shared->pdelayreq_work);
		break;

	case PTP_MSGTYPE_PDELAY_RESP:
		skb_queue_tail(&ptp_shared->pdelayrsp_queue, skb_get(skb));
		kthread_queue_work(ptp_shared->pdelayrsp_worker, &ptp_shared->pdelayrsp_work);
		break;

	default:
		kfree_skb(clone);
		KSZ_SKB_CB(skb)->clone = NULL;
		return skb;
	}

	return NULL;
}

static void lan937x_rcv_timestamp(struct sk_buff *skb, u8 *tag,
                                  struct net_device *dev, unsigned int port)
{
	struct skb_shared_hwtstamps *hwtstamps = skb_hwtstamps(skb);
	struct dsa_switch *ds = dev->dsa_ptr->ds;
	struct lan937x_port_ptp_shared *port_ptp_shared;
	u8 *tstamp_raw = tag - KSZ9477_PTP_TAG_LEN;
	ktime_t tstamp;

	port_ptp_shared = dsa_to_port(ds, port)->priv;
	if (!port_ptp_shared)
		return;

	/* convert time stamp and write to skb */
	tstamp = ksz_decode_tstamp(get_unaligned_be32(tstamp_raw));
	memset(hwtstamps, 0, sizeof(*hwtstamps));
	hwtstamps->hwtstamp = ksz_tstamp_reconstruct(port_ptp_shared->dev, tstamp);
}

static struct sk_buff *lan937x_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	struct lan937x_port_ptp_shared *port_ptp_shared = dp->priv;
	struct ksz_device_ptp_shared *ptp_shared = port_ptp_shared->dev;
	u16 queue_mapping = skb_get_queue_mapping(skb);
	u8 prio = netdev_txq_to_tc(dev, queue_mapping);
	const struct ethhdr *hdr = eth_hdr(skb);
	__be16 *tag;
	u16 val;

	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb))
		return NULL;

	/* Tag encoding */
	if (test_bit(LAN937X_HWTS_EN, &ptp_shared->state))
		lan937x_xmit_timestamp(skb);

	tag = skb_put(skb, LAN937X_EGRESS_TAG_LEN);

	val = BIT(dp->index);

	/* priority */
	val |= (prio<<8);

	if (is_link_local_ether_addr(hdr->h_dest))
		val |= LAN937X_TAIL_TAG_BLOCKING_OVERRIDE;

	/* Tail tag valid bit - This bit should always be set by the CPU */
	val |= LAN937X_TAIL_TAG_VALID;

	put_unaligned_be16(val,tag);

	return lan937x_defer_xmit(dp, skb);
}

static struct sk_buff *lan937x_rcv(struct sk_buff *skb, struct net_device *dev)
{
	/* Tag decoding */
	u8 *tag = skb_tail_pointer(skb) - KSZ_EGRESS_TAG_LEN;
	unsigned int port = tag[0] & LAN937X_TAIL_TAG_PORT_MASK;
	unsigned int len = KSZ_EGRESS_TAG_LEN;
	u8 device = 0;

	/* Extra 4-bytes PTP timestamp */
	if (tag[0] & LAN937X_PTP_TAG_INDICATION) {
		lan937x_rcv_timestamp(skb, tag, dev, port);
		len += KSZ9477_PTP_TAG_LEN;
	}

	if (tag[0] & LAN937X_CASCADE_CHIP)
		device = 1;

	return ksz_common_rcv(skb, dev, port, len, device);
}

static const struct dsa_device_ops lan937x_netdev_ops = {
	.name	= "lan937x",
	.proto	= DSA_TAG_PROTO_LAN937X,
	.xmit	= lan937x_xmit,
	.rcv	= lan937x_rcv,
	.needed_tailroom = LAN937X_EGRESS_TAG_LEN + LAN937X_PTP_TAG_LEN,
};

DSA_TAG_DRIVER(lan937x_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_LAN937X);

static struct sk_buff *lan937x_cascade_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	struct lan937x_port_ptp_shared *port_ptp_shared = dp->priv;
	struct ksz_device_ptp_shared *ptp_shared = port_ptp_shared->dev;
	u16 queue_mapping = skb_get_queue_mapping(skb);
	u8 prio = netdev_txq_to_tc(dev, queue_mapping);
	const struct ethhdr *hdr = eth_hdr(skb);
	__be32 *tag_32;
	u32 val_32 = 0;

	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb))
		return NULL;

	/* Tag encoding */
	if (test_bit(LAN937X_HWTS_EN, &ptp_shared->state))
		lan937x_xmit_timestamp(skb);

	tag_32 = skb_put(skb, LAN937X_CASCADE_TAG_LEN);

	/* priority */
	val_32 |= (prio<<8);

	if (is_link_local_ether_addr(hdr->h_dest))
		val_32 |= LAN937X_TAIL_TAG_BLOCKING_OVERRIDE;

	/* Tail tag valid bit - This bit should always be set by the CPU */
	val_32 |= LAN937X_TAIL_TAG_VALID;

	val_32 = (val_32 << 8);
	val_32 |= BIT((dp->index + (8 * dp->ds->index)));

	put_unaligned_be24(val_32,tag_32);

	return lan937x_defer_xmit(dp, skb);
}

static const struct dsa_device_ops cascade_netdev_ops = {
	.name	= "lan937x_cascade",
	.proto	= DSA_TAG_PROTO_LAN937X_CASCADE,
	.xmit	= lan937x_cascade_xmit,
	.rcv	= lan937x_rcv,
	.needed_tailroom = LAN937X_CASCADE_TAG_LEN + LAN937X_PTP_TAG_LEN,
};

DSA_TAG_DRIVER(cascade_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_LAN937X_CASCADE);

static struct dsa_tag_driver *dsa_tag_driver_array[] = {
	&DSA_TAG_DRIVER_NAME(ksz8795_netdev_ops),
	&DSA_TAG_DRIVER_NAME(ksz9477_netdev_ops),
	&DSA_TAG_DRIVER_NAME(ksz9893_netdev_ops),
	&DSA_TAG_DRIVER_NAME(lan937x_netdev_ops),
	&DSA_TAG_DRIVER_NAME(cascade_netdev_ops),
};

module_dsa_tag_drivers(dsa_tag_driver_array);

MODULE_LICENSE("GPL");
