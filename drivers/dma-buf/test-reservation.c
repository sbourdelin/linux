/*
 * Test cases for struct reservation_object
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dma-fence.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/reservation.h>

#define SHIFT (ilog2(NSHARED))
#define NFENCES 4096

static const char *fake_get_driver_name(struct dma_fence *fence)
{
	return "test-reservation";
}

static const char *fake_get_timeline_name(struct dma_fence *fence)
{
	return "test";
}

static bool fake_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static void fake_release(struct dma_fence *fence)
{
	WARN(1, "invalid fence unref\n");
}

const struct dma_fence_ops fake_fence_ops = {
	.get_driver_name = fake_get_driver_name,
	.get_timeline_name = fake_get_timeline_name,
	.enable_signaling = fake_enable_signaling,
	.wait = dma_fence_default_wait,
	.release = fake_release,
};

static int validate_layer(struct reservation_shared_layer *parent,
			  struct reservation_shared_layer *p,
			  int idx)
{
	int ret;
	int n;

	if (parent && p->height >= parent->height) {
		pr_err("child layer (prefix=%llx) has greater height [%d] than parent [%d] (prefix=%llx)\n",
		       p->prefix, p->height, parent->height, parent->prefix);
		return -EINVAL;
	}

	if (parent && (p->prefix >> parent->height) != (parent->prefix >> p->height)) {
		pr_err("child layer (prefix=%llx, height=%d) does not fit in parent (prefix %llx, height %d)\n",
		       p->prefix, p->height,
		       parent->prefix, parent->height);
		return -EINVAL;
	}

	if (parent && ((p->prefix >> (parent->height - p->height - SHIFT)) & (NSHARED - 1)) != idx) {
		pr_err("child layer (prefix=%llx, height=%d) in position %d does not match expected %d of parent (prefix=%llx, height=%d)\n",


		       p->prefix, p->height,
		       idx, (int)((p->prefix >> (parent->height - p->height - SHIFT) & (NSHARED - 1)) != idx),
		       parent->prefix, parent->height);
		return -EINVAL;
	}

	for (n = 0; n < NSHARED; n++) {
		bool has_bit = p->bitmap & BIT(n);
		bool has_child = p->slot[n];

		if (has_bit ^ has_child) {
			pr_err("layer (prefix=%llx, height=%d) inconsistent bitmap position %d, has_child=%d, has_bit=%dn",
			       p->prefix, p->height, n, has_bit, has_child);
			return -EINVAL;
		}

		if (!p->slot[n] || !p->height)
			continue;

		ret = validate_layer(p, p->slot[n], n);
		if (ret)
			return ret;
	}

	return 0;
}

static int validate_tree(struct reservation_object *resv)
{
	if (!resv->shared.top)
		return 0;

	return validate_layer(NULL, resv->shared.top, 0);
}

enum direction {
	forward,
	backward,
	random,
};

static const char *direction_string(enum direction dir)
{
	switch (dir) {
	case forward: return "forward";
	case backward: return "backward";
	case random: return "random";
	default: return "unknown!";
	}
}

static int test_fences(u64 stride, enum direction dir)
{
	DEFINE_SPINLOCK(lock);
	struct dma_fence *fences;
	struct reservation_object resv;
	struct reservation_shared_iter iter;
	struct dma_fence **shared, *excl;
	unsigned int nshared, n;
	int *order;
	u64 context;
	int ret = -EINVAL;

	fences = kmalloc_array(NFENCES, sizeof(*fences), GFP_KERNEL);
	if (!fences)
		return -ENOMEM;

	order = kmalloc_array(NFENCES, sizeof(*order), GFP_KERNEL);
	if (!order) {
		kfree(fences);
		return -ENOMEM;
	}

	pr_info("Testing %d fences with context stride %llu, %s\n",
		NFENCES, stride, direction_string(dir));

	reservation_object_init(&resv);

	context = 1;
	for (n = 0; n < NFENCES; n++) {
		dma_fence_init(&fences[n], &fake_fence_ops, &lock,
			       context, n);
		order[n] = dir == backward ? NFENCES - n - 1 : n;
		context += stride;
	}

	if (dir == random) {
		for (n = NFENCES-1; n > 1; n--) {
			int r = get_random_int() % (n + 1);
			if (r != n) {
				int tmp = order[n];
				order[n] = order[r];
				order[r] = tmp;
			}
		}
	}

	ww_mutex_lock(&resv.lock, NULL);
	for (n = 0; n < NFENCES; n++) {
		if (reservation_object_reserve_shared(&resv) == 0)
			reservation_object_add_shared_fence(&resv,
							    &fences[order[n]]);
	}
	ww_mutex_unlock(&resv.lock);
	kfree(order);

	if (validate_tree(&resv)) {
		pr_err("reservation object has an invalid tree!\n");
		goto out;
	}

	if (!reservation_object_has_shared(&resv)) {
		pr_err("reservation object has no shared fences!\n");
		goto out;
	}

	n = 0;
	reservation_object_for_each_shared(&resv, iter) {
		if (iter.fence != &fences[n]) {
			pr_err("fence[%d] iter out of order: found %llx [%d], expected %llx\n",
			       n,
			       iter.fence->context,
			       iter.fence->seqno,
			       fences[n].context);
			goto out;
		}
		n++;
	}
	if (n != NFENCES) {
		pr_err("iterated over %d shared fences, expected %d\n", nshared, NFENCES);
		goto out;
	}

	if (reservation_object_get_fences_rcu(&resv, &excl,
					      &nshared, &shared)) {
		pr_err("reservation_object_get_fences_rcu failed\n");
		goto out;
	}

	if (excl) {
		pr_err("reservation_object_get_fences_rcu reported an exclusive fence\n");
		goto out;
	}

	if (nshared != NFENCES) {
		pr_err("reservation_object_get_fences_rcu reported %d shared fences, expected %d\n", nshared, NFENCES);
		goto out;
	}

	for (n = 0; n < NFENCES; n++) {
		if (shared[n] != &fences[n]) {
			pr_err("fence[%d] iter out of order: found %llx [%d], expected %llx\n",
			       n,
			       shared[n]->context,
			       shared[n]->seqno,
			       fences[n].context);
			goto out;
		}
		dma_fence_put(shared[n]);
	}
	kfree(shared);

	if (!reservation_object_test_signaled_rcu(&resv, false)) {
		pr_err("reservation object not signaled [exclusive]\n");
		goto out;
	}

	if (reservation_object_test_signaled_rcu(&resv, true)) {
		pr_err("reservation object was signaled [all]\n");
		goto out;
	}

	//reservation_object_wait_timeout_rcu(&resv, true, false, 0);

	reservation_object_add_excl_fence(&resv, NULL);

	for (n = 0; n < NFENCES; n++) {
		if (atomic_read(&fences[n].refcount.refcount) > 1) {
			pr_err("fence[%d] leaked, refcount now %d\n",
			       n, atomic_read(&fences[n].refcount.refcount));
			goto out;
		}
	}

	if (reservation_object_has_shared(&resv)) {
		pr_err("reservation object did not discard shared fences!\n");
		goto out;
	}

	if (!reservation_object_test_signaled_rcu(&resv, false)) {
		pr_err("empty reservation object not signaled [exclusive]\n");
		goto out;
	}

	if (!reservation_object_test_signaled_rcu(&resv, true)) {
		pr_err("empty reservation object not signaled [all]\n");
		goto out;
	}

	reservation_object_fini(&resv);

	ret = 0;
out:
	kfree(fences);
	return ret;
}

static int __init test_reservation_init(void)
{
	const u64 max_stride = ~0ull / NFENCES;
	int strides[] = { NSHARED-1, NSHARED, NSHARED+1 };
	int ret, n;

	pr_info("Testing reservation objects\n");

	for (n = 0; n < ARRAY_SIZE(strides); n++) {
		u64 stride;

		for (stride = 1; stride < max_stride; stride *= strides[n]) {
			enum direction dir;

			for (dir = forward; dir <= random; dir++) {
				ret = test_fences(stride, dir);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static void __exit test_reservation_cleanup(void)
{
}

module_init(test_reservation_init);
module_exit(test_reservation_cleanup);

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
