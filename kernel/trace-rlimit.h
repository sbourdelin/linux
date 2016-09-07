
#undef TRACE_SYSTEM
#define TRACE_SYSTEM rlimit

#if !defined(_TRACE_RLIMIT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RLIMIT_H
#include <linux/tracepoint.h>

TRACE_DEFINE_ENUM(RLIMIT_CPU);
TRACE_DEFINE_ENUM(RLIMIT_FSIZE);
TRACE_DEFINE_ENUM(RLIMIT_DATA);
TRACE_DEFINE_ENUM(RLIMIT_STACK);
TRACE_DEFINE_ENUM(RLIMIT_CORE);
TRACE_DEFINE_ENUM(RLIMIT_RSS);
TRACE_DEFINE_ENUM(RLIMIT_NPROC);
TRACE_DEFINE_ENUM(RLIMIT_NOFILE);
TRACE_DEFINE_ENUM(RLIMIT_MEMLOCK);
TRACE_DEFINE_ENUM(RLIMIT_AS);
TRACE_DEFINE_ENUM(RLIMIT_LOCKS);
TRACE_DEFINE_ENUM(RLIMIT_SIGPENDING);
TRACE_DEFINE_ENUM(RLIMIT_MSGQUEUE);
TRACE_DEFINE_ENUM(RLIMIT_NICE);
TRACE_DEFINE_ENUM(RLIMIT_RTPRIO);
TRACE_DEFINE_ENUM(RLIMIT_RTTIME);


#define __print_rlimit_name(id_var)				\
	__print_symbolic(id_var,				\
			 { RLIMIT_CPU, "CPU" },			\
			 { RLIMIT_FSIZE, "FSIZE" },		\
			 { RLIMIT_DATA, "DATA" },		\
			 { RLIMIT_STACK, "STACK" },		\
			 { RLIMIT_CORE, "CORE" },		\
			 { RLIMIT_RSS, "RSS" },			\
			 { RLIMIT_NPROC, "NPROC" },		\
			 { RLIMIT_NOFILE, "NOFILE" },		\
			 { RLIMIT_MEMLOCK, "MEMLOCK" },		\
			 { RLIMIT_AS, "AS" },			\
			 { RLIMIT_LOCKS, "LOCKS" },		\
			 { RLIMIT_SIGPENDING, "SIGPENDING" },	\
			 { RLIMIT_MSGQUEUE, "MSGQUEUE" },	\
			 { RLIMIT_NICE, "NICE" },		\
			 { RLIMIT_RTPRIO, "RTPRIO" },		\
			 { RLIMIT_RTTIME, "RTTIME" })

DECLARE_EVENT_CLASS(rlimit_exceeded_template,

	    TP_PROTO(int rlimit_id,
		     unsigned long long cur,
		     unsigned long long req,
		     pid_t pid,
		     char *comm),

	    TP_ARGS(rlimit_id, cur, req, pid, comm),

	    TP_STRUCT__entry(
		    __field(int, rlimit_id)
		    __field(unsigned long long, cur)
		    __field(unsigned long long, req)
		    __field(pid_t, pid)
		    __string(comm, comm)
		    ),
	    TP_fast_assign(
		    __entry->rlimit_id = rlimit_id;
		    __entry->cur = cur;
		    __entry->req = req;
		    __entry->pid = pid;
		    __assign_str(comm, comm);
		    ),
	    TP_printk("RLIMIT %s violation [%s:%d]. Limit %llu, requested %s",
		      __print_rlimit_name(__entry->rlimit_id),
		      __get_str(comm),
		      __entry->pid,
		      __entry->cur,
		      __print_symbolic(__entry->req,
				       {(unsigned long long)-1, "Unknown"}))
	);

DEFINE_EVENT(rlimit_exceeded_template, rlimit_exceeded,
	    TP_PROTO(int rlimit_id,
		     unsigned long long cur,
		     unsigned long long req,
		     pid_t pid,
		     char *comm),

	    TP_ARGS(rlimit_id, cur, req, pid, comm)
	);

DEFINE_EVENT_PRINT(rlimit_exceeded_template, rlimit_hard_exceeded,
	    TP_PROTO(int rlimit_id,
		     unsigned long long cur,
		     unsigned long long req,
		     pid_t pid,
		     char *comm),

	    TP_ARGS(rlimit_id, cur, req, pid, comm),

	    TP_printk("Hard RLIMIT %s violation [%s:%d]. Limit %llu, requested %s",
		      __print_rlimit_name(__entry->rlimit_id),
		      __get_str(comm),
		      __entry->pid,
		      __entry->cur,
		      __print_symbolic(__entry->req,
				       {(unsigned long long)-1, "Unknown"}))
	);

#endif /* _TRACE_RLIMIT_H */
#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE trace-rlimit
#include <trace/define_trace.h>
