#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rseq.h"

#if defined(__x86_64__)

#define barrier() {__asm__ __volatile__("" : : : "memory"); }

struct rseq_section {
	void *begin;
	void *end;
	void *restart;
};

extern struct rseq_section const __start___rseq_sections[]
__attribute((weak));
extern struct rseq_section const __stop___rseq_sections[]
__attribute((weak));

/* Implemented by percpu_ops.S */
struct percpu_lock {
	int word[CPU_SETSIZE][16];  /* cache aligned; lock-word is [cpu][0] */
};

/* A simple percpu spinlock.  Returns the cpu lock was acquired on. */
int rseq_percpu_lock(struct percpu_lock *lock)
{
	int out = -1;

	asm volatile (
		"1:\n\t"
		"movl %1, %0\n\t"
		"leaq (,%0,8), %%r10\n\t"
		"leaq (%2, %%r10, 8), %%r10\n\t"
		"2:\n\t"
		"cmpl $0, (%%r10)\n\t"
		"jne 2b\n\t"
		"movl $1, (%%r10)\n\t"
		"3:\n\t"
		".pushsection __rseq_sections, \"a\"\n\t"
		".quad 1b, 3b, 1b\n\t"
		".popsection\n\t"
		: "+r" (out)
		: "m" (__rseq_current_cpu), "r" ((unsigned long)lock)
		: "memory", "r10");
	return out;
}

/*
 * cmpxchg [with an additional check value].
 *
 * Returns:
 *  -1 if *p != old or cpu != current cpu [ || check_ptr != check_val, ]
 * otherwise 0.
 *
 * Note: When specified, check_ptr is dereferenced iff *p == old
 */
int rseq_percpu_cmpxchg(int cpu, intptr_t *p, intptr_t old, intptr_t new)
{
	asm volatile goto (
		"1:\n\t"
		"cmpl %1, %0\n\t"
		"jne %l[fail]\n\t"
		"cmpq %2, %3\n\t"
		"jne %l[fail]\n\t"
		"movq %4, %3\n\t"
		"2:\n\t"
		".pushsection __rseq_sections, \"a\"\n\t"
		".quad 1b, 2b, 1b\n\t"
		".popsection\n\t"
		:
		: "r" (cpu), "m" (__rseq_current_cpu),
		  "r" (old), "m" (*p), "r" (new)
		: "memory"
		: fail);
	return 0;
fail:
	return -1;
}
int rseq_percpu_cmpxchgcheck(int cpu, intptr_t *p, intptr_t old, intptr_t new,
			intptr_t *check_ptr, intptr_t check_val)
{
	asm volatile goto (
		"1:\n\t"
		"cmpl %1, %0\n\t"
		"jne %l[fail]\n\t"
		"cmpq %2, %3\n\t"
		"jne %l[fail]\n\t"
		"cmpq %5, %6\n\t"
		"jne %l[fail]\n\t"
		"movq %4, %3\n\t"
		"2:\n\t"
		".pushsection __rseq_sections, \"a\"\n\t"
		".quad 1b, 2b, 1b\n\t"
		".popsection\n\t"
		:
		: "r" (cpu), "m" (__rseq_current_cpu),
		  "r" (old), "m" (*p), "r" (new),
		  "r" (check_val), "m" (*check_ptr)
		: "memory"
		: fail);
	return 0;
fail:
	return -1;
}


void rseq_percpu_unlock(struct percpu_lock *lock, int cpu)
{
	barrier();  /* need a release-store here, this suffices on x86. */
	assert(lock->word[cpu][0] == 1);
	lock->word[cpu][0] = 0;
}

void rseq_unknown_restart_addr(void *addr)
{
	fprintf(stderr, "rseq: unrecognized restart address %p\n", addr);
	exit(1);
}

struct spinlock_test_data {
	struct percpu_lock lock;
	int counts[CPU_SETSIZE];
	int reps;
};

void *test_percpu_spinlock_thread(void *arg)
{
	struct spinlock_test_data *data = arg;
	int i, cpu;

	rseq_configure_cpu_pointer();
	for (i = 0; i < data->reps; i++) {
		cpu = rseq_percpu_lock(&data->lock);
		data->counts[cpu]++;
		rseq_percpu_unlock(&data->lock, cpu);
	}

	return 0;
}

/*
 * A simple test which implements a sharded counter using a per-cpu lock.
 * Obviously real applications might prefer to simply use a per-cpu increment;
 * however, this is reasonable for a test and the lock can be extended to
 * synchronize more complicated operations.
 */
void test_percpu_spinlock(void)
{
	int i, sum;
	pthread_t test_threads[200];
	struct spinlock_test_data data;

	memset(&data, 0, sizeof(data));
	data.reps = 5000;

	for (i = 0; i < 200; i++)
		pthread_create(&test_threads[i], NULL,
			       test_percpu_spinlock_thread, &data);

	for (i = 0; i < 200; i++)
		pthread_join(test_threads[i], NULL);

	sum = 0;
	for (i = 0; i < CPU_SETSIZE; i++)
		sum += data.counts[i];

	assert(sum == data.reps * 200);
}

struct percpu_list_node {
	intptr_t data;
	struct percpu_list_node *next;
};

struct percpu_list {
	struct percpu_list_node *heads[CPU_SETSIZE];
};

int percpu_list_push(struct percpu_list *list, struct percpu_list_node *node)
{
	int cpu;

	do {
		cpu = rseq_current_cpu();
		node->next = list->heads[cpu];
	} while (0 != rseq_percpu_cmpxchg(
			cpu,
			(intptr_t *)&list->heads[cpu], (intptr_t)node->next,
			(intptr_t)node));

	return cpu;
}

struct percpu_list_node *percpu_list_pop(struct percpu_list *list)
{
	int cpu;
	struct percpu_list_node *head, *next;

	do {
		cpu = rseq_current_cpu();
		head = list->heads[cpu];
		/*
		 * Unlike a traditional lock-less linked list; the availability
		 * of a cmpxchg-check primitive allows us to implement pop
		 * without concerns over ABA-type races.
		 */
		if (!head)
			return 0;
		next = head->next;
	} while (0 != rseq_percpu_cmpxchgcheck(cpu,
		(intptr_t *)&list->heads[cpu], (intptr_t)head, (intptr_t)next,
		(intptr_t *)&head->next, (intptr_t)next));

	return head;
}


void *test_percpu_list_thread(void *arg)
{
	int i;
	struct percpu_list *list = (struct percpu_list *)arg;

	rseq_configure_cpu_pointer();
	for (i = 0; i < 100000; i++) {
		struct percpu_list_node *node = percpu_list_pop(list);

		sched_yield();  /* encourage shuffling */
		if (node)
			percpu_list_push(list, node);
	}

	return 0;
}

/*
 * Implements a per-cpu linked list then shuffles it via popping and pushing
 * from many threads.
 */
void test_percpu_list(void)
{
	int i, j;
	long sum = 0, expected_sum = 0;
	struct percpu_list list;
	pthread_t test_threads[200];
	cpu_set_t allowed_cpus;

	memset(&list, 0, sizeof(list));

	/* Generate list entries for every usable cpu. */
	sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus);
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (!CPU_ISSET(i, &allowed_cpus))
			continue;
		for (j = 1; j <= 100; j++) {
			struct percpu_list_node *node;

			expected_sum += j;

			node = malloc(sizeof(*node));
			assert(node);
			node->data = j;
			node->next = list.heads[i];
			list.heads[i] = node;
		}
	}

	for (i = 0; i < 200; i++)
		assert(pthread_create(&test_threads[i], NULL,
			       test_percpu_list_thread, &list) == 0);

	for (i = 0; i < 200; i++)
		pthread_join(test_threads[i], NULL);

	for (i = 0; i < CPU_SETSIZE; i++) {
		cpu_set_t pin_mask;
		struct percpu_list_node *node;

		if (!CPU_ISSET(i, &allowed_cpus))
			continue;

		CPU_ZERO(&pin_mask);
		CPU_SET(i, &pin_mask);
		sched_setaffinity(0, sizeof(pin_mask), &pin_mask);

		while ((node = percpu_list_pop(&list))) {
			sum += node->data;
			free(node);
		}
	}

	/*
	 * All entries should now be accounted for (unless some external actor
	 * is interfering with our allowed affinity while this test is
	 * running).
	 */
	assert(sum == expected_sum);
}

int main(int argc, char **argv)
{
	const struct rseq_section *iter;

	for (iter = __start___rseq_sections;
	     iter < __stop___rseq_sections;
	     iter++) {
		rseq_configure_region(iter->begin, iter->end, iter->restart);
		printf("Installing region %p, %p\n", iter->begin, iter->end);
	}
	rseq_configure_cpu_pointer();

	test_percpu_spinlock();
	test_percpu_list();

	return 0;
}

#else
int main(int argc, char **argv)
{
	fprintf(stderr, "architecture not supported\n");
	return 0;
}
#endif
