TEST_GEN_PROGS := gettimeofday context_switch mmap_bench futex_bench

CFLAGS += -O2

$(TEST_GEN_PROGS): ../harness.c

EXTRA_CLEAN = *.o

include ../../lib.mk

context_switch: ../utils.c
context_switch: CFLAGS += -maltivec -mvsx -mabi=altivec
context_switch: LDLIBS += -lpthread
