# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_NET_DSA_MICROCHIP_KSZ_COMMON)	+= ksz_common.o
obj-$(CONFIG_NET_DSA_MICROCHIP_KSZ9477)		+= ksz9477.o
obj-$(CONFIG_NET_DSA_MICROCHIP_KSZ9477_I2C)	+= ksz9477_i2c.o
obj-$(CONFIG_NET_DSA_MICROCHIP_KSZ9477_SPI)	+= ksz9477_spi.o
obj-$(CONFIG_NET_DSA_MICROCHIP_KSZ8795)		+= ksz8795.o
obj-$(CONFIG_NET_DSA_MICROCHIP_KSZ8795_SPI)	+= ksz8795_spi.o
obj-$(CONFIG_NET_DSA_MICROCHIP_KSZ8863_SMI)	+= ksz8863_smi.o

obj-$(CONFIG_NET_DSA_MICROCHIP_LAN937X)		+= lan937x.o
lan937x-objs := lan937x_dev.o
lan937x-objs += lan937x_main.o
lan937x-objs += lan937x_spi.o
lan937x-objs += lan937x_tc.o
lan937x-objs += lan937x_devlink.o

ifdef CONFIG_NET_DSA_MICROCHIP_LAN937X_PTP
lan937x-objs += lan937x_ptp.o
endif

ifdef CONFIG_NET_DSA_MICROCHIP_LAN937X_TAS
lan937x-objs += lan937x_tas.o
endif
lan937x-objs += lan937x_flower.o
lan937x-objs += lan937x_acl.o
