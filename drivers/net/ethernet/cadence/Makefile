#
# Makefile for the Atmel network device drivers.
#

cadence-macb-y	:= macb.o

ifeq ($(CONFIG_MACB_USE_HWSTAMP),y)
cadence-macb-y	+= macb_ptp.o
endif

obj-$(CONFIG_MACB) += cadence-macb.o
