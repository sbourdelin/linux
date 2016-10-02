/*
 * Copyright (c) 2016 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#define _GNU_SOURCE
#include <linux/types.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/bpf.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include "libbpf.h"

#define offsetof(TYPE, MEMBER)	((size_t)&((TYPE *)0)->MEMBER)

#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

struct list_head {
	struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void __list_del_entry(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

static inline void list_move(struct list_head *list, struct list_head *head)
{
	__list_del_entry(list);
	list_add(list, head);
}

#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)


#define list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)

struct pfect_lru_node {
	struct list_head list;
	unsigned long long key;
};

struct pfect_lru {
	struct list_head list;
	struct pfect_lru_node *free_nodes;
	unsigned int cur_size;
	unsigned int lru_size;
	unsigned int nr_unique;
	unsigned int nr_misses;
	unsigned int total;
	int map_fd;
};

static void pfect_lru_init(struct pfect_lru *lru, unsigned int lru_size,
			   unsigned int nr_possible_elems)
{
	lru->map_fd = bpf_create_map(BPF_MAP_TYPE_HASH,
				     sizeof(unsigned long long),
				     sizeof(struct pfect_lru_node *),
				     nr_possible_elems, 0);
	assert(lru->map_fd != -1);

	lru->free_nodes = malloc(lru_size * sizeof(struct pfect_lru_node));
	assert(lru->free_nodes);

	INIT_LIST_HEAD(&lru->list);
	lru->cur_size = 0;
	lru->lru_size = lru_size;
	lru->nr_unique = lru->nr_misses = lru->total = 0;
}

static void pfect_lru_destroy(struct pfect_lru *lru)
{
	close(lru->map_fd);
	free(lru->free_nodes);
}

static int pfect_lru_lookup_or_insert(struct pfect_lru *lru,
				      unsigned long long key)
{
	struct pfect_lru_node *node = NULL;
	int seen = 0;

	lru->total++;
	if (!bpf_lookup_elem(lru->map_fd, &key, &node)) {
		if (node) {
			list_move(&node->list, &lru->list);
			return 1;
		}
		seen = 1;
	}

	if (lru->cur_size < lru->lru_size) {
		node =  &lru->free_nodes[lru->cur_size++];
		INIT_LIST_HEAD(&node->list);
	} else {
		struct pfect_lru_node *null_node = NULL;

		node = list_last_entry(&lru->list,
				       struct pfect_lru_node,
				       list);
		bpf_update_elem(lru->map_fd, &node->key, &null_node, BPF_EXIST);
	}

	node->key = key;
	list_move(&node->list, &lru->list);

	lru->nr_misses++;
	if (seen) {
		assert(!bpf_update_elem(lru->map_fd, &key, &node, BPF_EXIST));
	} else {
		lru->nr_unique++;
		assert(!bpf_update_elem(lru->map_fd, &key, &node, BPF_NOEXIST));
	}

	return seen;
}

static unsigned int read_keys(const char *dist_file,
			      unsigned long long **keys)
{
	struct stat fst;
	unsigned long long *retkeys;
	unsigned int counts = 0;
	int dist_fd;
	char *b, *l;
	int i;

	dist_fd = open(dist_file, 0);
	assert(dist_fd != -1);

	assert(fstat(dist_fd, &fst) == 0);
	b = malloc(fst.st_size);
	assert(b);

	assert(read(dist_fd, b, fst.st_size) == fst.st_size);
	close(dist_fd);
	for (i = 0; i < fst.st_size; i++) {
		if (b[i] == '\n')
			counts++;
	}
	counts++; /* in case the last line has no \n */

	retkeys = malloc(counts * sizeof(unsigned long long));
	assert(retkeys);

	counts = 0;
	for (l = strtok(b, "\n"); l; l = strtok(NULL, "\n"))
		retkeys[counts++] = strtoull(l, NULL, 10);
	free(b);

	*keys = retkeys;

	return counts;
}

static void do_test_lru_dist(int lru_map_fd, int task,
			     const unsigned long long *keys,
			     unsigned int key_counts, unsigned int lru_size)
{
	unsigned int nr_misses = 0;
	struct pfect_lru pfect_lru;
	unsigned long long key_offset = task * key_counts;
	unsigned long long key, value = 1234;
	unsigned int i;

	printf("task:%d %s:......\n", task, __func__);

	pfect_lru_init(&pfect_lru, lru_size, key_counts);

	for (i = 0; i < key_counts; i++) {
		key = keys[i] + key_offset;

		pfect_lru_lookup_or_insert(&pfect_lru, key);

		if (!bpf_lookup_elem(lru_map_fd, &key, &value))
			continue;

		if (bpf_update_elem(lru_map_fd, &key, &value, BPF_NOEXIST)) {
			printf("bpf_update_elem(lru_map_fd, %llu): errno:%d\n",
			       key, errno);
			assert(0);
		}

		nr_misses++;
	}

	printf("    task:%d BPF LRU: nr_unique:%u(/%u) nr_misses:%u(/%u)\n",
	       task, pfect_lru.nr_unique, key_counts, nr_misses, key_counts);
	printf("    task:%d Perfect LRU: nr_unique:%u(/%u nr_misses:%u(/%u)\n",
	       task, pfect_lru.nr_unique, pfect_lru.total,
	       pfect_lru.nr_misses, pfect_lru.total);

	pfect_lru_destroy(&pfect_lru);
}

static void test_lru_dist(int map_type, const unsigned long long *keys,
			  unsigned int key_counts, unsigned int lru_size)
{
	cpu_set_t cpuset;
	int lru_map_fd;

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	assert(!sched_setaffinity(0, sizeof(cpuset), &cpuset));

	lru_map_fd = bpf_create_map(map_type, sizeof(unsigned long long),
				    sizeof(unsigned long long),
				    lru_size, 0);
	assert(lru_map_fd != -1);
	do_test_lru_dist(lru_map_fd, 0, keys, key_counts, lru_size);
	close(lru_map_fd);
}

static void test_parallel_lru_dist(int map_type, int nr_tasks,
				   const unsigned long long *keys,
				   unsigned int key_counts,
				   unsigned int lru_size)
{
	cpu_set_t cpuset;
	pid_t pid[nr_tasks];
	int lru_map_fd;
	int i;


	lru_map_fd = bpf_create_map(map_type, sizeof(unsigned long long),
				    sizeof(unsigned long long),
				    nr_tasks * lru_size, 0);
	assert(lru_map_fd != -1);

	for (i = 0; i < nr_tasks; i++) {
		pid[i] = fork();
		if (pid[i] == 0) {
			CPU_ZERO(&cpuset);
			CPU_SET(i, &cpuset);
			assert(!sched_setaffinity(0, sizeof(cpuset), &cpuset));
			do_test_lru_dist(lru_map_fd, i, keys, key_counts,
					 lru_size);
			exit(0);
		} else if (pid[i] == -1) {
			printf("couldn't spawn #%d process\n", i);
			exit(1);
		}
	}
	for (i = 0; i < nr_tasks; i++) {
		int status;

		assert(waitpid(pid[i], &status, 0) == pid[i]);
		assert(status == 0);
	}

	close(lru_map_fd);
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	const char *dist_file = argv[1];
	int lru_size = atoi(argv[2]);
	unsigned long long *keys = NULL;
	unsigned int counts;

	setbuf(stdout, NULL);

	assert(!setrlimit(RLIMIT_MEMLOCK, &r));

	counts = read_keys(dist_file, &keys);
	test_lru_dist(BPF_MAP_TYPE_LRU_HASH, keys, counts, lru_size);
	if (argc > 3)
		test_parallel_lru_dist(BPF_MAP_TYPE_LRU_HASH, atoi(argv[3]),
				       keys, counts, lru_size);

	free(keys);

	return 0;
}
