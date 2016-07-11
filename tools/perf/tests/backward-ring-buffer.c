/*
 * Test backward bit in event attribute, read ring buffer from end to
 * beginning
 */

#include <perf.h>
#include <evlist.h>
#include <sys/prctl.h>
#include "tests.h"
#include "debug.h"

#define NR_ITERS 111

static void testcase(void)
{
	int i;

	for (i = 0; i < NR_ITERS; i++) {
		char proc_name[10];

		snprintf(proc_name, sizeof(proc_name), "p:%d\n", i);
		prctl(PR_SET_NAME, proc_name);
	}
}

static void perf_evlist__mmap_read_catchup_all(struct perf_evlist *evlist)
{
	int i;

	for (i = 0; i < evlist->nr_mmaps; i++)
		perf_evlist__mmap_read_catchup(evlist, i);
}

static int count_samples(struct perf_evlist *evlist, int *sample_count,
			 int *comm_count)
{
	int i;
	union perf_event *(*reader)(struct perf_evlist *, int);

	if (evlist->backward)
		reader = perf_evlist__mmap_read_backward;
	else
		reader = perf_evlist__mmap_read_forward;

	for (i = 0; i < evlist->nr_mmaps; i++) {
		union perf_event *event;

		/*
		 * Before calling count_samples(), ring buffers in backward
		 * evlist should have catched up with newest record
		 * using perf_evlist__mmap_read_catchup_all().
		 */
		while ((event = (*reader)(evlist, i)) != NULL) {
			const u32 type = event->header.type;

			switch (type) {
			case PERF_RECORD_SAMPLE:
				(*sample_count)++;
				break;
			case PERF_RECORD_COMM:
				(*comm_count)++;
				break;
			default:
				pr_err("Unexpected record of type %d\n", type);
				return TEST_FAIL;
			}
		}
	}
	return TEST_OK;
}

struct test_result {
	int sys_enter;
	int sys_exit;
	int comm;
};

static int do_test(struct perf_evlist *evlist,
		   struct perf_evlist *aux_evlist,
		   int mmap_pages,
		   struct test_result *res)
{
	int err;
	char sbuf[STRERR_BUFSIZE];

	err = perf_evlist__mmap(evlist, mmap_pages, false);
	if (err < 0) {
		pr_debug("perf_evlist__mmap: %s\n",
			 strerror_r(errno, sbuf, sizeof(sbuf)));
		return TEST_FAIL;
	}

	err = perf_evlist__mmap(aux_evlist, mmap_pages, true);
	if (err < 0) {
		pr_debug("perf_evlist__mmap for aux_evlist: %s\n",
			 strerror_r(errno, sbuf, sizeof(sbuf)));
		return TEST_FAIL;
	}

	perf_evlist__enable(evlist);
	testcase();
	perf_evlist__disable(evlist);

	perf_evlist__mmap_read_catchup_all(aux_evlist);
	err = count_samples(aux_evlist, &res->sys_exit, &res->comm);
	if (err)
		goto errout;
	err = count_samples(evlist, &res->sys_enter, &res->comm);
	if (err)
		goto errout;
errout:
	perf_evlist__munmap(evlist);
	perf_evlist__munmap(aux_evlist);
	return err;
}


int test__backward_ring_buffer(int subtest __maybe_unused)
{
	int ret = TEST_SKIP, err;
	char pid[16], sbuf[STRERR_BUFSIZE];
	struct perf_evlist *evlist, *aux_evlist = NULL;
	struct perf_evsel *evsel __maybe_unused;
	struct parse_events_error parse_error;
	struct record_opts opts = {
		.target = {
			.uid = UINT_MAX,
			.uses_mmap = true,
		},
		.freq	      = 0,
		.mmap_pages   = 256,
		.default_interval = 1,
	};
	struct test_result res = {0, 0, 0};

	snprintf(pid, sizeof(pid), "%d", getpid());
	pid[sizeof(pid) - 1] = '\0';
	opts.target.tid = opts.target.pid = pid;

	evlist = perf_evlist__new();
	if (!evlist) {
		pr_debug("No ehough memory to create evlist\n");
		return TEST_FAIL;
	}

	err = perf_evlist__create_maps(evlist, &opts.target);
	if (err < 0) {
		pr_debug("Not enough memory to create thread/cpu maps\n");
		goto out_delete_evlist;
	}

	bzero(&parse_error, sizeof(parse_error));
	err = parse_events(evlist, "syscalls:sys_enter_prctl", &parse_error);
	if (err) {
		pr_debug("Failed to parse tracepoint event, try use root\n");
		ret = TEST_SKIP;
		goto out_delete_evlist;
	}

	/*
	 * Set backward bit, ring buffer should be writing from end. Record
	 * it in aux evlist
	 */
	perf_evlist__last(evlist)->attr.write_backward = 1;

	err = parse_events(evlist, "syscalls:sys_exit_prctl", &parse_error);
	if (err) {
		pr_debug("Failed to parse tracepoint event, try use root\n");
		ret = TEST_SKIP;
		goto out_delete_evlist;
	}
	/* Don't set backward bit for exit event. Record it in main evlist */

	perf_evlist__config(evlist, &opts, NULL);

	err = perf_evlist__open(evlist);
	if (err < 0) {
		pr_debug("perf_evlist__open: %s\n",
			 strerror_r(errno, sbuf, sizeof(sbuf)));
		goto out_delete_evlist;
	}

	aux_evlist = perf_evlist__new_aux(evlist);
	if (!aux_evlist) {
		pr_debug("perf_evlist__new_aux failed\n");
		goto out_delete_evlist;
	}
	aux_evlist->backward = true;

	ret = TEST_FAIL;
	err = do_test(evlist, aux_evlist, opts.mmap_pages, &res);
	if (err != TEST_OK)
		goto out_delete_evlist;

	if (res.sys_enter != res.sys_exit) {
		pr_err("Unexpected counter: sys_enter count=%d, sys_exit count=%d\n",
		       res.sys_enter, res.sys_exit);
		goto out_delete_evlist;
	}

	if ((res.sys_exit != NR_ITERS) || (res.comm != NR_ITERS)) {
		pr_err("Unexpected counter: sys_exit count=%d, comm count=%d\n",
		       res.sys_exit, res.comm);
		goto out_delete_evlist;
	}

	err = do_test(evlist, aux_evlist, 1, &res);
	if (err != TEST_OK)
		goto out_delete_evlist;

	ret = TEST_OK;
out_delete_evlist:
	if (aux_evlist)
		perf_evlist__delete(aux_evlist);
	perf_evlist__delete(evlist);
	return ret;
}
