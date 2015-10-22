CFLAGS += -Wall
LDFLAGS += -lpthread

TESTS = basic_test basic_percpu_ops_test

basic_percpu_ops_test: basic_percpu_ops_test.c


all: $(TESTS)
%: %.c
	$(CC) $(CFLAGS) -o $@ $^ rseq.c $(LDFLAGS)

clean:
	$(RM) $(TESTS)
