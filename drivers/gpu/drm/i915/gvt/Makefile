# SPDX-License-Identifier: GPL-2.0
GVT_SOURCE := gvt.o aperture_gm.o handlers.o vgpu.o trace_points.o firmware.o \
	interrupt.o gtt.o cfg_space.o opregion.o mmio.o display.o edid.o \
	execlist.o scheduler.o sched_policy.o mmio_context.o cmd_parser.o debugfs.o \
	fb_decoder.o dmabuf.o page_track.o

ccflags-y				+= -I$(src) -I$(src)/../
i915_gvt-y				:= $(GVT_SOURCE)
obj-$(CONFIG_DRM_I915_GVT_KVMGT)	+= kvmgt.o
obj-$(CONFIG_DRM_I915_GVT)              += i915_gvt.o
