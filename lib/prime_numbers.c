#define pr_fmt(fmt) "prime numbers: " fmt "\n"

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/prime_numbers.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

struct primes {
	struct rcu_head rcu;
	unsigned long last, sz;
	unsigned long primes[];
};

#if BITS_PER_LONG == 64
static const struct primes small_primes = {
	.last = 61,
	.sz = 64,
	.primes = { 0x28208a20a08a28ac }
};
#elif BITS_PER_LONG == 32
static const struct primes small_primes = {
	.last = 31,
	.sz = 32,
	.primes = { 0xa08a28ac }
};
#else
#error "unhandled BITS_PER_LONG"
#endif

static DEFINE_MUTEX(lock);
static const struct primes __rcu *primes = &small_primes;

static unsigned long selftest_max;

static bool slow_is_prime_number(unsigned long x)
{
	unsigned long y = int_sqrt(x);

	while (y > 1) {
		if ((x % y) == 0)
			break;
		y--;
	}

	return y == 1;
}

static unsigned long slow_next_prime_number(unsigned long x)
{
	for (;;) {
		if (slow_is_prime_number(++x))
			return x;
	}
}

static unsigned long mark_multiples(unsigned long x,
				    unsigned long *p,
				    unsigned long start,
				    unsigned long end)
{
	unsigned long m;

	m = 2 * x;
	if (m < start)
		m = roundup(start, x);

	while (m < end) {
		__clear_bit(m, p);
		m += x;
	}

	return x;
}

static const struct primes *expand_to_next(unsigned long x)
{
	const struct primes *p;
	struct primes *new;
	unsigned long sz, y;

	rcu_read_unlock();

	/* Betrand's Theorem states:
	 * 	For all n > 1, there exists a prime p: n < p <= 2*n.
	 */
	sz = 2 * x + 1;
	if (sz < x)
		return NULL;

	sz = round_up(sz, BITS_PER_LONG);
	new = kmalloc(sizeof(*new) + sz / sizeof(long),
		      GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN);
	if (!new)
		new = vmalloc(sizeof(*new) + sz / sizeof(long));
	if (!new)
		return NULL;

	mutex_lock(&lock);
	p = rcu_dereference_protected(primes, lockdep_is_held(&lock));
	if (x < p->last) {
		kfree(new);
		goto relock;
	}

	/* Where memory permits, track the primes using the
	 * Sieve of Eratosthenes.
	 */
	memcpy(new->primes, p->primes, p->sz / BITS_PER_LONG * sizeof(long));
	memset(new->primes + p->sz / BITS_PER_LONG,
	       0xff, (sz - p->sz) / BITS_PER_LONG * sizeof(long));
	for (y = 2UL; y < sz; y = find_next_bit(new->primes, sz, y + 1))
		new->last = mark_multiples(y, new->primes, p->sz, sz);
	new->sz = sz;

	rcu_assign_pointer(primes, new);
	if (p != &small_primes)
		kfree_rcu((struct primes *)p, rcu);
	p = new;

relock:
	rcu_read_lock();
	mutex_unlock(&lock);
	return p;
}

static const struct primes *get_primes(unsigned long x)
{
	const struct primes *p;

	rcu_read_lock();
	p = rcu_dereference(primes);
	if (!p || x >= p->last)
		p = expand_to_next(x);

	/* returns under RCU iff p != NULL */
	return p;
}

unsigned long next_prime_number(unsigned long x)
{
	const struct primes *p;

	p = get_primes(x);
	if (unlikely(!p))
		return slow_next_prime_number(x);

	x = find_next_bit(p->primes, p->last, x + 1);
	rcu_read_unlock();

	return x;
}
EXPORT_SYMBOL(next_prime_number);

bool is_prime_number(unsigned long x)
{
	const struct primes *p;
	bool result;

	p = get_primes(x);
	if (unlikely(!p))
		return slow_is_prime_number(x);

	result = test_bit(x, p->primes);
	rcu_read_unlock();

	return result;
}
EXPORT_SYMBOL(is_prime_number);

static int selftest(unsigned long max)
{
	unsigned long x, last;

	if (!max)
		return 0;

	for (last = 0, x = 2; x < max; x++) {
		bool slow = slow_is_prime_number(x);
		bool fast = is_prime_number(x);

		if (slow != fast) {
			pr_err("inconsistent result for is-prime(%lu): slow=%s, fast=%s!",
			       x, slow ? "yes" : "no", fast ? "yes" : "no");
			rcu_read_lock();
			pr_info("primes.{last=%lu, .sz=%lu, .primes[]=...x%lx}",
				primes->last, primes->sz,
				primes->primes[primes->sz / BITS_PER_LONG - 1]);
			rcu_read_unlock();
			return -EINVAL;
		}

		if (!slow)
			continue;

		if (next_prime_number(last) != x) {
			pr_err("incorrect result for next-prime(%lu): expected %lu, got %lu",
			       last, x, next_prime_number(last));
			rcu_read_lock();
			pr_info("primes.{last=%lu, .sz=%lu, .primes[]=...0x%lx}",
				primes->last, primes->sz,
				primes->primes[primes->sz / BITS_PER_LONG - 1]);
			rcu_read_unlock();
			return -EINVAL;
		}
		last = x;
	}

	pr_info("selftest(%lu) passed, last prime was %lu", x, last);
	return 0;
}

static int __init primes_init(void)
{
	return selftest(selftest_max);
}

static void __exit primes_exit(void)
{
	if (primes != &small_primes)
		kfree_rcu((struct primes *)primes, rcu);
}

module_init(primes_init);
module_exit(primes_exit);

module_param_named(selftest, selftest_max, ulong, 0400);

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
