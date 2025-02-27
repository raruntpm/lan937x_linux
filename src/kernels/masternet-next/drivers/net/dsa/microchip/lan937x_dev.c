// SPDX-License-Identifier: GPL-2.0
/* Microchip lan937x dev ops functions
 * Copyright (C) 2019-2021 Microchip Technology Inc.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/of_mdio.h>
#include <linux/platform_data/microchip-ksz.h>
#include <linux/phy.h>
#include <linux/if_bridge.h>
#include <net/dsa.h>
#include <net/switchdev.h>

#include "lan937x_reg.h"
#include "ksz_common.h"
#include "lan937x_dev.h"
#include "lan937x_ptp.h"
#include "lan937x_flower.h"

const struct mib_names lan937x_mib_names[] = {
	{ 0x00, "rx_hi" },
	{ 0x01, "rx_undersize" },
	{ 0x02, "rx_fragments" },
	{ 0x03, "rx_oversize" },
	{ 0x04, "rx_jabbers" },
	{ 0x05, "rx_symbol_err" },
	{ 0x06, "rx_crc_err" },
	{ 0x07, "rx_align_err" },
	{ 0x08, "rx_mac_ctrl" },
	{ 0x09, "rx_pause" },
	{ 0x0A, "rx_bcast" },
	{ 0x0B, "rx_mcast" },
	{ 0x0C, "rx_ucast" },
	{ 0x0D, "rx_64_or_less" },
	{ 0x0E, "rx_65_127" },
	{ 0x0F, "rx_128_255" },
	{ 0x10, "rx_256_511" },
	{ 0x11, "rx_512_1023" },
	{ 0x12, "rx_1024_1522" },
	{ 0x13, "rx_1523_2000" },
	{ 0x14, "rx_2001" },
	{ 0x15, "tx_hi" },
	{ 0x16, "tx_late_col" },
	{ 0x17, "tx_pause" },
	{ 0x18, "tx_bcast" },
	{ 0x19, "tx_mcast" },
	{ 0x1A, "tx_ucast" },
	{ 0x1B, "tx_deferred" },
	{ 0x1C, "tx_total_col" },
	{ 0x1D, "tx_exc_col" },
	{ 0x1E, "tx_single_col" },
	{ 0x1F, "tx_mult_col" },
	{ 0x80, "rx_total" },
	{ 0x81, "tx_total" },
	{ 0x82, "rx_discards" },
	{ 0x83, "tx_discards" },
};

int lan937x_cfg(struct ksz_device *dev, u32 addr, u8 bits, bool set)
{
	return regmap_update_bits(dev->regmap[0], addr, bits, set ? bits : 0);
}

int lan937x_port_cfg(struct ksz_device *dev, int port, int offset, u8 bits,
		     bool set)
{
	return regmap_update_bits(dev->regmap[0], PORT_CTRL_ADDR(port, offset),
				  bits, set ? bits : 0);
}

int lan937x_cfg32(struct ksz_device *dev, u32 addr, u32 bits, bool set)
{
	return regmap_update_bits(dev->regmap[2], addr, bits, set ? bits : 0);
}

int lan937x_pread8(struct ksz_device *dev, int port, int offset, u8 *data)
{
	return ksz_read8(dev, PORT_CTRL_ADDR(port, offset), data);
}

int lan937x_pread16(struct ksz_device *dev, int port, int offset, u16 *data)
{
	return ksz_read16(dev, PORT_CTRL_ADDR(port, offset), data);
}

int lan937x_pread32(struct ksz_device *dev, int port, int offset, u32 *data)
{
	return ksz_read32(dev, PORT_CTRL_ADDR(port, offset), data);
}

int lan937x_pwrite8(struct ksz_device *dev, int port, int offset, u8 data)
{
	return ksz_write8(dev, PORT_CTRL_ADDR(port, offset), data);
}

int lan937x_pwrite16(struct ksz_device *dev, int port, int offset, u16 data)
{
	return ksz_write16(dev, PORT_CTRL_ADDR(port, offset), data);
}

int lan937x_pwrite32(struct ksz_device *dev, int port, int offset, u32 data)
{
	return ksz_write32(dev, PORT_CTRL_ADDR(port, offset), data);
}

int lan937x_pwrite8_bulk(struct ksz_device *dev, int port, int offset,
			 u8 *data, u8 n)
{
	return ksz_write8_bulk(dev, PORT_CTRL_ADDR(port, offset), data, n);
}

void lan937x_cfg_port_member(struct ksz_device *dev, int port, u8 member)
{
	lan937x_pwrite32(dev, port, REG_PORT_VLAN_MEMBERSHIP__4, member);
}

static void lan937x_flush_dyn_mac_table(struct ksz_device *dev, int port)
{
	unsigned int value;
	u8 data;

	regmap_update_bits(dev->regmap[0], REG_SW_LUE_CTRL_2,
			   SW_FLUSH_OPTION_M << SW_FLUSH_OPTION_S,
			   SW_FLUSH_OPTION_DYN_MAC << SW_FLUSH_OPTION_S);

	if (port < dev->port_cnt) {
		/* flush individual port */
		lan937x_pread8(dev, port, P_STP_CTRL, &data);
		if (!(data & PORT_LEARN_DISABLE))
			lan937x_pwrite8(dev, port, P_STP_CTRL,
					(data | PORT_LEARN_DISABLE));
		lan937x_cfg(dev, S_FLUSH_TABLE_CTRL, SW_FLUSH_DYN_MAC_TABLE,
			    true);

		regmap_read_poll_timeout(dev->regmap[0], S_FLUSH_TABLE_CTRL,
					 value,
					 !(value & SW_FLUSH_DYN_MAC_TABLE), 10,
					 1000);

		lan937x_pwrite8(dev, port, P_STP_CTRL, data);
	} else {
		/* flush all */
		lan937x_cfg(dev, S_FLUSH_TABLE_CTRL, SW_FLUSH_STP_TABLE, true);
	}
}

static void lan937x_r_mib_cnt(struct ksz_device *dev, int port, u16 addr,
			      u64 *cnt)
{
	unsigned int val;
	u32 data;
	int ret;

	/* Enable MIB Counter read */
	data = MIB_COUNTER_READ;
	data |= (addr << MIB_COUNTER_INDEX_S);
	lan937x_pwrite32(dev, port, REG_PORT_MIB_CTRL_STAT, data);

	ret = regmap_read_poll_timeout(dev->regmap[2],
				       PORT_CTRL_ADDR(port,
						      REG_PORT_MIB_CTRL_STAT),
				       val, !(val & MIB_COUNTER_READ),
				       10, 1000);
	if (ret) {
		dev_err(dev->dev, "Failed to get MIB\n");
		return;
	}

	/* count resets upon read */
	lan937x_pread32(dev, port, REG_PORT_MIB_DATA, &data);
	*cnt += data;
}

void lan937x_r_mib_pkt(struct ksz_device *dev, int port, u16 addr,
		       u64 *dropped, u64 *cnt)
{
	addr = lan937x_mib_names[addr].index;
	lan937x_r_mib_cnt(dev, port, addr, cnt);
}

static void lan937x_r_mib_stats64(struct ksz_device *dev, int port)
{
	struct ksz_port_mib *mib = &dev->ports[port].mib;
	struct rtnl_link_stats64 *s;
	u64 *ctr = mib->counters;

	s = &mib->stats64;
	spin_lock(&mib->stats64_lock);

	s->rx_packets = ctr[lan937x_mib_rx_mcast] +
			ctr[lan937x_mib_rx_bcast] +
			ctr[lan937x_mib_rx_ucast] +
			ctr[lan937x_mib_rx_pause];

	s->tx_packets = ctr[lan937x_mib_tx_mcast] +
			ctr[lan937x_mib_tx_bcast] +
			ctr[lan937x_mib_tx_ucast] +
			ctr[lan937x_mib_tx_pause];

	s->rx_bytes = ctr[lan937x_mib_rx_total];
	s->tx_bytes = ctr[lan937x_mib_tx_total];

	s->rx_errors = ctr[lan937x_mib_rx_fragments] +
			ctr[lan937x_mib_rx_jabbers] +
			ctr[lan937x_mib_rx_sym_err] +
			ctr[lan937x_mib_rx_align_err] +
			ctr[lan937x_mib_rx_crc_err];

	s->tx_errors = ctr[lan937x_mib_tx_exc_col] +
			ctr[lan937x_mib_tx_late_col];

	s->rx_dropped = ctr[lan937x_mib_rx_discard];
	s->tx_dropped = ctr[lan937x_mib_tx_discard];
	s->multicast = ctr[lan937x_mib_rx_mcast];

	s->collisions = ctr[lan937x_mib_tx_late_col] +
			ctr[lan937x_mib_tx_single_col] +
			ctr[lan937x_mib_tx_mult_col];

	s->rx_length_errors = ctr[lan937x_mib_rx_fragments] +
			ctr[lan937x_mib_rx_jabbers];

	s->rx_crc_errors = ctr[lan937x_mib_rx_crc_err];
	s->rx_frame_errors = ctr[lan937x_mib_rx_align_err];
	s->tx_aborted_errors = ctr[lan937x_mib_tx_exc_col];
	s->tx_window_errors = ctr[lan937x_mib_tx_late_col];

	spin_unlock(&mib->stats64_lock);
}

static void lan937x_port_init_cnt(struct ksz_device *dev, int port)
{
	struct ksz_port_mib *mib = &dev->ports[port].mib;

	/* flush all enabled port MIB counters */
	mutex_lock(&mib->cnt_mutex);
	lan937x_pwrite32(dev, port, REG_PORT_MIB_CTRL_STAT,
			 MIB_COUNTER_FLUSH_FREEZE);
	ksz_write8(dev, REG_SW_MAC_CTRL_6, SW_MIB_COUNTER_FLUSH);
	lan937x_pwrite32(dev, port, REG_PORT_MIB_CTRL_STAT, 0);
	mutex_unlock(&mib->cnt_mutex);
}

int lan937x_reset_switch(struct ksz_device *dev)
{
	u32 data32;
	u8 data8;
	int ret;

	/* reset switch */
	ret = lan937x_cfg(dev, REG_SW_OPERATION, SW_RESET, true);
	if (ret < 0)
		return ret;

	ret = ksz_read8(dev, REG_SW_LUE_CTRL_1, &data8);
	if (ret < 0)
		return ret;

	/* Enable Auto Aging */
	ret = ksz_write8(dev, REG_SW_LUE_CTRL_1, data8 | SW_LINK_AUTO_AGING);
	if (ret < 0)
		return ret;

	/* disable interrupts */
	ret = ksz_write32(dev, REG_SW_INT_MASK__4, SWITCH_INT_MASK);
	if (ret < 0)
		return ret;

	ret = ksz_write32(dev, REG_SW_PORT_INT_MASK__4, 0xFF);
	if (ret < 0)
		return ret;

	return ksz_read32(dev, REG_SW_PORT_INT_STATUS__4, &data32);
}

static int lan937x_switch_detect(struct ksz_device *dev)
{
	u32 id32;
	int ret;

	/* Read Chip ID */
	ret = ksz_read32(dev, REG_CHIP_ID0__1, &id32);
	if (ret < 0)
		return ret;

	if (id32 != 0 && id32 != 0xffffffff) {
		dev->chip_id = id32;
		dev_info(dev->dev, "Chip ID: 0x%x", id32);
		ret = 0;
	} else {
		ret = -EINVAL;
	}

	return ret;
}

int lan937x_enable_spi_indirect_access(struct ksz_device *dev)
{
	u16 data16;
	u8 data8;
	int ret;

	ret = ksz_read8(dev, REG_GLOBAL_CTRL_0, &data8);
	if (ret < 0)
		return ret;

	/* Check if PHY register is blocked */
	if (data8 & SW_PHY_REG_BLOCK) {
		/* Enable Phy access through SPI */
		data8 &= ~SW_PHY_REG_BLOCK;

		ret = ksz_write8(dev, REG_GLOBAL_CTRL_0, data8);
		if (ret < 0)
			return ret;
	}

	ret = ksz_read16(dev, REG_VPHY_SPECIAL_CTRL__2, &data16);
	if (ret < 0)
		return ret;

	/* Allow SPI access */
	data16 |= VPHY_SPI_INDIRECT_ENABLE;
	return ksz_write16(dev, REG_VPHY_SPECIAL_CTRL__2, data16);
}

static u32 lan937x_get_port_addr(int port, int offset)
{
	return PORT_CTRL_ADDR(port, offset);
}

bool lan937x_is_internal_phy_port(struct ksz_device *dev, int port)
{
	/* Check if the port is RGMII */
	if (port == LAN937X_RGMII_1_PORT || port == LAN937X_RGMII_2_PORT)
		return false;

	/* Check if the port is SGMII */
	if (port == LAN937X_SGMII_PORT &&
	    GET_CHIP_ID_LSB(dev->chip_id) == CHIP_ID_73)
		return false;

	return true;
}

bool lan937x_is_rgmii_port(struct ksz_device *dev, int port)
{
	/* Check if the port is RGMII */
	if (port == LAN937X_RGMII_1_PORT || port == LAN937X_RGMII_2_PORT)
		return true;

	return false;
}

bool lan937x_is_internal_base_tx_phy_port(struct ksz_device *dev, int port)
{
	/* Check if the port is internal tx phy port */
	if (lan937x_is_internal_phy_port(dev, port) &&
	    port == LAN937X_TXPHY_PORT)
		if ((GET_CHIP_ID_LSB(dev->chip_id) == CHIP_ID_71) ||
		    (GET_CHIP_ID_LSB(dev->chip_id) == CHIP_ID_72))
			return true;

	return false;
}

bool lan937x_is_internal_base_t1_phy_port(struct ksz_device *dev, int port)
{
	/* Check if the port is internal t1 phy port */
	if (lan937x_is_internal_phy_port(dev, port) &&
	    !lan937x_is_internal_base_tx_phy_port(dev, port))
		return true;

	return false;
}

static int lan937x_vphy_ind_addr_wr(struct ksz_device *dev, int addr, int reg)
{
	u16 temp, addr_base;

	if (lan937x_is_internal_base_tx_phy_port(dev, addr))
		addr_base = REG_PORT_TX_PHY_CTRL_BASE;
	else
		addr_base = REG_PORT_T1_PHY_CTRL_BASE;

	/* get register address based on the logical port */
	temp = PORT_CTRL_ADDR(addr, (addr_base + (reg << 2)));

	return ksz_write16(dev, REG_VPHY_IND_ADDR__2, temp);
}

int lan937x_internal_phy_write(struct ksz_device *dev, int addr, int reg,
			       u16 val)
{
	unsigned int value;
	int ret;

	/* Check for internal phy port */
	if (!lan937x_is_internal_phy_port(dev, addr))
		return -EOPNOTSUPP;

	ret = lan937x_vphy_ind_addr_wr(dev, addr, reg);
	if (ret < 0)
		return ret;

	/* Write the data to be written to the VPHY reg */
	ret = ksz_write16(dev, REG_VPHY_IND_DATA__2, val);
	if (ret < 0)
		return ret;

	/* Write the Write En and Busy bit */
	ret = ksz_write16(dev, REG_VPHY_IND_CTRL__2,
			  (VPHY_IND_WRITE | VPHY_IND_BUSY));
	if (ret < 0)
		return ret;

	ret = regmap_read_poll_timeout(dev->regmap[1], REG_VPHY_IND_CTRL__2,
				       value, !(value & VPHY_IND_BUSY), 10,
				       1000);
	if (ret < 0) {
		dev_err(dev->dev, "Failed to write phy register\n");
		return ret;
	}

	return 0;
}

int lan937x_internal_phy_read(struct ksz_device *dev, int addr, int reg,
			      u16 *val)
{
	unsigned int value;
	int ret;

	/* Check for internal phy port, return 0xffff for non-existent phy */
	if (!lan937x_is_internal_phy_port(dev, addr))
		return 0xffff;

	ret = lan937x_vphy_ind_addr_wr(dev, addr, reg);
	if (ret < 0)
		return ret;

	/* Write Read and Busy bit to start the transaction */
	ret = ksz_write16(dev, REG_VPHY_IND_CTRL__2, VPHY_IND_BUSY);
	if (ret < 0)
		return ret;

	ret = regmap_read_poll_timeout(dev->regmap[1], REG_VPHY_IND_CTRL__2,
				       value, !(value & VPHY_IND_BUSY), 10,
				       1000);
	if (ret < 0) {
		dev_err(dev->dev, "Failed to read phy register\n");
		return ret;
	}

	/* Read the VPHY register which has the PHY data */
	return ksz_read16(dev, REG_VPHY_IND_DATA__2, val);
}

static void lan937x_config_gbit(struct ksz_device *dev, bool gbit, u8 *data)
{
	if (gbit)
		*data &= ~PORT_MII_NOT_1GBIT;
	else
		*data |= PORT_MII_NOT_1GBIT;
}

static void lan937x_update_rgmii_tx_rx_delay(struct ksz_device *dev, int port,
					      bool is_tx)
{
	u16 data16;
	int reg;
	u8 val;

	/* Apply different codes based on the ports as per characterization
	 * results
	 */
	if (is_tx) {
		reg = REG_PORT_XMII_CTRL_5;
		val = (port == LAN937X_RGMII_1_PORT) ? RGMII_1_TX_DELAY_2NS :
						       RGMII_2_TX_DELAY_2NS;
	} else {
		reg = REG_PORT_XMII_CTRL_4;
		val = (port == LAN937X_RGMII_1_PORT) ? RGMII_1_RX_DELAY_2NS :
						       RGMII_2_RX_DELAY_2NS;
	}

	lan937x_pread16(dev, port, reg, &data16);

	/* clear tune Adjust */
	data16 &= ~PORT_TUNE_ADJ;
	data16 |= (val << 7);
	lan937x_pwrite16(dev, port, reg, data16);

	data16 |= PORT_DLL_RESET;
	/* write DLL reset to take effect */
	lan937x_pwrite16(dev, port, reg, data16);
}

static void lan937x_apply_rgmii_delay(struct ksz_device *dev, int port,
				      phy_interface_t interface, u8 val)
{
	struct ksz_port *p = &dev->ports[port];

	/* Clear Ingress & Egress internal delay enabled bits */
	val &= ~(PORT_RGMII_ID_EG_ENABLE | PORT_RGMII_ID_IG_ENABLE);

	/* if the delay is 0, do not enable DLL */
	if (p->rgmii_tx_val) {
		lan937x_update_rgmii_tx_rx_delay(dev, port, true);
		dev_info(dev->dev, "Applied rgmii tx delay for the port %d\n",
			 port);
		val |= PORT_RGMII_ID_EG_ENABLE;
	}

	/* if the delay is 0, do not enable DLL */
	if (p->rgmii_rx_val) {
		lan937x_update_rgmii_tx_rx_delay(dev, port, false);
		dev_info(dev->dev, "Applied rgmii rx delay for the port %d\n",
			 port);
		val |= PORT_RGMII_ID_IG_ENABLE;
	}

	/* Enable RGMII internal delays */
	lan937x_pwrite8(dev, port, REG_PORT_XMII_CTRL_1, val);
}

void lan937x_mac_config(struct ksz_device *dev, int port,
			phy_interface_t interface)
{
	u8 data8;

	lan937x_pread8(dev, port, REG_PORT_XMII_CTRL_1, &data8);

	/* clear MII selection & set it based on interface later */
	data8 &= ~PORT_MII_SEL_M;

	/* configure MAC based on interface */
	switch (interface) {
	case PHY_INTERFACE_MODE_MII:
		lan937x_config_gbit(dev, false, &data8);
		data8 |= PORT_MII_SEL;
		break;
	case PHY_INTERFACE_MODE_RMII:
		lan937x_config_gbit(dev, false, &data8);
		data8 |= PORT_RMII_SEL;
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
		lan937x_config_gbit(dev, true, &data8);
		data8 |= PORT_RGMII_SEL;

		/* Apply rgmii internal delay for the mac */
		lan937x_apply_rgmii_delay(dev, port, interface, data8);

		/* rgmii delay configuration is already applied above,
		 * hence return from here as no changes required
		 */
		return;
	default:
		dev_err(dev->dev, "Unsupported interface '%s' for port %d\n",
			phy_modes(interface), port);
		return;
	}

	/* Write the updated value */
	lan937x_pwrite8(dev, port, REG_PORT_XMII_CTRL_1, data8);
}

void lan937x_config_interface(struct ksz_device *dev, int port,
			      int speed, int duplex,
			      bool tx_pause, bool rx_pause)
{
	u8 xmii_ctrl0, xmii_ctrl1;

	lan937x_pread8(dev, port, REG_PORT_XMII_CTRL_0, &xmii_ctrl0);
	lan937x_pread8(dev, port, REG_PORT_XMII_CTRL_1, &xmii_ctrl1);

	switch (speed) {
	case SPEED_1000:
		lan937x_config_gbit(dev, true, &xmii_ctrl1);
		break;
	case SPEED_100:
		lan937x_config_gbit(dev, false, &xmii_ctrl1);
		xmii_ctrl0 |= PORT_MAC_SPEED_100;
		break;
	case SPEED_10:
		lan937x_config_gbit(dev, false, &xmii_ctrl1);
		xmii_ctrl0 &= ~PORT_MAC_SPEED_100;
		break;
	default:
		dev_err(dev->dev, "Unsupported speed on port %d: %d\n",
			port, speed);
		return;
	}

	if (duplex)
		xmii_ctrl0 |= PORT_FULL_DUPLEX;
	else
		xmii_ctrl0 &= ~PORT_FULL_DUPLEX;

	if (tx_pause)
		xmii_ctrl0 |= PORT_TX_FLOW_CTRL;
	else
		xmii_ctrl1 &= ~PORT_TX_FLOW_CTRL;

	if (rx_pause)
		xmii_ctrl0 |= PORT_RX_FLOW_CTRL;
	else
		xmii_ctrl0 &= ~PORT_RX_FLOW_CTRL;

	lan937x_pwrite8(dev, port, REG_PORT_XMII_CTRL_0, xmii_ctrl0);
	lan937x_pwrite8(dev, port, REG_PORT_XMII_CTRL_1, xmii_ctrl1);
}

void lan937x_port_setup(struct ksz_device *dev, int port, bool cpu_port)
{
	struct dsa_switch *ds = dev->ds;
	u8 member;

	/* enable tag tail for host port */
	if (cpu_port)
		lan937x_port_cfg(dev, port, REG_PORT_CTRL_0,
				 PORT_TAIL_TAG_ENABLE, true);

	/* disable frame check length field */
	lan937x_port_cfg(dev, port, REG_PORT_MAC_CTRL_0, PORT_FR_CHK_LENGTH,
			 false);

	/* set back pressure for half duplex */
	lan937x_port_cfg(dev, port, REG_PORT_MAC_CTRL_1, PORT_BACK_PRESSURE,
			 true);

	/* enable 802.1p priority */
	lan937x_port_cfg(dev, port, P_PRIO_CTRL, PORT_802_1P_PRIO_ENABLE, true);

	if (!lan937x_is_internal_phy_port(dev, port))
		lan937x_port_cfg(dev, port, REG_PORT_XMII_CTRL_0,
				 PORT_TX_FLOW_CTRL | PORT_RX_FLOW_CTRL,
				 true);

	if (dsa_is_cpu_port(ds, port))
		member = (dsa_user_ports(ds) | BIT(dev->dsa_port));
	else
		member = BIT(dsa_upstream_port(ds, port));

	lan937x_cfg_port_member(dev, port, member);
}

static int lan937x_sw_mdio_read(struct mii_bus *bus, int addr, int regnum)
{
	struct ksz_device *dev = bus->priv;
	u16 val;
	int ret;

	if (regnum & MII_ADDR_C45)
		return -EOPNOTSUPP;

	ret = lan937x_internal_phy_read(dev, addr, regnum, &val);
	if (ret < 0)
		return ret;

	return val;
}

static irqreturn_t lan937x_switch_irq_thread(int irq, void *dev_id)
{
	struct ksz_device *dev = dev_id;
	irqreturn_t result = IRQ_NONE;
	u32 data;
	int port;
	int ret;

	/* Read global interrupt status register */
	ret = ksz_read32(dev, REG_SW_INT_STATUS__4, &data);
	if (ret)
		return result;

	if (data & POR_READY_INT) {
		ret = ksz_write32(dev, REG_SW_INT_STATUS__4, POR_READY_INT);
		if (ret)
			return result;
	}

	/*Read the Port Interrupt status register */
	ret = ksz_read32(dev, REG_SW_PORT_INT_STATUS__4, &data);
	if (ret)
		return result;

	for (port = 0; port < dev->port_cnt; port++) {
		if (data & BIT(port)) {
			u32 prtaddr;
			u8 data8;

			prtaddr = PORT_CTRL_ADDR(port, REG_PORT_INT_STATUS);

			/* Read port interrupt status register */
			ret = ksz_read8(dev, prtaddr, &data8);
			if (ret)
				return result;

			if (data8 & PORT_PTP_INT) {
				if (lan937x_ptp_port_interrupt(dev, port) !=
				    IRQ_NONE)
					result = IRQ_HANDLED;
			}

			if (data8 & PORT_ACL_INT) {
				if (lan937x_acl_isr(dev, port) != IRQ_NONE)
					result = IRQ_HANDLED;
			}

			if (data8 & PORT_QCI_INT) {
				if (lan937x_qci_cntr_isr(dev, port) != IRQ_NONE)
					result = IRQ_HANDLED;
			}
		}
	}

	return result;
}

static int lan937x_enable_port_interrupts(struct ksz_device *dev, bool enable)
{
	u32 data, mask;
	int ret;

	ret = ksz_read32(dev, REG_SW_PORT_INT_MASK__4, &data);
	if (ret)
		return ret;

	/* 0 means enabling the interrupts */
	mask = ((1 << dev->port_cnt) - 1);

	if (enable)
		data &= ~mask;
	else
		data |= mask;

	ret = ksz_write32(dev, REG_SW_PORT_INT_MASK__4, data);
	if (ret)
		return ret;

	return 0;
}

static int lan937x_sw_mdio_write(struct mii_bus *bus, int addr, int regnum,
				 u16 val)
{
	struct ksz_device *dev = bus->priv;

	if (regnum & MII_ADDR_C45)
		return -EOPNOTSUPP;

	return lan937x_internal_phy_write(dev, addr, regnum, val);
}

static int lan937x_mdio_register(struct ksz_device *dev)
{
	struct dsa_switch *ds = dev->ds;
	struct device_node *mdio_np;
	struct mii_bus *bus;
	int ret;

	mdio_np = of_get_child_by_name(dev->dev->of_node, "mdio");
	if (!mdio_np) {
		dev_err(ds->dev, "no MDIO bus node\n");
		return -ENODEV;
	}

	bus = devm_mdiobus_alloc(ds->dev);
	if (!bus) {
		of_node_put(mdio_np);
		return -ENOMEM;
	}

	bus->priv = dev;
	bus->read = lan937x_sw_mdio_read;
	bus->write = lan937x_sw_mdio_write;
	bus->name = "lan937x slave smi";
	snprintf(bus->id, MII_BUS_ID_SIZE, "SMI-%d", dev->smi_index);
	bus->parent = ds->dev;
	bus->phy_mask = ~ds->phys_mii_mask;

	ds->slave_mii_bus = bus;

	ret = devm_of_mdiobus_register(ds->dev, bus, mdio_np);
	if (ret) {
		dev_err(ds->dev, "unable to register MDIO bus %s\n",
			bus->id);
	}

	of_node_put(mdio_np);

	return ret;
}

static int lan937x_switch_init(struct ksz_device *dev)
{
	int i, ret;

	dev->ds->ops = &lan937x_switch_ops;

	ret = lan937x_reset_switch(dev);
	if (ret) {
		dev_err(dev->dev, "failed to reset switch\n");
		return ret;
	}

	/* Check device tree */
	ret = lan937x_check_device_id(dev);
	if (ret < 0)
		return ret;

	dev->port_mask = (1 << dev->port_cnt) - 1;

	dev->reg_mib_cnt = SWITCH_COUNTER_NUM;
	dev->mib_cnt = ARRAY_SIZE(lan937x_mib_names);

	dev->ports = devm_kzalloc(dev->dev,
				  dev->port_cnt * sizeof(struct ksz_port),
				  GFP_KERNEL);
	if (!dev->ports)
		return -ENOMEM;

	for (i = 0; i < dev->port_cnt; i++) {
		spin_lock_init(&dev->ports[i].mib.stats64_lock);
		mutex_init(&dev->ports[i].mib.cnt_mutex);
		dev->ports[i].mib.counters =
			devm_kzalloc(dev->dev,
				     sizeof(u64) * (dev->mib_cnt + 1),
				     GFP_KERNEL);

		if (!dev->ports[i].mib.counters)
			return -ENOMEM;

		dev->ports[i].priv =
			devm_kzalloc(dev->dev,
				     sizeof(struct lan937x_flr_blk),
				     GFP_KERNEL);

		if (!dev->ports[i].priv)
			return -ENOMEM;
	}

	/* set the real number of ports */
	dev->ds->num_ports = dev->port_cnt;

	if (dev->irq > 0) {
		unsigned long irqflags =
			irqd_get_trigger_type(irq_get_irq_data(dev->irq));

		irqflags |= IRQF_ONESHOT;
		irqflags |= IRQF_SHARED;
		ret = devm_request_threaded_irq(dev->dev, dev->irq, NULL,
						lan937x_switch_irq_thread,
						irqflags, dev_name(dev->dev),
						dev);
		if (ret) {
			dev_err(dev->dev, "failed to request IRQ.\n");
			return ret;
		}

		ret = lan937x_enable_port_interrupts(dev, true);
		if (ret)
			return ret;
	}

	return 0;
}

static void lan937x_switch_exit(struct ksz_device *dev)
{
	lan937x_reset_switch(dev);
}

static int lan937x_init(struct ksz_device *dev)
{
	int ret;

	ret = lan937x_switch_init(dev);
	if (ret < 0) {
		dev_err(dev->dev, "failed to initialize the switch");
		return ret;
	}

	/* enable Indirect Access from SPI to the VPHY registers */
	ret = lan937x_enable_spi_indirect_access(dev);
	if (ret < 0) {
		dev_err(dev->dev, "failed to enable spi indirect access");
		return ret;
	}

	ret = lan937x_mdio_register(dev);
	if (ret < 0) {
		dev_err(dev->dev, "failed to register the mdio");
		return ret;
	}

	return 0;
}

const struct ksz_dev_ops lan937x_dev_ops = {
	.get_port_addr = lan937x_get_port_addr,
	.cfg_port_member = lan937x_cfg_port_member,
	.flush_dyn_mac_table = lan937x_flush_dyn_mac_table,
	.port_setup = lan937x_port_setup,
	.r_mib_cnt = lan937x_r_mib_cnt,
	.r_mib_pkt = lan937x_r_mib_pkt,
	.port_init_cnt = lan937x_port_init_cnt,
	.r_mib_stat64 = lan937x_r_mib_stats64,
	.shutdown = lan937x_reset_switch,
	.detect = lan937x_switch_detect,
	.init = lan937x_init,
	.exit = lan937x_switch_exit,
};
