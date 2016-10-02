/*
 * Copyright (c) 2016 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <linux/bpf.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <sched.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <time.h>
#include "libbpf.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
#define LOCAL_FREE_TARGET	(128)

static long nr_cpus;

static int create_map(int map_type, unsigned int size)
{
	int map_fd;

	map_fd = bpf_create_map(map_type, sizeof(unsigned long long),
				sizeof(unsigned long long), size, 0);

	if (map_fd == -1)
		perror("bpf_create_map");

	return map_fd;
}

static int map_subset(int map0, int map1)
{
	unsigned long long next_key = 0;
	unsigned long long value0[nr_cpus], value1[nr_cpus];
	int ret;

	while (!bpf_get_next_key(map1, &next_key, &next_key)) {
		assert(!bpf_lookup_elem(map1, &next_key, value1));
		ret = bpf_lookup_elem(map0, &next_key, value0);
		if (ret) {
			printf("key:%llu not found from map. %s(%d)\n",
			       next_key, strerror(errno), errno);
			return 0;
		}
		if (value0[0] != value1[0]) {
			printf("key:%llu value0:%llu != value1:%llu\n",
			       next_key, value0[0], value1[0]);
			return 0;
		}
	}
	return 1;
}

static int map_equal(int lru_map, int expected)
{
	return map_subset(lru_map, expected) && map_subset(expected, lru_map);
}

/* Size of the LRU amp is 2
 * Add key=1 (+1 key)
 * Add key=2 (+1 key)
 * Lookup Key=1
 * Add Key=3
 *   => Key=2 will be removed by LRU
 * Iterate map.  Only found key=1 and key=3
 */
static void test_lru_sanity0(int map_type)
{
	unsigned long long key, value[nr_cpus];
	cpu_set_t cpuset;
	int map_fd, expected_map_fd;

	printf("%s (map_type:%d): ", __func__, map_type);

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	assert(!sched_setaffinity(0, sizeof(cpuset), &cpuset));

	map_fd = create_map(map_type, 2);
	assert(map_fd != -1);
	expected_map_fd = create_map(BPF_MAP_TYPE_HASH, 2);
	assert(expected_map_fd != -1);

	value[0] = 1234;

	/* insert key=1 element */

	key = 1;
	assert(!bpf_update_elem(map_fd, &key, value, BPF_NOEXIST));
	assert(!bpf_update_elem(expected_map_fd, &key, value, BPF_NOEXIST));

	/* BPF_NOEXIST means: add new element if it doesn't exist */
	assert(bpf_update_elem(map_fd, &key, value, BPF_NOEXIST) == -1 &&
	       /* key=1 already exists */
	       errno == EEXIST);

	assert(bpf_update_elem(map_fd, &key, value, -1) == -1 &&
	       errno == EINVAL);

	/* insert key=2 element */

	/* check that key=2 is not found */
	key = 2;
	assert(bpf_lookup_elem(map_fd, &key, value) == -1 && errno == ENOENT);

	/* BPF_EXIST means: update existing element */
	assert(bpf_update_elem(map_fd, &key, value, BPF_EXIST) == -1 &&
	       /* key=2 is not there */
	       errno == ENOENT);

	assert(!bpf_update_elem(map_fd, &key, value, BPF_NOEXIST));

	/* insert key=3 element */

	/* check that key=3 is not found */
	key = 3;
	assert(bpf_lookup_elem(map_fd, &key, value) == -1 && errno == ENOENT);

	/* check that key=1 can be found and mark the ref bit to
	 * stop LRU from removing key=1
	 */
	key = 1;
	assert(!bpf_lookup_elem(map_fd, &key, value));
	assert(value[0] == 1234);

	key = 3;
	assert(!bpf_update_elem(map_fd, &key, value, BPF_NOEXIST));
	assert(!bpf_update_elem(expected_map_fd, &key, value, BPF_NOEXIST));

	/* key=2 has been removed from the LRU */
	key = 2;
	assert(bpf_lookup_elem(map_fd, &key, value) == -1);

	assert(map_equal(map_fd, expected_map_fd));

	close(map_fd);

	printf("Pass\n");
}

/* Size of the LRU map is 1.5*LOCAL_FREE_TARGET
 * Insert 1 to LOCAL_FREE_TARGET (+LOCAL_FREE_TARGET keys)
 * Lookup 1 to LOCAL_FREE_TARGET/2
 * Insert 1+LOCAL_FREE_TARGET to 2*LOCAL_FREE_TARGET (+LOCAL_FREE_TARGET keys)
 * => 1+LOCAL_FREE_TARGET/2 to LOCALFREE_TARGET will be removed by LRU
 */
static void test_lru_sanity1(int map_type)
{
	unsigned long long key, end_key, value[nr_cpus];
	unsigned int map_size;
	int lru_map_fd, expected_map_fd;
	unsigned int batch_size;
	cpu_set_t cpuset;

	printf("%s (map_type:%d): ", __func__, map_type);

	batch_size = LOCAL_FREE_TARGET / 2;
	assert(batch_size * 2 == LOCAL_FREE_TARGET);

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	assert(sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0);

	map_size = LOCAL_FREE_TARGET + batch_size;
	lru_map_fd = create_map(map_type, map_size);
	assert(lru_map_fd != -1);
	expected_map_fd = create_map(BPF_MAP_TYPE_HASH, map_size);
	assert(expected_map_fd != -1);

	value[0] = 1234;

	/* Insert 1 to LOCAL_FREE_TARGET (+LOCAL_FREE_TARGET keys) */
	end_key = 1 + LOCAL_FREE_TARGET;
	for (key = 1; key < end_key; key++)
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));

	/* Lookup 1 to LOCAL_FREE_TARGET/2 */
	end_key = 1 + batch_size;
	for (key = 1; key < end_key; key++) {
		assert(!bpf_lookup_elem(lru_map_fd, &key, value));
		assert(!bpf_update_elem(expected_map_fd, &key, value,
					BPF_NOEXIST));
	}

	/* Insert 1+LOCAL_FREE_TARGET to 2*LOCAL_FREE_TARGET
	 * => 1+LOCAL_FREE_TARGET/2 to LOCALFREE_TARGET will be
	 * removed by LRU
	 */
	key = 1 + LOCAL_FREE_TARGET;
	end_key = key + LOCAL_FREE_TARGET;
	for (; key < end_key; key++) {
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));
		assert(!bpf_update_elem(expected_map_fd, &key, value,
					BPF_NOEXIST));
	}

	assert(map_equal(lru_map_fd, expected_map_fd));

	close(expected_map_fd);
	close(lru_map_fd);

	printf("Pass\n");
}

/* Size of the LRU map 1.5 * LOCAL_FREE_TARGET
 * Insert 1 to LOCAL_FREE_TARGET (+LOCAL_FREE_TARGET keys)
 * Update 1 to LOCAL_FREE_TARGET/2
 *   => The original 1 to LOCAL_FREE_TARGET/2 will be removed due to
 *      the LRU shrink process
 * Re-insert 1 to LOCAL_FREE_TARGET/2 again and do a lookup immeidately
 * Insert 1+LOCAL_FREE_TARGET to LOCAL_FREE_TARGET*3/2
 * Insert 1+LOCAL_FREE_TARGET*3/2 to LOCAL_FREE_TARGET*5/2
 *   => Key 1+LOCAL_FREE_TARGET to LOCAL_FREE_TARGET*3/2
 *      will be removed from LRU because it has never
 *      been lookup and ref bit is not set
 */
static void test_lru_sanity2(int map_type)
{
	unsigned long long key, value[nr_cpus];
	unsigned long long end_key;
	int lru_map_fd, expected_map_fd;
	unsigned int batch_size;
	unsigned int map_size;
	cpu_set_t cpuset;

	printf("%s (map_type:%d): ", __func__, map_type);

	batch_size = LOCAL_FREE_TARGET / 2;
	assert(batch_size * 2 == LOCAL_FREE_TARGET);

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	assert(sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0);

	map_size = LOCAL_FREE_TARGET + batch_size;
	lru_map_fd = create_map(map_type, map_size);
	assert(lru_map_fd != -1);
	expected_map_fd = create_map(BPF_MAP_TYPE_HASH, map_size);
	assert(expected_map_fd != -1);

	value[0] = 1234;

	/* Insert 1 to LOCAL_FREE_TARGET (+LOCAL_FREE_TARGET keys) */
	end_key = 1 + LOCAL_FREE_TARGET;
	for (key = 1; key < end_key; key++)
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));

	/* Any bpf_update_elem will require to acquire a new node
	 * from LRU first.
	 *
	 * The local list is running out of free nodes.
	 * It gets from the global LRU list which tries to
	 * shrink the inactive list to get LOCAL_FREE_TARGET
	 * number of free nodes.
	 *
	 * Hence, the oldest key 1 to LOCAL_FREE_TARGET/2
	 * are removed from the LRU list.
	 */
	key = 1;
	if (map_type == BPF_MAP_TYPE_LRU_PERCPU_HASH) {
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));
		assert(!bpf_delete_elem(lru_map_fd, &key));
	} else
		assert(bpf_update_elem(lru_map_fd, &key, value, BPF_EXIST));


	/* Re-insert 1 to LOCAL_FREE_TARGET/2 again and do a lookup
	 * immeidately.
	 */
	end_key = 1 + batch_size;
	value[0] = 4321;
	for (key = 1; key < end_key; key++) {
		assert(bpf_lookup_elem(lru_map_fd, &key, value));
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));
		assert(!bpf_lookup_elem(lru_map_fd, &key, value));
		assert(value[0] == 4321);
		assert(!bpf_update_elem(expected_map_fd, &key, value,
					BPF_NOEXIST));
	}

	value[0] = 1234;

	/* Insert 1+LOCAL_FREE_TARGET to LOCAL_FREE_TARGET*3/2 */
	end_key = 1 + LOCAL_FREE_TARGET + batch_size;
	for (key = 1 + LOCAL_FREE_TARGET; key < end_key; key++)
		/* These newly added but not referenced keys will be
		 * gone during the next LRU shrink.
		 */
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));

	/* Insert 1+LOCAL_FREE_TARGET*3/2 to  LOCAL_FREE_TARGET*5/2 */
	end_key = key + LOCAL_FREE_TARGET;
	for (; key < end_key; key++) {
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));
		assert(!bpf_update_elem(expected_map_fd, &key, value,
					BPF_NOEXIST));
	}

	assert(map_equal(lru_map_fd, expected_map_fd));

	close(expected_map_fd);
	close(lru_map_fd);

	printf("Pass\n");
}

/* Size of the LRU map is 2*LOCAL_FREE_TARGET
 * It is to test the active/inactive list rotation
 * Insert 1 to 2*LOCAL_FREE_TARGET (+2*LOCAL_FREE_TARGET keys)
 * Lookup key 1 to LOCAL_FREE_TARGET*3/2
 * Add 1+2*LOCAL_FREE_TARGET to LOCAL_FREE_TARGET*5/2 (+LOCAL_FREE_TARGET/2 keys)
 *  => key 1+LOCAL_FREE_TARGET*3/2 to 2*LOCAL_FREE_TARGET are removed from LRU
 */
static void test_lru_sanity3(int map_type)
{
	unsigned long long key, end_key, value[nr_cpus];
	int lru_map_fd, expected_map_fd;
	unsigned int batch_size;
	unsigned int map_size;
	cpu_set_t cpuset;

	printf("%s (map_type:%d): ", __func__, map_type);

	batch_size = LOCAL_FREE_TARGET / 2;
	assert(batch_size * 2 == LOCAL_FREE_TARGET);

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	assert(sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0);

	map_size = LOCAL_FREE_TARGET * 2;
	lru_map_fd = create_map(map_type, map_size);
	assert(lru_map_fd != -1);
	expected_map_fd = create_map(BPF_MAP_TYPE_HASH, map_size);
	assert(expected_map_fd != -1);

	value[0] = 1234;

	/* Insert 1 to 2*LOCAL_FREE_TARGET (+2*LOCAL_FREE_TARGET keys) */
	end_key = 1 + (2 * LOCAL_FREE_TARGET);
	for (key = 1; key < end_key; key++)
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));

	/* Lookup key 1 to LOCAL_FREE_TARGET*3/2 */
	end_key = LOCAL_FREE_TARGET + batch_size;
	for (key = 1; key < end_key; key++) {
		assert(!bpf_lookup_elem(lru_map_fd, &key, value));
		assert(!bpf_update_elem(expected_map_fd, &key, value,
					BPF_NOEXIST));
	}

	/* Add 1+2*LOCAL_FREE_TARGET to LOCAL_FREE_TARGET*5/2
	 * (+LOCAL_FREE_TARGET/2 keys)
	 */
	key = 2 * LOCAL_FREE_TARGET + 1;
	end_key = key + batch_size;
	for (; key < end_key; key++) {
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));
		assert(!bpf_update_elem(expected_map_fd, &key, value,
					BPF_NOEXIST));
	}

	assert(map_equal(lru_map_fd, expected_map_fd));

	close(expected_map_fd);
	close(lru_map_fd);

	printf("Pass\n");
}

/* Test deletion */
static void test_lru_sanity4(int map_type)
{
	int lru_map_fd, expected_map_fd;
	unsigned long long key, value[nr_cpus];
	unsigned long long end_key;
	cpu_set_t cpuset;

	printf("%s (map_type:%d): ", __func__, map_type);

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	assert(sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0);

	lru_map_fd = create_map(map_type, 3 * LOCAL_FREE_TARGET);
	assert(lru_map_fd != -1);
	expected_map_fd = create_map(BPF_MAP_TYPE_HASH,
				     3 * LOCAL_FREE_TARGET);

	value[0] = 1234;

	for (key = 1; key <= 2 * LOCAL_FREE_TARGET; key++)
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));

	key = 1;
	assert(bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));

	for (key = 1; key <= LOCAL_FREE_TARGET; key++) {
		assert(!bpf_lookup_elem(lru_map_fd, &key, value));
		assert(!bpf_update_elem(expected_map_fd, &key, value,
					BPF_NOEXIST));
	}

	for (; key <= 2 * LOCAL_FREE_TARGET; key++) {
		assert(!bpf_delete_elem(lru_map_fd, &key));
		assert(bpf_delete_elem(lru_map_fd, &key));
	}

	end_key = key + 2 * LOCAL_FREE_TARGET;
	for (; key < end_key; key++) {
		assert(!bpf_update_elem(lru_map_fd, &key, value, BPF_NOEXIST));
		assert(!bpf_update_elem(expected_map_fd, &key, value,
					BPF_NOEXIST));
	}

	assert(map_equal(lru_map_fd, expected_map_fd));

	close(expected_map_fd);
	close(lru_map_fd);

	printf("Pass\n");
}

static void do_test_lru_small0(int cpu, int map_fd)
{
	unsigned long long key, value[nr_cpus];

	/* Ensure the last key inserted by previous CPU can be found */
	key = cpu;
	assert(!bpf_lookup_elem(map_fd, &key, value));

	value[0] = 1234;

	key = cpu + 1;
	assert(!bpf_update_elem(map_fd, &key, value, BPF_NOEXIST));
	assert(!bpf_lookup_elem(map_fd, &key, value));

	/* Cannot find the last key because it was removed by LRU */
	key = cpu;
	assert(bpf_lookup_elem(map_fd, &key, value));
}

static void test_lru_small0(int map_type)
{
	unsigned long long key, value[nr_cpus];
	int map_fd;
	int i;

	printf("%s (map_type%d): ", __func__, map_type);

	map_fd = create_map(map_type, 1);
	assert(map_fd != -1);

	value[0] = 1234;
	key = 0;
	assert(!bpf_update_elem(map_fd, &key, value, BPF_NOEXIST));

	for (i = 0; i < nr_cpus; i++) {
		cpu_set_t cpuset;
		pid_t pid;

		pid = fork();
		if (pid == 0) {
			CPU_ZERO(&cpuset);
			CPU_SET(i, &cpuset);
			assert(!sched_setaffinity(0, sizeof(cpuset), &cpuset));
			do_test_lru_small0(i, map_fd);
			exit(0);
		} else if (pid == -1) {
			printf("couldn't spawn #%d process\n", i);
			exit(1);
		} else {
			int status;

			assert(waitpid(pid, &status, 0) == pid);
			assert(status == 0);
		}
	}

	close(map_fd);

	printf("Pass\n");
}

static void test_lru_loss0(int map_type)
{
	unsigned long long key, value[nr_cpus];
	unsigned int old_unused_losses = 0;
	unsigned int new_unused_losses = 0;
	unsigned int used_losses = 0;
	int map_fd;

	printf("%s (map_type:%d): ", __func__, map_type);

	map_fd = create_map(map_type, 900);
	assert(map_fd != -1);

	value[0] = 1234;

	for (key = 1; key <= 1000; key++) {
		int start_key, end_key;

		assert(bpf_update_elem(map_fd, &key, value, BPF_NOEXIST) == 0);

		start_key = 101;
		end_key = min(key, 900);

		while (start_key <= end_key) {
			bpf_lookup_elem(map_fd, &start_key, value);
			start_key++;
		}
	}

	for (key = 1; key <= 1000; key++) {
		if (bpf_lookup_elem(map_fd, &key, value)) {
			if (key <= 100)
				old_unused_losses++;
			else if (key <= 900)
				used_losses++;
			else
				new_unused_losses++;
		}
	}

	close(map_fd);

	printf("older-elem-losses:%d(/100) active-elem-losses:%d(/800) "
	       "newer-elem-losses:%d(/100)\n",
	       old_unused_losses, used_losses, new_unused_losses);
}

static void test_lru_loss1(int map_type)
{
	unsigned long long key, value[nr_cpus];
	int map_fd;
	unsigned int nr_losses = 0;

	printf("%s (map_type:%d): ", __func__, map_type);

	map_fd = create_map(map_type, 1000);
	assert(map_fd != -1);

	value[0] = 1234;

	for (key = 1; key <= 1000; key++)
		assert(!bpf_update_elem(map_fd, &key, value, BPF_NOEXIST));

	for (key = 1; key <= 1000; key++) {
		if (bpf_lookup_elem(map_fd, &key, value))
			nr_losses++;
	}

	close(map_fd);

	printf("nr_losses:%d(/1000)\n", nr_losses);
}

static void do_test_lru_parallel_loss(int task, void *data)
{
	const unsigned int nr_stable_elems = 1000;
	const unsigned int nr_repeats = 100000;

	int map_fd = *(int *)data;
	unsigned long long stable_base;
	unsigned long long key, value[nr_cpus];
	unsigned long long next_ins_key;
	unsigned int nr_losses = 0;
	unsigned int i;

	stable_base = task * nr_repeats * 2 + 1;
	next_ins_key = stable_base;
	value[0] = 1234;
	for (i = 0; i < nr_stable_elems; i++) {
		assert(bpf_update_elem(map_fd, &next_ins_key, value,
				       BPF_NOEXIST) == 0);
		next_ins_key++;
	}

	for (i = 0; i < nr_repeats; i++) {
		int rn;

		rn = rand();

		if (rn % 10) {
			key = rn % nr_stable_elems + stable_base;
			bpf_lookup_elem(map_fd, &key, value);
		} else {
			bpf_update_elem(map_fd, &next_ins_key, value,
					BPF_NOEXIST);
			next_ins_key++;
		}
	}

	key = stable_base;
	for (i = 0; i < nr_stable_elems; i++) {
		if (bpf_lookup_elem(map_fd, &key, value))
			nr_losses++;
		key++;
	}

	printf("    task:%d nr_losses:%u\n", task, nr_losses);
}

static void run_parallel(int tasks, void (*fn)(int i, void *data), void *data)
{
	cpu_set_t cpuset;
	pid_t pid[tasks];
	int i;

	for (i = 0; i < tasks; i++) {
		pid[i] = fork();
		if (pid[i] == 0) {
			CPU_ZERO(&cpuset);
			CPU_SET(i, &cpuset);
			assert(!sched_setaffinity(0, sizeof(cpuset), &cpuset));
			fn(i, data);
			exit(0);
		} else if (pid[i] == -1) {
			printf("couldn't spawn #%d process\n", i);
			exit(1);
		}
	}
	for (i = 0; i < tasks; i++) {
		int status;

		assert(waitpid(pid[i], &status, 0) == pid[i]);
		assert(status == 0);
	}
}

static void test_lru_parallel_loss(int map_type, int nr_tasks)
{
	int map_fd;

	printf("%s (map_type:%d):\n", __func__, map_type);

	/* Give 20% more than the active working set */
	map_fd = create_map(map_type, nr_tasks * (1000 + 200));

	assert(map_fd != -1);

	run_parallel(nr_tasks, do_test_lru_parallel_loss, &map_fd);

	close(map_fd);
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	int map_types[] = {BPF_MAP_TYPE_LRU_HASH,
			   BPF_MAP_TYPE_LRU_PERCPU_HASH};
	int i;

	setbuf(stdout, NULL);

	assert(!setrlimit(RLIMIT_MEMLOCK, &r));

	srand(time(NULL));

	nr_cpus = sysconf(_SC_NPROCESSORS_CONF);
	assert(nr_cpus != -1);
	printf("nr_cpus:%ld\n\n", nr_cpus);

	for (i = 0; i < sizeof(map_types) / sizeof(*map_types); i++) {
		test_lru_sanity0(map_types[i]);
		test_lru_sanity1(map_types[i]);
		test_lru_sanity2(map_types[i]);
		test_lru_sanity3(map_types[i]);
		test_lru_sanity4(map_types[i]);

		test_lru_small0(map_types[i]);

		test_lru_loss0(map_types[i]);
		test_lru_loss1(map_types[i]);
		test_lru_parallel_loss(map_types[i], nr_cpus);

		printf("\n");
	}


	return 0;
}
