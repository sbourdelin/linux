# SPDX-License-Identifier: GPL-2.0
ifndef CROSS_COMPILE
hostprogs-$(CONFIG_SAMPLE_SECCOMP) := bpf-fancy dropper bpf-direct

KBUILD_HOSTCFLAGS_bpf-fancy.o += -I$(objtree)/usr/include
KBUILD_HOSTCFLAGS_bpf-fancy.o += -idirafter $(objtree)/include
KBUILD_HOSTCFLAGS_bpf-helper.o += -I$(objtree)/usr/include
KBUILD_HOSTCFLAGS_bpf-helper.o += -idirafter $(objtree)/include
bpf-fancy-objs := bpf-fancy.o bpf-helper.o

KBUILD_HOSTCFLAGS_dropper.o += -I$(objtree)/usr/include
KBUILD_HOSTCFLAGS_dropper.o += -idirafter $(objtree)/include
dropper-objs := dropper.o

KBUILD_HOSTCFLAGS_bpf-direct.o += -I$(objtree)/usr/include
KBUILD_HOSTCFLAGS_bpf-direct.o += -idirafter $(objtree)/include
bpf-direct-objs := bpf-direct.o

# Try to match the kernel target.
ifndef CONFIG_64BIT

# s390 has -m31 flag to build 31 bit binaries
ifndef CONFIG_S390
MFLAG = -m32
else
MFLAG = -m31
endif

KBUILD_HOSTCFLAGS_bpf-direct.o += $(MFLAG)
KBUILD_HOSTCFLAGS_dropper.o += $(MFLAG)
KBUILD_HOSTCFLAGS_bpf-helper.o += $(MFLAG)
KBUILD_HOSTCFLAGS_bpf-fancy.o += $(MFLAG)
HOSTLOADLIBES_bpf-direct += $(MFLAG)
HOSTLOADLIBES_bpf-fancy += $(MFLAG)
HOSTLOADLIBES_dropper += $(MFLAG)
endif
always := $(hostprogs-m)
endif
