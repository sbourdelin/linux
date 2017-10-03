#
# Makefile for Linux TCP
#

obj-y     := tcp.o tcp_input.o tcp_output.o tcp_timer.o \
	     tcp_minisocks.o tcp_cong.o tcp_metrics.o tcp_fastopen.o \
	     tcp_rate.o tcp_recovery.o tcp_ulp.o

obj-$(CONFIG_INET_TCP_DIAG)     +=  tcp_diag.o
obj-$(CONFIG_NET_TCPPROBE)      +=  tcp_probe.o
obj-$(CONFIG_TCP_CONG_BBR)      +=  tcp_bbr.o
obj-$(CONFIG_TCP_CONG_BIC)      +=  tcp_bic.o
obj-$(CONFIG_TCP_CONG_CDG)      +=  tcp_cdg.o
obj-$(CONFIG_TCP_CONG_CUBIC)    +=  tcp_cubic.o
obj-$(CONFIG_TCP_CONG_DCTCP)    +=  tcp_dctcp.o
obj-$(CONFIG_TCP_CONG_WESTWOOD) +=  tcp_westwood.o
obj-$(CONFIG_TCP_CONG_HSTCP)    +=  tcp_highspeed.o
obj-$(CONFIG_TCP_CONG_HYBLA)    +=  tcp_hybla.o
obj-$(CONFIG_TCP_CONG_HTCP)     +=  tcp_htcp.o
obj-$(CONFIG_TCP_CONG_VEGAS)    +=  tcp_vegas.o
obj-$(CONFIG_TCP_CONG_NV)       +=  tcp_nv.o
obj-$(CONFIG_TCP_CONG_VENO)     +=  tcp_veno.o
obj-$(CONFIG_TCP_CONG_SCALABLE) +=  tcp_scalable.o
obj-$(CONFIG_TCP_CONG_LP)       +=  tcp_lp.o
obj-$(CONFIG_TCP_CONG_YEAH)     +=  tcp_yeah.o
obj-$(CONFIG_TCP_CONG_ILLINOIS) +=  tcp_illinois.o
