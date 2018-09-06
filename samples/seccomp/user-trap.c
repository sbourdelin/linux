#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stddef.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/mount.h>
#include <linux/limits.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

/*
 * Because of some grossness, we can't include linux/ptrace.h here, so we
 * re-define PTRACE_SECCOMP_NEW_LISTENER.
 */
#ifndef PTRACE_SECCOMP_NEW_LISTENER
#define PTRACE_SECCOMP_NEW_LISTENER	0x420e
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

static int seccomp(unsigned int op, unsigned int flags, void *args)
{
	errno = 0;
	return syscall(__NR_seccomp, op, flags, args);
}

static int user_trap_syscall(int nr, unsigned int flags)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD+BPF_W+BPF_ABS,
			offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, nr, 0, 1),
		BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
	};

	struct sock_fprog prog = {
		.len = (unsigned short)ARRAY_SIZE(filter),
		.filter = filter,
	};

	return seccomp(SECCOMP_SET_MODE_FILTER, flags, &prog);
}

static int handle_req(struct seccomp_notif *req,
		      struct seccomp_notif_resp *resp, int listener)
{
	char path[PATH_MAX], source[PATH_MAX], target[PATH_MAX];
	int ret = -1, mem;

	resp->len = sizeof(*resp);
	resp->id = req->id;
	resp->error = -EPERM;
	resp->val = 0;

	if (req->data.nr != __NR_mount) {
		fprintf(stderr, "huh? trapped something besides mknod? %d\n", req->data.nr);
		return -1;
	}

	/* Only allow bind mounts. */
	if (!(req->data.args[3] & MS_BIND))
		return 0;

	/*
	 * Ok, let's read the task's memory to see where they wanted their
	 * mount to go.
	 */
	snprintf(path, sizeof(path), "/proc/%d/mem", req->pid);
	mem = open(path, O_RDONLY);
	if (mem < 0) {
		perror("open mem");
		return -1;
	}

	/*
	 * Now we avoid a TOCTOU: we referred to a pid by its pid, but since
	 * the pid that made the syscall may have died, we need to confirm that
	 * the pid is still valid after we open its /proc/pid/mem file. We can
	 * ask the listener fd this as follows.
	 *
	 * Note that this check should occur *after* any task-specific
	 * resources are opened, to make sure that the task has not died and
	 * we're not wrongly reading someone else's state in order to make
	 * decisions.
	 */
	if (ioctl(listener, SECCOMP_NOTIF_IS_ID_VALID, &req->id) != 1) {
		fprintf(stderr, "task died before we could map its memory\n");
		goto out;
	}

	/*
	 * Phew, we've got the right /proc/pid/mem. Now we can read it. Note
	 * that to avoid another TOCTOU, we should read all of the pointer args
	 * before we decide to allow the syscall.
	 */
	if (lseek(mem, req->data.args[0], SEEK_SET) < 0) {
		perror("seek");
		goto out;
	}

	ret = read(mem, source, sizeof(source));
	if (ret < 0) {
		perror("read");
		goto out;
	}

	if (lseek(mem, req->data.args[1], SEEK_SET) < 0) {
		perror("seek");
		goto out;
	}

	ret = read(mem, target, sizeof(target));
	if (ret < 0) {
		perror("read");
		goto out;
	}

	/*
	 * Our policy is to only allow bind mounts inside /tmp. This isn't very
	 * interesting, because we could do unprivlieged bind mounts with user
	 * namespaces already, but you get the idea.
	 */
	if (!strncmp(source, "/tmp", 4) && !strncmp(target, "/tmp", 4)) {
		if (mount(source, target, NULL, req->data.args[3], NULL) < 0) {
			ret = -1;
			perror("actual mount");
			goto out;
		}
		resp->error = 0;
	}

	/* Even if we didn't allow it because of policy, generating the
	 * response was be a success, because we want to tell the worker EPERM.
	 */
	ret = 0;

out:
	close(mem);
	return ret;
}

int main(void)
{
	int sk_pair[2], ret = 1, status, listener;
	pid_t worker = 0 , tracer = 0;
	char c;

	if (socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sk_pair) < 0) {
		perror("socketpair");
		return 1;
	}

	worker = fork();
	if (worker < 0) {
		perror("fork");
		goto close_pair;
	}

	if (worker == 0) {
		if (user_trap_syscall(__NR_mount, 0) < 0) {
			perror("seccomp");
			exit(1);
		}

		if (setuid(1000) < 0) {
			perror("setuid");
			exit(1);
		}

		if (write(sk_pair[1], "a", 1) != 1) {
			perror("write");
			exit(1);
		}

		if (read(sk_pair[1], &c, 1) != 1) {
			perror("write");
			exit(1);
		}

		if (mkdir("/tmp/foo", 0755) < 0) {
			perror("mkdir");
			exit(1);
		}

		if (mount("/dev/sda", "/tmp/foo", NULL, 0, NULL) != -1) {
			fprintf(stderr, "huh? mounted /dev/sda?\n");
			exit(1);
		}

		if (errno != EPERM) {
			perror("bad error from mount");
			exit(1);
		}

		if (mount("/tmp/foo", "/tmp/foo", NULL, MS_BIND, NULL) < 0) {
			perror("mount");
			exit(1);
		}

		exit(0);
	}

	if (read(sk_pair[0], &c, 1) != 1) {
		perror("read ready signal");
		goto out_kill;
	}

	if (ptrace(PTRACE_ATTACH, worker) < 0) {
		perror("ptrace");
		goto out_kill;
	}

	if (waitpid(worker, NULL, 0) != worker) {
		perror("waitpid");
		goto out_kill;
	}

	listener = ptrace(PTRACE_SECCOMP_NEW_LISTENER, worker, 0);
	if (listener < 0) {
		perror("ptrace get listener");
		goto out_kill;
	}

	if (ptrace(PTRACE_DETACH, worker, NULL, 0) < 0) {
		perror("ptrace detach");
		goto out_kill;
	}

	if (write(sk_pair[0], "a", 1) != 1) {
		perror("write");
		exit(1);
	}

	tracer = fork();
	if (tracer < 0) {
		perror("fork");
		goto out_kill;
	}

	if (tracer == 0) {
		while (1) {
			struct seccomp_notif req = {};
			struct seccomp_notif_resp resp = {};

			req.len = sizeof(req);
			if (ioctl(listener, SECCOMP_NOTIF_RECV, &req) != sizeof(req)) {
				perror("ioctl recv");
				goto out_close;
			}

			if (handle_req(&req, &resp, listener) < 0)
				goto out_close;

			if (ioctl(listener, SECCOMP_NOTIF_SEND, &resp) != sizeof(resp)) {
				perror("ioctl send");
				goto out_close;
			}
		}
out_close:
		close(listener);
		exit(1);
	}

	close(listener);

	if (waitpid(worker, &status, 0) != worker) {
		perror("waitpid");
		goto out_kill;
	}

	if (umount2("/tmp/foo", MNT_DETACH) < 0 && errno != EINVAL) {
		perror("umount2");
		goto out_kill;
	}

	if (remove("/tmp/foo") < 0 && errno != ENOENT) {
		perror("remove");
		exit(1);
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "worker exited nonzero\n");
		goto out_kill;
	}

	ret = 0;

out_kill:
	if (tracer > 0)
		kill(tracer, SIGKILL);
	if (worker > 0)
		kill(worker, SIGKILL);

close_pair:
	close(sk_pair[0]);
	close(sk_pair[1]);
	return ret;
}
