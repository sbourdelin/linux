# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_RTWLAN_CORE)	+= rtwlan.o
rtwlan-y += main.o \
	    mac80211.o \
	    debug.o \
	    tx.o \
	    rx.o \
	    mac.o \
	    phy.o \
	    efuse.o \
	    fw.o \
	    ps.o \
	    sec.o \
	    regd.o

rtwlan-$(CONFIG_RTWLAN_8822BE)	+= rtw8822b.o rtw8822b_table.o
rtwlan-$(CONFIG_RTWLAN_8822CE)	+= rtw8822c.o rtw8822c_table.o

obj-$(CONFIG_RTWLAN_PCI)	+= rtwpci.o
rtwpci-objs			:= pci.o
