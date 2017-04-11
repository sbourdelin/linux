lan9303-objs-y			:= lan9303-core.o
lan9303-objs-$(CONFIG_NET_DSA_SMSC_LAN9303_I2C) += lan9303_i2c.o

obj-$(CONFIG_NET_DSA_MV88E6060) += mv88e6060.o
obj-$(CONFIG_NET_DSA_BCM_SF2)	+= bcm-sf2.o
bcm-sf2-objs			:= bcm_sf2.o bcm_sf2_cfp.o
obj-$(CONFIG_NET_DSA_QCA8K)	+= qca8k.o
obj-$(CONFIG_NET_DSA_SMSC_LAN9303) += lan9303.o
lan9303-objs			:= $(lan9303-objs-y)

obj-y				+= b53/
obj-y				+= mv88e6xxx/
