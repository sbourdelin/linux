#include <linux/bitops.h>
#include <linux/fdtable.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

/**
 * fdmap - get opened file descriptors of a process
 * @pid: the pid of the target process
 * @fds: allocated userspace buffer
 * @count: buffer size (in descriptors)
 * @start_fd: first descriptor to search from (inclusive)
 * @flags: reserved for future functionality, must be zero
 *
 * If @pid is zero then it's current process.
 * Return: number of descriptors written. An error code otherwise.
 */
SYSCALL_DEFINE5(fdmap, pid_t, pid, int __user *, fds, unsigned int, count,
		int, start_fd, int, flags)
{
	struct task_struct *task;
	struct files_struct *files;
	unsigned long search_mask;
	unsigned int user_index, offset;
	int masksize;

	if (start_fd < 0 || flags != 0)
		return -EINVAL;

	if (!pid) {
		files = get_files_struct(current);
	} else {
		rcu_read_lock();
		task = find_task_by_vpid(pid);
		if (!task) {
			rcu_read_unlock();
			return -ESRCH;
		}
		if (!ptrace_may_access(task, PTRACE_MODE_READ_REALCREDS)) {
			rcu_read_unlock();
			return -EACCES;
		}
		files = get_files_struct(task);
		rcu_read_unlock();
	}
	if (!files)
		return 0;

	offset = start_fd / BITS_PER_LONG;
	search_mask = ULONG_MAX << (start_fd % BITS_PER_LONG);
	user_index = 0;
#define FDS_BUF_SIZE	(512/sizeof(unsigned long))
	masksize = FDS_BUF_SIZE;
	while (user_index < count && masksize == FDS_BUF_SIZE) {
		unsigned long open_fds[FDS_BUF_SIZE];
		struct fdtable *fdt;
		unsigned int i;

		/*
		 * fdt->max_fds can grow, get it every time
		 * before copying part into internal buffer.
		 */
		rcu_read_lock();
		fdt = files_fdtable(files);
		masksize = fdt->max_fds / 8 - offset * sizeof(long);
		if (masksize < 0) {
			rcu_read_unlock();
			break;
		}
		masksize = min(masksize, (int)sizeof(open_fds));
		memcpy(open_fds, fdt->open_fds + offset, masksize);
		rcu_read_unlock();

		open_fds[0] &= search_mask;
		search_mask = ULONG_MAX;
		masksize = (masksize + sizeof(long) - 1) / sizeof(long);
		start_fd = offset * BITS_PER_LONG;
		/*
		 * for_each_set_bit_from() can re-read first word
		 * multiple times which is not optimal.
		 */
		for (i = 0; i < masksize; i++) {
			unsigned long mask = open_fds[i];

			while (mask) {
				unsigned int real_fd = start_fd + __ffs(mask);

				if (put_user(real_fd, fds + user_index)) {
					put_files_struct(files);
					return -EFAULT;
				}
				if (++user_index >= count)
					goto out;
				mask &= mask - 1;
			}
			start_fd += BITS_PER_LONG;
		}
		offset += FDS_BUF_SIZE;
	}
out:
	put_files_struct(files);

	return user_index;
}
