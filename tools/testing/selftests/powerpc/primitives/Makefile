CFLAGS += -I$(CURDIR)

TEST_GEN_PROGS := load_unaligned_zeropad

EXTRA_CLEAN = $(OUTPUT)*.o

include ../../lib.mk

$(TEST_GEN_PROGS): ../harness.c
