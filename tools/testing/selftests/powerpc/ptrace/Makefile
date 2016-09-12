TEST_PROGS := ptrace-ebb ptrace-gpr ptrace-tm-gpr

all: $(TEST_PROGS)
CFLAGS += -m64
$(TEST_PROGS): ../harness.c ptrace.S ../utils.c ptrace.h
ptrace-ebb: ../pmu/event.c ../pmu/lib.c ../pmu/ebb/ebb_handler.S ../pmu/ebb/busy_loop.S

clean:
	rm -f $(TEST_PROGS) *.o
