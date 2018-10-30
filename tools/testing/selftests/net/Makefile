# SPDX-License-Identifier: GPL-2.0
# Makefile for net selftests

CFLAGS =  -Wall -Wl,--no-as-needed -O2 -g
CFLAGS += -I../../../../usr/include/

TEST_PROGS := run_netsocktests run_afpackettests test_bpf.sh netdevice.sh rtnetlink.sh
TEST_PROGS += fib_tests.sh fib-onlink-tests.sh pmtu.sh udpgso.sh ip_defrag.sh
TEST_PROGS += udpgso_bench.sh fib_rule_tests.sh msg_zerocopy.sh psock_snd.sh
TEST_PROGS_EXTENDED := in_netns.sh
TEST_GEN_FILES =  socket
TEST_GEN_FILES += psock_fanout psock_tpacket msg_zerocopy
TEST_GEN_FILES += tcp_mmap tcp_inq psock_snd
TEST_GEN_FILES += udpgso udpgso_bench_tx udpgso_bench_rx ip_defrag
TEST_GEN_PROGS = reuseport_bpf reuseport_bpf_cpu reuseport_bpf_numa
TEST_GEN_PROGS += reuseport_dualstack reuseaddr_conflict tls

KSFT_KHDR_INSTALL := 1

# Allows pointing LLC/CLANG to a LLVM backend with bpf support, redefine on cmdline:
#  make samples/bpf/ LLC=~/git/llvm/build/bin/llc CLANG=~/git/llvm/build/bin/clang
LLC ?= llc
CLANG ?= clang
LLVM_OBJCOPY ?= llvm-objcopy
BTF_PAHOLE ?= pahole
HAS_LLC := $(shell which $(LLC) 2>/dev/null)

# conditional enable testes requiring llc
ifneq (, $(HAS_LLC))
TEST_GEN_FILES += xdp_dummy.o
endif

include ../lib.mk

ifneq (, $(HAS_LLC))

# Detect that we're cross compiling and use the cross compiler
ifdef CROSS_COMPILE
CLANG_ARCH_ARGS = -target $(ARCH)
endif

PROBE := $(shell $(LLC) -march=bpf -mcpu=probe -filetype=null /dev/null 2>&1)

# Let newer LLVM versions transparently probe the kernel for availability
# of full BPF instruction set.
ifeq ($(PROBE),)
  CPU ?= probe
else
  CPU ?= generic
endif

SRC_PATH := $(abspath ../../../..)
LIB_PATH := $(SRC_PATH)/tools/lib
XDP_CFLAGS := -D SUPPORT_XDP=1 -I$(LIB_PATH)
LIBBPF = $(LIB_PATH)/bpf/libbpf.a
BTF_LLC_PROBE := $(shell $(LLC) -march=bpf -mattr=help 2>&1 | grep dwarfris)
BTF_PAHOLE_PROBE := $(shell $(BTF_PAHOLE) --help 2>&1 | grep BTF)
BTF_OBJCOPY_PROBE := $(shell $(LLVM_OBJCOPY) --help 2>&1 | grep -i 'usage.*llvm')
CLANG_SYS_INCLUDES := $(shell $(CLANG) -v -E - </dev/null 2>&1 \
        | sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }')
CLANG_FLAGS = -I. -I$(SRC_PATH)/include -I../bpf/ \
	      $(CLANG_SYS_INCLUDES) -Wno-compare-distinct-pointer-types

ifneq ($(and $(BTF_LLC_PROBE),$(BTF_PAHOLE_PROBE),$(BTF_OBJCOPY_PROBE)),)
	CLANG_CFLAGS += -g
	LLC_FLAGS += -mattr=dwarfris
	DWARF2BTF = y
endif

$(LIBBPF): FORCE
# Fix up variables inherited from Kbuild that tools/ build system won't like
	$(MAKE) -C $(dir $@) RM='rm -rf' LDFLAGS= srctree=$(SRC_PATH) O= $(nodir $@)

$(OUTPUT)/udpgso_bench_rx: $(OUTPUT)/udpgso_bench_rx.c $(LIBBPF)
	$(CC) -o $@ $(XDP_CFLAGS) $(CFLAGS) $(LOADLIBES) $(LDLIBS) $^ -lelf

FORCE:

# bpf program[s] generation
$(OUTPUT)/%.o: %.c
	$(CLANG) $(CLANG_FLAGS) \
		 -O2 -target bpf -emit-llvm -c $< -o - |      \
	$(LLC) -march=bpf -mcpu=$(CPU) $(LLC_FLAGS) -filetype=obj -o $@
ifeq ($(DWARF2BTF),y)
	$(BTF_PAHOLE) -J $@
endif

endif

$(OUTPUT)/reuseport_bpf_numa: LDFLAGS += -lnuma
$(OUTPUT)/tcp_mmap: LDFLAGS += -lpthread
$(OUTPUT)/tcp_inq: LDFLAGS += -lpthread
