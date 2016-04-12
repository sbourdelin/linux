/*
 */

#ifdef CONFIG_RTC_CYCLIC
extern void dequeue_task_rt2(struct rq *rq, struct task_struct *p, int flags);
extern void requeue_task_rt2(struct rq *rq, struct task_struct *p, int head);
#endif
