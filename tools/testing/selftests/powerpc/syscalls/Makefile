TEST_PROGS := ipc_unmuxed spr_exec

CFLAGS += -I../../../../../usr/include

all: $(TEST_PROGS)

$(TEST_PROGS): ../harness.c
spr_exec: ../utils.c

include ../../lib.mk

clean:
	rm -f $(TEST_PROGS) *.o
