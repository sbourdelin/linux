#include <linux/uaccess.h>
#include <linux/bpfilter.h>
#include <uapi/linux/bpf.h>
#include <linux/wait.h>
#include <linux/kmod.h>
struct sock;

extern struct wait_queue_head bpfilter_get_cmd_wq;
extern struct wait_queue_head bpfilter_reply_wq;

extern bool bpfilter_get_cmd_ready;
extern bool bpfilter_reply_ready;

extern struct bpf_mbox_request bpfilter_get_cmd_mbox;
extern struct bpf_mbox_reply bpfilter_reply_mbox;

bool loaded = false;

int bpfilter_mbox_request(struct sock *sk, int optname, char __user *optval,
			  unsigned int optlen, int kind)
{
	int err;

	if (!loaded) {
		err = request_module("bpfilter");
		printk("request_module %d\n", err);
//		if (err)
//			return err;
		loaded = true;
	}

	bpfilter_get_cmd_mbox.subsys = BPF_MBOX_SUBSYS_BPFILTER;
	bpfilter_get_cmd_mbox.kind = kind;
	bpfilter_get_cmd_mbox.pid = current->pid;
	bpfilter_get_cmd_mbox.cmd = optname;
	bpfilter_get_cmd_mbox.addr = (long) optval;
	bpfilter_get_cmd_mbox.len = optlen;
	bpfilter_get_cmd_ready = true;

	wake_up(&bpfilter_get_cmd_wq);
	wait_event_killable(bpfilter_reply_wq, bpfilter_reply_ready);
	bpfilter_reply_ready = false;

	return bpfilter_reply_mbox.status;
}

int bpfilter_ip_set_sockopt(struct sock *sk, int optname, char __user *optval,
			    unsigned int optlen)
{
	return bpfilter_mbox_request(sk, optname, optval, optlen,
				     BPF_MBOX_KIND_SET);
}

int bpfilter_ip_get_sockopt(struct sock *sk, int optname, char __user *optval,
			    int __user *optlen)
{
	int len;

	if (get_user(len, optlen))
		return -EFAULT;

	return bpfilter_mbox_request(sk, optname, optval, len,
				     BPF_MBOX_KIND_GET);
}
