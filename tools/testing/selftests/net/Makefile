# Makefile for net selftests

CFLAGS = -Wall -O2 -g

CFLAGS += -I../../../../usr/include/

%: %.c
	$(CC) $(CFLAGS) -o $@ $^
TEST_PROGS := run_netsocktests run_afpackettests test_bpf.sh
TEST_GEN_FILES := socket psock_fanout psock_tpacket reuseport_bpf reuseport_bpf_cpu reuseport_dualstack

include ../lib.mk

