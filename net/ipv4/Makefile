#
# Makefile for the Linux IP layer.
#

obj-y     := route.o inetpeer.o protocol.o \
	     ip_input.o ip_fragment.o ip_forward.o ip_options.o \
	     ip_output.o ip_sockglue.o inet_hashtables.o \
	     inet_timewait_sock.o inet_connection_sock.o \
	     datagram.o raw.o udp.o udplite.o tcp_offload.o tcp_ipv4.o \
	     udp_offload.o arp.o icmp.o devinet.o af_inet.o igmp.o \
	     fib_frontend.o fib_semantics.o fib_trie.o fib_notifier.o \
	     inet_fragment.o ping.o ip_tunnel_core.o gre_offload.o

obj-$(CONFIG_NET_IP_TUNNEL) += ip_tunnel.o
obj-$(CONFIG_SYSCTL) += sysctl_net_ipv4.o
obj-$(CONFIG_PROC_FS) += proc.o
obj-$(CONFIG_IP_MULTIPLE_TABLES) += fib_rules.o
obj-$(CONFIG_IP_MROUTE) += ipmr.o
obj-$(CONFIG_NET_IPIP) += ipip.o
gre-y := gre_demux.o
obj-$(CONFIG_NET_FOU) += fou.o
obj-$(CONFIG_NET_IPGRE_DEMUX) += gre.o
obj-$(CONFIG_NET_IPGRE) += ip_gre.o
obj-$(CONFIG_NET_UDP_TUNNEL) += udp_tunnel.o
obj-$(CONFIG_NET_IPVTI) += ip_vti.o
obj-$(CONFIG_SYN_COOKIES) += syncookies.o
obj-$(CONFIG_INET_AH) += ah4.o
obj-$(CONFIG_INET_ESP) += esp4.o
obj-$(CONFIG_INET_ESP_OFFLOAD) += esp4_offload.o
obj-$(CONFIG_INET_IPCOMP) += ipcomp.o
obj-$(CONFIG_INET_XFRM_TUNNEL) += xfrm4_tunnel.o
obj-$(CONFIG_INET_XFRM_MODE_BEET) += xfrm4_mode_beet.o
obj-$(CONFIG_INET_TUNNEL) += tunnel4.o
obj-$(CONFIG_INET_XFRM_MODE_TRANSPORT) += xfrm4_mode_transport.o
obj-$(CONFIG_INET_XFRM_MODE_TUNNEL) += xfrm4_mode_tunnel.o
obj-$(CONFIG_IP_PNP) += ipconfig.o
obj-$(CONFIG_NETFILTER)	+= netfilter.o netfilter/
obj-$(CONFIG_INET_DIAG) += inet_diag.o 
obj-$(CONFIG_INET_UDP_DIAG) += udp_diag.o
obj-$(CONFIG_INET_RAW_DIAG) += raw_diag.o
obj-$(CONFIG_NETLABEL) += cipso_ipv4.o

obj-$(CONFIG_XFRM) += xfrm4_policy.o xfrm4_state.o xfrm4_input.o \
		      xfrm4_output.o xfrm4_protocol.o
