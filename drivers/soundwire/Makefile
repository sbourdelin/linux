#
# Makefile for soundwire core
#

##Bus objs
soundwire-bus-objs := sdw_bus_type.o sdw_bus.o sdw_slave.o sdw_mipi.o \
		sdw_sysfs.o sdw_runtime.o sdw_stream.o
obj-$(CONFIG_SOUNDWIRE_BUS) += soundwire-bus.o

##Intel Objs

#link driver
soundwire-cadence-objs := sdw_cadence.o sdw_cadence_dai.o
obj-$(CONFIG_SOUNDWIRE_CADENCE)	+= soundwire-cadence.o
