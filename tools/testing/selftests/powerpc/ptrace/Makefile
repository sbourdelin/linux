TEST_PROGS := ptrace-ebb ptrace-gpr ptrace-tm-gpr ptrace-tm-spd-gpr \
ptrace-tar ptrace-tm-tar ptrace-tm-spd-tar

include ../../lib.mk

all: $(TEST_PROGS)
CFLAGS += -m64 -I../tm -mhtm
$(TEST_PROGS): ../harness.c ../utility/reg.S ../utility/utils.c ptrace.h
ptrace-ebb: ../pmu/event.c ../pmu/lib.c ../pmu/ebb/ebb_handler.S ../pmu/ebb/busy_loop.S
ptrace-ebb: CFLAGS += -I../pmu/ebb

clean:
	rm -f $(TEST_PROGS) *.o
