TEST_PROGS := ptrace-ebb

include ../../lib.mk

all: $(TEST_PROGS)
CFLAGS += -m64
$(TEST_PROGS): ../harness.c ../utility/utils.c ptrace.h
ptrace-ebb: ../pmu/event.c ../pmu/lib.c ../pmu/ebb/ebb_handler.S ../pmu/ebb/busy_loop.S
ptrace-ebb: CFLAGS += -I../pmu/ebb
clean:
	rm -f $(TEST_PROGS) *.o
