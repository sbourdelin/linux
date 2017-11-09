#
# Makefile for the Cavium network device drivers.
#

obj-$(CONFIG_OCTEON_MGMT_ETHERNET)	+= octeon_mgmt.o
obj-$(CONFIG_OCTEON3_BGX_PORT)		+= octeon3-bgx-port.o
obj-$(CONFIG_OCTEON3_BGX_NEXUS)		+= octeon3-bgx-nexus.o
obj-$(CONFIG_OCTEON3_ETHERNET)		+= octeon3-ethernet.o

octeon3-ethernet-objs += octeon3-core.o octeon3-pki.o octeon3-sso.o \
			 octeon3-pko.o
