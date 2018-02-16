# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux BPFILTER layer.
#

hostprogs-y := bpfilter.ko
always := $(hostprogs-y)
bpfilter.ko-objs := bpfilter.o tgts.o targets.o tables.o init.o ctor.o sockopt.o gen.o
HOSTCFLAGS += -I. -Itools/include/
