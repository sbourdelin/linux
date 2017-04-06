struct seq_file;
struct request_queue;

void dm_mq_show_q(struct seq_file *m, struct request_queue *q);
