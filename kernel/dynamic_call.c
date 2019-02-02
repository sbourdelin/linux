// SPDX-License-Identifier: GPL-2.0

#include <linux/dynamic_call.h>
#include <linux/printk.h>

static void dynamic_call_add_cand(struct dynamic_call_candidate *top,
				 size_t ncands,
				 struct dynamic_call_candidate next)
{
	struct dynamic_call_candidate old;
	int i;

	for (i = 0; i < ncands; i++) {
		if (next.hit_count > top[i].hit_count) {
			/* Swap next with top[i], so that the old top[i] can
			 * shunt along all lower scores
			 */
			old = top[i];
			top[i] = next;
			next = old;
		}
	}
}

static void dynamic_call_count_hits(struct dynamic_call_candidate *top,
				   size_t ncands, struct dynamic_call *dc,
				   int i)
{
	struct dynamic_call_candidate next;
	struct dynamic_call_percpu *percpu;
	int cpu;

	next.func = dc->key[i]->func;
	next.hit_count = 0;
	for_each_online_cpu(cpu) {
		percpu = per_cpu_ptr(dc->percpu, cpu);
		next.hit_count += percpu->hit_count[i];
		percpu->hit_count[i] = 0;
	}

	dynamic_call_add_cand(top, ncands, next);
}

void dynamic_call_update(struct work_struct *work)
{
	struct dynamic_call *dc = container_of(work, struct dynamic_call,
					       update_work);
	struct dynamic_call_candidate top[4], next, *cands, *cands2;
	struct dynamic_call_percpu *percpu, *percpu2;
	int cpu, i, cpu2, j;

	memset(top, 0, sizeof(top));

	pr_debug("dynamic_call_update called for %ps\n", dc);
	mutex_lock(&dc->update_lock);
	/* We don't stop the other CPUs adding to their counts while this is
	 * going on; but it doesn't really matter because this is a heuristic
	 * anyway so we don't care about perfect accuracy.
	 */
	/* First count up the hits on the existing static branches */
	for (i = 0; i < DYNAMIC_CALL_BRANCHES; i++)
		dynamic_call_count_hits(top, ARRAY_SIZE(top), dc, i);
	/* Next count up the callees seen in the fallback path */
	/* Switch off stats collection in the slowpath first */
	static_branch_enable(dc->skip_stats);
	synchronize_rcu();
	for_each_online_cpu(cpu) {
		percpu = per_cpu_ptr(dc->percpu, cpu);
		cands = percpu->candidates;
		for (i = 0; i < DYNAMIC_CALL_CANDIDATES; i++) {
			next = cands[i];
			if (next.func == NULL)
				continue;
			next.hit_count = 0;
			for_each_online_cpu(cpu2) {
				percpu2 = per_cpu_ptr(dc->percpu, cpu2);
				cands2 = percpu2->candidates;
				for (j = 0; j < DYNAMIC_CALL_CANDIDATES; j++) {
					if (cands2[j].func == next.func) {
						cands2[j].func = NULL;
						next.hit_count += cands2[j].hit_count;
						cands2[j].hit_count = 0;
						break;
					}
				}
			}
			dynamic_call_add_cand(top, ARRAY_SIZE(top), next);
		}
	}
	/* Record our results (for debugging) */
	for (i = 0; i < ARRAY_SIZE(top); i++) {
		if (i < DYNAMIC_CALL_BRANCHES)
			pr_debug("%ps: selected [%d] %pf, score %lu\n",
				 dc, i, top[i].func, top[i].hit_count);
		else
			pr_debug("%ps: runnerup [%d] %pf, score %lu\n",
				 dc, i, top[i].func, top[i].hit_count);
	}
	/* It's possible that we could have picked up multiple pushes of the
	 * workitem, so someone already collected most of the count.  In that
	 * case, don't make a decision based on only a small number of calls.
	 */
	if (top[0].hit_count > 250) {
		/* Divert callers away from the fast path */
		static_branch_enable(dc->skip_fast);
		/* Wait for existing fast path callers to finish */
		synchronize_rcu();
		/* Patch the chosen callees into the fast path */
		for(i = 0; i < DYNAMIC_CALL_BRANCHES; i++) {
			__static_call_update(dc->key[i], top[i].func);
			/* Clear the hit-counts, they were for the old funcs */
			for_each_online_cpu(cpu)
				per_cpu_ptr(dc->percpu, cpu)->hit_count[i] = 0;
		}
		/* Ensure the new fast path is seen before we direct anyone
		 * into it.  This probably isn't necessary (the binary-patching
		 * framework probably takes care of it) but let's be paranoid.
		 */
		wmb();
		/* Switch callers back onto the fast path */
		static_branch_disable(dc->skip_fast);
	} else {
		pr_debug("%ps: too few hits, not patching\n", dc);
	}

	/* Finally, re-enable stats gathering in the fallback path. */
	static_branch_disable(dc->skip_stats);

	mutex_unlock(&dc->update_lock);
	pr_debug("dynamic_call_update (%ps) finished\n", dc);
}
