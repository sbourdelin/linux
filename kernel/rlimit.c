
#include <linux/resource.h>

#define CREATE_TRACE_POINTS
#include "trace-rlimit.h"

void rlimit_exceeded_task(int rlimit_id, u64 req, struct task_struct *task)
{
	trace_rlimit_exceeded(rlimit_id, task_rlimit(task, rlimit_id), req,
			      task_pid_nr(task), task->comm);
}

void rlimit_exceeded(int rlimit_id, u64 req)
{
	rlimit_exceeded_task(rlimit_id, req, current);
}

void rlimit_hard_exceeded_task(int rlimit_id, u64 req, struct task_struct *task)
{
	trace_rlimit_hard_exceeded(rlimit_id, task_rlimit_max(task, rlimit_id),
				   req, task_pid_nr(task), task->comm);
}
void rlimit_hard_exceeded(int rlimit_id, u64 req)
{
	rlimit_hard_exceeded_task(rlimit_id, req, current);
}
