#
# Makefile for Linux TCP
#

obj-y     := tcp.o tcp_input.o tcp_output.o tcp_timer.o \
	     tcp_minisocks.o tcp_cong.o tcp_metrics.o tcp_fastopen.o \
	     tcp_rate.o tcp_recovery.o tcp_ulp.o

obj-$(CONFIG_INET_TCP_DIAG)     +=  tcp_diag.o
obj-$(CONFIG_NET_TCPPROBE)      +=  tcp_probe.o

obj-$(CONFIG_TCP_CONG_ADVANCED) +=  cc_algos/
