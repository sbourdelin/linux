#undef TRACE_SYSTEM
#define TRACE_SYSTEM tcp

#if !defined(_TRACE_TCP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TCP_H

#include <linux/tracepoint.h>
#include <net/sock.h>
#include <net/inet_timewait_sock.h>
#include <net/request_sock.h>
#include <net/inet_sock.h>
#include <net/tcp_states.h>

TRACE_EVENT(tcp_set_state,
	TP_PROTO(struct sock *sk, int oldstate, int newstate),
	TP_ARGS(sk, oldstate, newstate),

	TP_STRUCT__entry(
		__field(__be32, dst)
		__field(__be32, src)
		__field(__u16, dport)
		__field(__u16, sport)
		__field(int, oldstate)
		__field(int, newstate)
	),

	TP_fast_assign(
		if (oldstate == TCP_TIME_WAIT) {
			__entry->dst = inet_twsk(sk)->tw_daddr;
			__entry->src = inet_twsk(sk)->tw_rcv_saddr;
			__entry->dport = ntohs(inet_twsk(sk)->tw_dport);
			__entry->sport = ntohs(inet_twsk(sk)->tw_sport);
		} else if (oldstate == TCP_NEW_SYN_RECV) {
			__entry->dst = inet_rsk(inet_reqsk(sk))->ir_rmt_addr;
			__entry->src = inet_rsk(inet_reqsk(sk))->ir_loc_addr;
			__entry->dport =
				ntohs(inet_rsk(inet_reqsk(sk))->ir_rmt_port);
			__entry->sport = inet_rsk(inet_reqsk(sk))->ir_num;
		} else {
			__entry->dst = inet_sk(sk)->inet_daddr;
			__entry->src = inet_sk(sk)->inet_rcv_saddr;
			__entry->dport = ntohs(inet_sk(sk)->inet_dport);
			__entry->sport = ntohs(inet_sk(sk)->inet_sport);
		}

		__entry->oldstate = oldstate;
		__entry->newstate = newstate;
	),

	TP_printk("%08X:%04X %08X:%04X, %02x %02x",
		__entry->src, __entry->sport, __entry->dst, __entry->dport,
		__entry->oldstate, __entry->newstate)
);

#endif

/* This part must be outside protection */
#include <trace/define_trace.h>
