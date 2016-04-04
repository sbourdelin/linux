TEST_PROGS := ipc_unmuxed spr_exec spr_fork

CFLAGS += -I../../../../../usr/include

all: $(TEST_PROGS)

$(TEST_PROGS): ../harness.c
spr_exec: ../utils.c
spr_fork: ../utils.c

include ../../lib.mk

clean:
	rm -f $(TEST_PROGS) *.o
