noarg:
	$(MAKE) -C ../../

TEST_PROGS := bhrb_filters

all: $(TEST_PROGS)

$(TEST_PROGS): ../../harness.c ../event.c ../lib.c bhrb_filters_asm.S

include ../../../lib.mk

clean:
	rm -f $(TEST_PROGS)
