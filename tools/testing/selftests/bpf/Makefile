LIBDIR := ../../../lib
BPFOBJ := $(LIBDIR)/bpf/bpf.o

CFLAGS += -Wall -O2 -lcap -I../../../include/uapi -I$(LIBDIR)

<<<<<<< HEAD
test_objs = test_verifier test_tag test_maps test_lru_map test_lpm_map

TEST_PROGS := $(test_objs) test_kmod.sh
TEST_FILES := $(test_objs)

.PHONY: all clean force

all: $(test_objs)

=======
TEST_GEN_PROGS = test_verifier test_tag test_maps test_lru_map test_lpm_map

TEST_PROGS := test_kmod.sh

.PHONY: all clean force

>>>>>>> linux-next/akpm-base
# force a rebuild of BPFOBJ when its dependencies are updated
force:

$(BPFOBJ): force
	$(MAKE) -C $(dir $(BPFOBJ))
<<<<<<< HEAD

$(test_objs): $(BPFOBJ)

include ../lib.mk
=======
>>>>>>> linux-next/akpm-base

$(test_objs): $(BPFOBJ)

include ../lib.mk
