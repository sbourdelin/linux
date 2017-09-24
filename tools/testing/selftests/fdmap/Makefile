TEST_GEN_PROGS := fdmap_test
CFLAGS += -Wall

include ../lib.mk

$(TEST_GEN_PROGS): fdmap_test.c fdmap.c fdmap.h ../kselftest_harness.h
	$(CC) $(CFLAGS) $(LDFLAGS) $< fdmap.c -o $@
