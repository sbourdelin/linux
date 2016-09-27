/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Handle /proc entries for SMC sockets
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Ursula Braun <ubraun@linux.vnet.ibm.com>
 */

#include <linux/proc_fs.h>

#include "smc.h"
#include "smc_core.h"
#include "smc_proc.h"

struct smc_proc_sock_list {
	struct list_head list;
	rwlock_t lock;
};

static struct smc_proc_sock_list smc_proc_socket_list = {
	.list = LIST_HEAD_INIT(smc_proc_socket_list.list),
	.lock = __RW_LOCK_UNLOCKED(smc_proc_socket_list.lock),
};

void smc_proc_sock_list_add(struct smc_sock *smc)
{
	write_lock(&smc_proc_socket_list.lock);
	list_add_tail(&smc->proc_list, &smc_proc_socket_list.list);
	write_unlock(&smc_proc_socket_list.lock);
}

void smc_proc_sock_list_del(struct smc_sock *smc)
{
	write_lock(&smc_proc_socket_list.lock);
	if (!list_empty(&smc->proc_list))
		list_del_init(&smc->proc_list);
	write_unlock(&smc_proc_socket_list.lock);
}

#ifdef CONFIG_PROC_FS

static struct proc_dir_entry *proc_fs_smc;

static int smc_proc_gid_to_hex(char *gid, char *buf, int buf_len)
{
	int i;
	int j;

	if (buf_len < (2 * SMC_GID_SIZE + 1))
		return -EINVAL;

	j = 0;
	for (i = 0; i < SMC_GID_SIZE; i++) {
		buf[j++] = hex_asc_hi(gid[i]);
		buf[j++] = hex_asc_lo(gid[i]);
	}
	buf[j] = '\0';

	return 0;
}

static int smc_proc_seq_show_header(struct seq_file *m)
{
	seq_puts(m, "state   uid inode  local_address peer_address  ");
	seq_puts(m, "tcp target   role ");
	seq_puts(m, "gid_peer_0                       ");
	seq_puts(m, "gid_peer_1                       ");
	seq_puts(m, "sndbuf   rmbe     token    peerrmb  rxprodc  rxprodw ");
	seq_puts(m, "rxconsc  rxconsw txprodc  txprodw txconsc  txconsw ");
	seq_puts(m, "tx_flags rx_flags");
	seq_pad(m, '\n');
	return 0;
}

static void *smc_proc_seq_start(struct seq_file *seq, loff_t *pos)
	__acquires(smc_proc_socket_list.lock)
{
	read_lock(&smc_proc_socket_list.lock);

	if (!*pos)
		return SEQ_START_TOKEN;

	return seq_list_start(&smc_proc_socket_list.list, *pos);
}

static void *smc_proc_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	if (v == SEQ_START_TOKEN)
		return seq_list_start(&smc_proc_socket_list.list, *pos);
	return seq_list_next(v, &smc_proc_socket_list.list, pos);
}

static void smc_proc_seq_stop(struct seq_file *seq, void *v)
	__releases(smc_proc_socket_list.lock)
{
	read_unlock(&smc_proc_socket_list.lock);
}

static int smc_proc_seq_show(struct seq_file *m, void *v)
{
	struct smc_sock *smc = list_entry(v, struct smc_sock, proc_list);
	char hex_buf[2 * SMC_GID_SIZE + 1];
	struct sockaddr_in locl_addr;
	struct sockaddr_in peer_addr;
	int len;
	int rc;
	int i;

	if (v == SEQ_START_TOKEN)
		return smc_proc_seq_show_header(m);

	if (!smc)
		return -ENOENT;

	seq_printf(m,
		   "%5d %5d %6ld ",
		   smc->sk.sk_state,
		   from_kuid_munged(seq_user_ns(m), sock_i_uid(&smc->sk)),
		   sock_i_ino(&smc->sk));

	if (smc->sk.sk_state == SMC_INIT)
		goto out_line;

	if (smc->clcsock && smc->clcsock->sk) {
		rc = smc->clcsock->ops->getname(smc->clcsock,
						(struct sockaddr *)&locl_addr,
						&len, 0);
		if (!rc)
			seq_printf(m,
				   "%08X:%04X ",
				   locl_addr.sin_addr.s_addr,
				   locl_addr.sin_port);
		else
			seq_printf(m, "%13s ", " ");
	} else {
		seq_printf(m, "%13s ", " ");
	}

	if (smc->sk.sk_state == SMC_LISTEN)
		goto out_line;

	if (smc->clcsock && smc->clcsock->sk) {
		rc = smc->clcsock->ops->getname(smc->clcsock,
						(struct sockaddr *)&peer_addr,
						&len, 1);
		if (!rc)
			seq_printf(m,
				   "%08X:%04X ",
				   peer_addr.sin_addr.s_addr,
				   peer_addr.sin_port);
		else
			seq_printf(m, "%-13s ", " ");
	} else {
		seq_printf(m, "%13s ", " ");
	}

	seq_printf(m, "%3d ",  smc->use_fallback);
	if (smc->use_fallback)
		goto out_line;

	if (smc->conn.lgr && (smc->sk.sk_state != SMC_CLOSED)) {
		seq_printf(m, "%08X ", smc->conn.lgr->daddr);
		seq_printf(m, "%4d ", smc->conn.lgr->role);

		for (i = 0; i < 2; i++) {
			smc_proc_gid_to_hex(smc->conn.lgr->lnk[i].peer_gid,
					    hex_buf, sizeof(hex_buf));
			seq_printf(m, "%32s ", hex_buf);
		}
	} else {
		seq_printf(m, "%-80s ", " ");
	}

	seq_printf(m,
		   "%08X %08X %08X %08X ",
		   smc->conn.sndbuf_size,
		   smc->conn.rmbe_size,
		   smc->conn.alert_token_local,
		   smc->conn.peer_rmbe_len);
	seq_printf(m,
		   "%08X    %04X %08X    %04X ",
		   smc->conn.local_rx_ctrl.prod.count,
		   smc->conn.local_rx_ctrl.prod.wrap,
		   smc->conn.local_rx_ctrl.cons.count,
		   smc->conn.local_rx_ctrl.cons.wrap);
	seq_printf(m,
		   "%08X    %04X %08X    %04X  ",
		   smc->conn.local_tx_ctrl.prod.count,
		   smc->conn.local_tx_ctrl.prod.wrap,
		   smc->conn.local_tx_ctrl.cons.count,
		   smc->conn.local_tx_ctrl.cons.wrap);
	seq_printf(m,
		   "%02X%02X     %02X%02X     ",
		   *(u8 *)&smc->conn.local_tx_ctrl.prod_flags,
		   *(u8 *)&smc->conn.local_tx_ctrl.conn_state_flags,
		   *(u8 *)&smc->conn.local_rx_ctrl.prod_flags,
		   *(u8 *)&smc->conn.local_rx_ctrl.conn_state_flags);
out_line:
	seq_putc(m, '\n');
	return 0;
}

static const struct seq_operations smc_proc_seq_ops = {
	.start = smc_proc_seq_start,
	.next  = smc_proc_seq_next,
	.stop  = smc_proc_seq_stop,
	.show  = smc_proc_seq_show,
};

static int smc_proc_seq_open(struct inode *inode, struct file *filp)
{
	return seq_open(filp, &smc_proc_seq_ops);
}

static const struct file_operations smc_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= smc_proc_seq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

int __init smc_proc_init(void)
{
	proc_fs_smc = proc_create("smc", S_IFREG | S_IRUGO,
				  init_net.proc_net, &smc_proc_fops);
	return (!proc_fs_smc) ? -EFAULT : 0;
}

void smc_proc_exit(void)
{
	proc_remove(proc_fs_smc);
}

#else /* CONFIG_PROC_FS */
int __init smc_proc_init(void)
{
	return 0;
}

void smc_proc_exit(void)
{
}

#endif /* CONFIG_PROC_FS */
