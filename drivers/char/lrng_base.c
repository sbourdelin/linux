/*
 * Linux Random Number Generator (LRNG)
 *
 * Documentation and test code: http://www.chronox.de/lrng.html
 *
 * Copyright (C) 2016 - 2017, Stephan Mueller <smueller@chronox.de>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL2
 * are required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/timex.h>
#include <linux/percpu.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/random.h>
#include <linux/workqueue.h>
#include <linux/poll.h>
#include <linux/cryptohash.h>
#include <linux/syscalls.h>
#include <linux/uuid.h>
#include <linux/slab.h>
#include <asm/irq_regs.h>

/* Security strength of LRNG -- this must match DRNG security strength */
#define LRNG_DRNG_SECURITY_STRENGTH_BYTES 32
#define LRNG_DRNG_SECURITY_STRENGTH_BITS (LRNG_DRNG_SECURITY_STRENGTH_BYTES * 8)

#define LRNG_DRNG_BLOCKSIZE 64		/* Maximum of DRNG block sizes */

/*
 * Alignmask which should cover all cipher implementations
 * WARNING: If this is changed to a value larger than 8, manual
 * alignment is necessary as older versions of GCC may not be capable
 * of aligning stack variables at boundaries greater than 8.
 * In this case, PTR_ALIGN must be used.
 */
#define LRNG_KCAPI_ALIGN 8

/* Primary DRNG state handle */
struct lrng_pdrng {
	void *pdrng;				/* DRNG handle */
	bool pdrng_fully_seeded;		/* Is DRNG fully seeded? */
	bool pdrng_min_seeded;			/* Is DRNG minimally seeded? */
	u32 pdrng_entropy_bits;			/* DRNG entropy level */
	struct work_struct lrng_seed_work;	/* (re)seed work queue */
	spinlock_t lock;
};

/* Secondary DRNG state handle */
struct lrng_sdrng {
	void *sdrng;				/* DRNG handle */
	atomic_t requests;			/* Number of DRNG requests */
	unsigned long last_seeded;		/* Last time it was seeded */
	bool fully_seeded;			/* Is DRNG fully seeded? */
	bool force_reseed;			/* Force a reseed */
	spinlock_t lock;
};

/*
 * SP800-90A defines a maximum request size of 1<<16 bytes. The given value is
 * considered a safer margin. This applies to secondary DRNG.
 *
 * This value is allowed to be changed.
 */
#define LRNG_DRNG_MAX_REQSIZE (1<<12)

/*
 * SP800-90A defines a maximum number of requests between reseeds of 1<<48.
 * The given value is considered a much safer margin, balancing requests for
 * frequent reseeds with the need to conserve entropy. This value MUST NOT be
 * larger than INT_MAX because it is used in an atomic_t. This applies to
 * secondary DRNG.
 *
 * This value is allowed to be changed.
 */
#define LRNG_DRNG_RESEED_THRESH (1<<17)

/* Status information about IRQ noise source */
struct lrng_irq_info {
	atomic_t num_events;	/* Number of non-stuck IRQs since last read */
	atomic_t num_events_thresh;	/* Reseed threshold */
	atomic_t last_time;	/* Stuck test: time of previous IRQ */
	atomic_t last_delta;	/* Stuck test: delta of previous IRQ */
	atomic_t last_delta2;	/* Stuck test: 2. time derivation of prev IRQ */
	atomic_t reseed_in_progress;	/* Flag for on executing reseed */
	bool irq_highres_timer;	/* Is high-resolution timer available? */
	bool stuck_test;	/* Perform stuck test ? */
	u32 irq_entropy_bits;	/* LRNG_IRQ_ENTROPY_BITS? */
};

/*
 * This is the entropy pool used by the slow noise source. Its size should
 * be at least as large as the interrupt entropy estimate.
 *
 * The pool array is aligned to 8 bytes to comfort the kernel crypto API cipher
 * implementations: for some accelerated implementations, we need an alignment
 * to avoid a realignment which involves memcpy(). The alignment to 8 bytes
 * should satisfy all crypto implementations.
 *
 * LRNG_POOL_SIZE is allowed to be changed only if the taps for the LFSR are
 * changed as well. The size must be in powers of 2 due to the mask handling in
 * lrng_pool_lfsr which uses AND instead of modulo.
 *
 * The polynomials for the LFSR are taken from the following URL
 * which lists primitive polynomials
 * http://courses.cse.tamu.edu/csce680/walker/lfsr_table.pdf. The first
 * polynomial is from "Primitive Binary Polynomials" by Wayne Stahnke (1993)
 * and is primitive as well as irreducible.
 *
 * Note, the tap values are smaller by one compared to the documentation because
 * they are used as an index into an array where the index starts by zero.
 *
 * All polynomials were also checked to be primitive and irreducible with magma.
 *
 * LRNG_POOL_SIZE must match the selected polynomial (i.e. LRNG_POOL_SIZE must
 * be equal to the first value of the polynomial plus one).
 */
static u32 const lrng_lfsr_polynomial[] =
	{ 127, 28, 26, 1 };			/* 128 words by Stahnke */
	/* { 255, 253, 250, 245 }; */		/* 256 words */
	/* { 511, 509, 506, 503 }; */		/* 512 words */
	/* { 1023, 1014, 1001, 1000 }; */	/* 1024 words */
	/* { 2047, 2034, 2033, 2028 }; */	/* 2048 words */
	/* { 4095, 4094, 4080, 4068 }; */	/* 4096 words */

struct lrng_pool {
#define LRNG_POOL_SIZE 128
#define LRNG_POOL_WORD_BYTES (sizeof(atomic_t))
#define LRNG_POOL_SIZE_BYTES (LRNG_POOL_SIZE * LRNG_POOL_WORD_BYTES)
#define LRNG_POOL_SIZE_BITS (LRNG_POOL_SIZE_BYTES * 8)
#define LRNG_POOL_WORD_BITS (LRNG_POOL_WORD_BYTES * 8)
	atomic_t pool[LRNG_POOL_SIZE];	/* Pool */
	atomic_t pool_ptr;	/* Ptr into pool for next IRQ word injection */
	atomic_t input_rotate;		/* rotate for LFSR */
	u32 numa_drngs;			/* Number of online DRNGs */
	bool all_online_numa_node_seeded;	/* All NUMA DRNGs seeded? */
	void *lrng_hash;
	struct lrng_irq_info irq_info;	/* IRQ noise source status info */
};

/*
 * Number of interrupts to be recorded to assume that DRNG security strength
 * bits of entropy are received.
 * Note: a value below the DRNG security strength should not be defined as this
 *	 may imply the DRNG can never be fully seeded in case other noise
 *	 sources are unavailable.
 *
 * This value is allowed to be changed.
 */
#define LRNG_IRQ_ENTROPY_BYTES (LRNG_DRNG_SECURITY_STRENGTH_BYTES)
#define LRNG_IRQ_ENTROPY_BITS (LRNG_IRQ_ENTROPY_BYTES * 8)

/*
 * Leave given amount of entropy in bits entropy pool to serve /dev/random while
 * /dev/urandom is stressed.
 *
 * This value is allowed to be changed.
 */
#define LRNG_EMERG_ENTROPY (LRNG_DRNG_SECURITY_STRENGTH_BITS * 2)

/*
 * Min required seed entropy is 128 bits covering the minimum entropy
 * requirement of SP800-131A and the German BSI's TR02102.
 *
 * This value is allowed to be changed.
 */
#define LRNG_MIN_SEED_ENTROPY_BITS 128

#define LRNG_INIT_ENTROPY_BITS 32

/*
 * Oversampling factor of IRQ events to obtain
 * LRNG_DRNG_SECURITY_STRENGTH_BYTES. This factor is used when a
 * high-resolution time stamp is not available. In this case, jiffies and
 * register contents are used to fill the entropy pool. These noise sources
 * are much less entropic than the high-resolution timer. The entropy content
 * is the entropy content assumed with LRNG_IRQ_ENTROPY_BYTES divided by
 * LRNG_IRQ_OVERSAMPLING_FACTOR.
 *
 * This value is allowed to be changed.
 */
#define LRNG_IRQ_OVERSAMPLING_FACTOR 10

static struct lrng_pdrng lrng_pdrng = {
	.lock = __SPIN_LOCK_UNLOCKED(lrng.pdrng.lock)
};

static struct lrng_sdrng **lrng_sdrng __read_mostly;

static struct lrng_pool lrng_pool __aligned(LRNG_KCAPI_ALIGN) =
					{ .irq_info = { .stuck_test = true } };

static LIST_HEAD(lrng_ready_list);
static DEFINE_SPINLOCK(lrng_ready_list_lock);

static atomic_t lrng_pdrng_avail = ATOMIC_INIT(0);
static atomic_t lrng_initrng_bytes = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(lrng_init_rng_lock);	/* Lock the init RNG state */

static DECLARE_WAIT_QUEUE_HEAD(lrng_read_wait);
static DECLARE_WAIT_QUEUE_HEAD(lrng_write_wait);
static DECLARE_WAIT_QUEUE_HEAD(lrng_pdrng_init_wait);
static struct fasync_struct *fasync;

/*
 * If the entropy count falls under this number of bits, then we
 * should wake up processes which are selecting or polling on write
 * access to /dev/random.
 */
static u32 lrng_write_wakeup_bits = LRNG_EMERG_ENTROPY +
				    2 * LRNG_DRNG_SECURITY_STRENGTH_BITS;

/*
 * The minimum number of bits of entropy before we wake up a read on
 * /dev/random.
 */
static u32 lrng_read_wakeup_bits = LRNG_POOL_WORD_BITS * 2;

/*
 * Maximum number of seconds between DRNG reseed intervals of the secondary
 * DRNG. Note, this is enforced with the next request of random numbers from
 * the secondary DRNG. Setting this value to zero implies a reseeding attempt
 * before every generated random number.
 */
static int lrng_sdrng_reseed_max_time = 600;

/************************** Crypto Implementations ***************************/

/**
 * Allocate DRNG -- the provided integer should be used for sanity checks.
 * @return: allocated data structure or PTR_ERR on error
 */
void *lrng_drng_alloc(u32 sec_strength);

/* Deallocate DRNG */
void lrng_drng_dealloc(void *drng);

/**
 * Seed the DRNG with data of arbitrary length
 * @drng: is pointer to data structure allocated with lrng_drng_alloc
 * @return: >= 0 on success, < 0 on error
 */
int lrng_drng_seed_helper(void *drng, const u8 *inbuf, u32 inbuflen);

/**
 * Generate random numbers from the DRNG with arbitrary length
 * @return: generated number of bytes, < 0 on error
 */
int lrng_drng_generate_helper(void *drng, u8 *outbuf, u32 outbuflen);

/**
 * Generate random numbers from the DRNG with arbitrary length where the
 * output is capable of providing 1 bit of entropy per data bit.
 * @return: generated number of bytes, < 0 on error
 */
int lrng_drng_generate_helper_full(void *drng, u8 *outbuf, u32 outbuflen);

/**
 * Allocate the hash for reading the entropy pool
 * @return: allocated data structure (NULL is success too) or ERR_PTR on error
 */
void *lrng_hash_alloc(const u8 *key, u32 keylen);

/**
 * Return the digestsize for the used hash to read out entropy pool
 * @hash: is pointer to data structure allocated with lrng_hash_alloc
 * @return: size of digest of hash in bytes
 */
u32 lrng_hash_digestsize(void *hash);

/**
 * Generate hash
 * @hash: is pointer to data structure allocated with lrng_hash_alloc
 * @return: 0 on success, < 0 on error
 */
int lrng_hash_buffer(void *hash, const u8 *inbuf, u32 inbuflen, u8 *digest);

/********************************** Helper ***********************************/

static inline u32 atomic_read_u32(atomic_t *v)
{
	return (u32)atomic_read(v);
}

static inline u32 atomic_xchg_u32(atomic_t *v, u32 x)
{
	return (u32)atomic_xchg(v, x);
}

static inline u32 lrng_entropy_to_data(u32 entropy_bits)
{
	return ((entropy_bits * lrng_pool.irq_info.irq_entropy_bits) /
		LRNG_DRNG_SECURITY_STRENGTH_BITS);
}

static inline u32 lrng_data_to_entropy(u32 irqnum)
{
	return ((irqnum * LRNG_DRNG_SECURITY_STRENGTH_BITS) /
		lrng_pool.irq_info.irq_entropy_bits);
}

static inline u32 lrng_avail_entropy(void)
{
	return min_t(u32, LRNG_POOL_SIZE_BITS, lrng_data_to_entropy(
			atomic_read_u32(&lrng_pool.irq_info.num_events)));
}

static inline void lrng_set_entropy_thresh(u32 new)
{
	atomic_set(&lrng_pool.irq_info.num_events_thresh,
		   lrng_entropy_to_data(new));
}

/* Is the primary DRNG seed level too low? */
static inline bool lrng_need_entropy(void)
{
	return ((lrng_avail_entropy() < lrng_write_wakeup_bits) &&
		(lrng_pdrng.pdrng_entropy_bits <
					LRNG_DRNG_SECURITY_STRENGTH_BITS));
}

/* Is the entropy pool filled for /dev/random pull or DRNG fully seeded? */
static inline bool lrng_have_entropy_full(void)
{
	return ((lrng_avail_entropy() >= lrng_read_wakeup_bits) ||
		lrng_pdrng.pdrng_entropy_bits >=
					LRNG_DRNG_SECURITY_STRENGTH_BITS);
}

/**
 * Ping all kernel internal callers waiting until the DRNG is fully
 * seeded that the DRNG is now fully seeded.
 */
static void lrng_process_ready_list(void)
{
	unsigned long flags;
	struct random_ready_callback *rdy, *tmp;

	spin_lock_irqsave(&lrng_ready_list_lock, flags);
	list_for_each_entry_safe(rdy, tmp, &lrng_ready_list, list) {
		struct module *owner = rdy->owner;

		list_del_init(&rdy->list);
		rdy->func(rdy);
		module_put(owner);
	}
	spin_unlock_irqrestore(&lrng_ready_list_lock, flags);
}

static __always_inline void lrng_debug_report_seedlevel(const char *name)
{
#ifdef CONFIG_WARN_UNSEEDED_RANDOM
	static void *previous = NULL;
	void *caller = (void *) _RET_IP_;

	if (READ_ONCE(previous) == caller)
		return;

	if (!lrng_pdrng.pdrng_min_seeded)
		pr_notice("lrng: %pF %s called without reaching "
			  "mimimally seeded level (received %u interrupts)\n",
			  caller, name,
			  atomic_read_u32(&lrng_pool.irq_info.num_events));

	WRITE_ONCE(previous, caller);
#endif
}

/*********************** Fast noise source processing ************************/

#ifdef CONFIG_CRYPTO_JITTERENTROPY
#include <crypto/jitterentropy.h>

/*
 * Estimated entropy of data is a 16th of LRNG_DRNG_SECURITY_STRENGTH_BITS.
 * Albeit a full entropy assessment is provided for the noise source indicating
 * that it provides high entropy rates and considering that it deactivates
 * when it detects insufficient hardware, the chosen under estimation of
 * entropy is considered to be acceptable to all reviewers.
 */
static u32 jitterrng = LRNG_DRNG_SECURITY_STRENGTH_BITS>>4;
module_param(jitterrng, uint, 0644);
MODULE_PARM_DESC(jitterrng, "Entropy in bits of of 256 data bits from Jitter "
			    "RNG noise source");

/**
 * Get Jitter RNG entropy
 *
 * @outbuf buffer to store entropy
 * @outbuflen length of buffer
 * @return > 0 on success where value provides the added entropy in bits
 *	   0 if no fast source was available
 */
static u32 lrng_get_jent(u8 *outbuf, unsigned int outbuflen)
{
	int ret;
	u32 ent_bits = jitterrng;
	unsigned long flags;
	static DEFINE_SPINLOCK(lrng_jent_lock);
	static int lrng_jent_initialized = 0;
	static unsigned char lrng_jent_mem[JENT_MEMORY_SIZE];
	static struct rand_data lrng_jent_state = {
		.data		= 0,
		.old_data	= 0,
		.prev_time	= 0,
		.last_delta	= 0,
		.last_delta2	= 0,
		.stuck		= 0,
		.osr		= 1,
		.disable_unbias	= 0,
		.mem		= lrng_jent_mem,
		.memlocation	= 0,
		.memblocks	= JENT_MEMORY_BLOCKSIZE,
		.memblocksize	= JENT_MEMORY_BLOCKS,
		.memaccessloops	= JENT_MEMORY_ACCESSLOOPS,
	};

	if (!ent_bits || (lrng_jent_initialized == -1))
		return 0;

	spin_lock_irqsave(&lrng_jent_lock, flags);
	if (!lrng_jent_initialized) {
		ret = jent_entropy_init();
		if (ret) {
			jitterrng = 0;
			lrng_jent_initialized = -1;
			pr_info("Jitter RNG unusable on current system %d\n",
				ret);
			return 0;
		}
		lrng_jent_initialized = 1;
		pr_debug("Jitter RNG working on current system\n");
	}
	ret = jent_read_entropy(&lrng_jent_state, outbuf, outbuflen);
	spin_unlock_irqrestore(&lrng_jent_lock, flags);

	if (ret) {
		pr_debug("Jitter RNG failed with %d\n", ret);
		return 0;
	}

	/* Obtain entropy statement */
	if (outbuflen != LRNG_DRNG_SECURITY_STRENGTH_BYTES)
		ent_bits = (ent_bits * outbuflen<<3) /
			   LRNG_DRNG_SECURITY_STRENGTH_BITS;
	/* Cap entropy to buffer size in bits */
	ent_bits = min_t(u32, ent_bits, outbuflen<<3);
	pr_debug("obtained %u bits of entropy from Jitter RNG noise source\n",
		 ent_bits);

	return ent_bits;
}
#else /* CONFIG_CRYPTO_JITTERENTROPY */
static u32 lrng_get_jent(u8 *outbuf, unsigned int outbuflen)
{
	return 0;
}
#endif /* CONFIG_CRYPTO_JITTERENTROPY */

/*
 * Estimated entropy of data is a 32th of LRNG_DRNG_SECURITY_STRENGTH_BITS.
 * As we have no ability to review the implementation of those noise sources,
 * it is prudent to have a conservative estimate here.
 */
static u32 archrandom = LRNG_DRNG_SECURITY_STRENGTH_BITS>>5;
module_param(archrandom, uint, 0644);
MODULE_PARM_DESC(archrandom, "Entropy in bits of 256 data bits from CPU noise "
			     "source (e.g. RDRAND)");

/**
 * Get CPU noise source entropy
 *
 * @outbuf: buffer to store entropy of size LRNG_DRNG_SECURITY_STRENGTH_BYTES
 * @return: > 0 on success where value provides the added entropy in bits
 *	    0 if no fast source was available
 */
static inline u32 lrng_get_arch(u8 *outbuf)
{
	u32 i;
	u32 ent_bits = archrandom;

	/* operate on full blocks */
	BUILD_BUG_ON(LRNG_DRNG_SECURITY_STRENGTH_BYTES % sizeof(unsigned long));

	if (!ent_bits)
		return 0;

	for (i = 0; i < LRNG_DRNG_SECURITY_STRENGTH_BYTES;
	     i += sizeof(unsigned long)) {
		if (!arch_get_random_seed_long((unsigned long *)(outbuf + i)) &&
		    !arch_get_random_long((unsigned long *)(outbuf + i))) {
			archrandom = 0;
			return 0;
		}
	}

	/* Obtain entropy statement -- cap entropy to buffer size in bits */
	ent_bits = min_t(u32, ent_bits, LRNG_DRNG_SECURITY_STRENGTH_BITS);
	pr_debug("obtained %u bits of entropy from CPU RNG noise source\n",
		 ent_bits);
	return ent_bits;
}

/************************ Slow noise source processing ************************/

/*
 * Implement a (modified) twisted Generalized Feedback Shift Register. (See M.
 * Matsumoto & Y. Kurita, 1992.  Twisted GFSR generators. ACM Transactions on
 * Modeling and Computer Simulation 2(3):179-194.  Also see M. Matsumoto & Y.
 * Kurita, 1994.  Twisted GFSR generators II.  ACM Transactions on Modeling and
 * Computer Simulation 4:254-266).
 */
static u32 const lrng_twist_table[8] = {
	0x00000000, 0x3b6e20c8, 0x76dc4190, 0x4db26158,
	0xedb88320, 0xd6d6a3e8, 0x9b64c2b0, 0xa00ae278 };

/**
 * Hot code path - inject data into entropy pool using LFSR
 *
 * The function is not marked as inline to support SystemTap testing of the
 * parameter which is considered to be the raw entropy.
 */
static void lrng_pool_lfsr_u32(u32 value)
{
	/*
	 * Process the LFSR by altering not adjacent words but rather
	 * more spaced apart words. Using a prime number ensures that all words
	 * are processed evenly. As some the LFSR polynomials taps are close
	 * together, processing adjacent words with the LSFR taps may be
	 * inappropriate as the data just mixed-in at these taps may be not
	 * independent from the current data to be mixed in.
	 */
	u32 ptr = (u32)atomic_add_return(67, &lrng_pool.pool_ptr) &
							(LRNG_POOL_SIZE - 1);
	/*
	 * Add 7 bits of rotation to the pool. At the beginning of the
	 * pool, add an extra 7 bits rotation, so that successive passes
	 * spread the input bits across the pool evenly.
	 */
	u32 input_rotate = (u32)atomic_add_return((ptr ? 7 : 14),
					&lrng_pool.input_rotate) & 31;
	u32 word = rol32(value, input_rotate);

	BUILD_BUG_ON(LRNG_POOL_SIZE - 1 != lrng_lfsr_polynomial[0]);
	word ^= atomic_read_u32(&lrng_pool.pool[ptr]);
	word ^= atomic_read_u32(&lrng_pool.pool[
		(ptr + lrng_lfsr_polynomial[0]) & (LRNG_POOL_SIZE - 1)]);
	word ^= atomic_read_u32(&lrng_pool.pool[
		(ptr + lrng_lfsr_polynomial[1]) & (LRNG_POOL_SIZE - 1)]);
	word ^= atomic_read_u32(&lrng_pool.pool[
		(ptr + lrng_lfsr_polynomial[2]) & (LRNG_POOL_SIZE - 1)]);
	word ^= atomic_read_u32(&lrng_pool.pool[
		(ptr + lrng_lfsr_polynomial[3]) & (LRNG_POOL_SIZE - 1)]);

	word = (word >> 3) ^ lrng_twist_table[word & 7];
	atomic_set(&lrng_pool.pool[ptr], word);
}

/* invoke function with buffer aligned to 4 bytes */
static inline void lrng_pool_lfsr(const u8 *buf, u32 buflen)
{
	u32 *p_buf = (u32 *)buf;

	for (; buflen >= 4; buflen -= 4)
		lrng_pool_lfsr_u32(*p_buf++);

	buf = (u8 *)p_buf;
	while (buflen--)
		lrng_pool_lfsr_u32(*buf++);
}

static inline void lrng_pool_lfsr_nonalinged(const u8 *buf, u32 buflen)
{
	if (!((unsigned long)buf & (sizeof(u32) - 1)))
		lrng_pool_lfsr(buf, buflen);
	else {
		while (buflen--)
			lrng_pool_lfsr_u32(*buf++);
	}
}

/**
 * Hot code path - Stuck test by checking the:
 *      1st derivative of the event occurrence (time delta)
 *      2nd derivative of the event occurrence (delta of time deltas)
 *      3rd derivative of the event occurrence (delta of delta of time deltas)
 *
 * All values must always be non-zero. The stuck test is disabled, if
 * no high-resolution time stamp is identified after initialization.
 *
 * @irq_info: Reference to IRQ information
 * @now: Event time
 * @return: 0 event occurrence not stuck (good bit)
 *	    1 event occurrence stuck (reject bit)
 */
static inline int lrng_irq_stuck(struct lrng_irq_info *irq_info, u32 now_time)
{
	u32 delta = now_time - atomic_xchg_u32(&irq_info->last_time, now_time);
	int delta2 = delta - atomic_xchg_u32(&irq_info->last_delta, delta);
	int delta3 = delta2 - atomic_xchg(&irq_info->last_delta2, delta2);

	if (!irq_info->stuck_test)
		return 0;

	if (!delta || !delta2 || !delta3)
		return 1;

	return 0;
}

/**
 * Hot code path - mix data into entropy pool
 */
static inline void lrng_pool_mixin(u32 irq_num)
{
	/* Should we wake readers? */
	if (!(atomic_read_u32(&lrng_pool.pool_ptr) & 0x3f) &&
	    irq_num >= lrng_entropy_to_data(lrng_read_wakeup_bits)) {
		wake_up_interruptible(&lrng_read_wait);
		kill_fasync(&fasync, SIGIO, POLL_IN);
	}

	/*
	 * Once all secondary DRNGs are fully seeded, the interrupt noise
	 * sources will not trigger any reseeding any more.
	 */
	if (lrng_pool.all_online_numa_node_seeded)
		return;

	/* Only try to reseed if the DRNG is alive. */
	if (!atomic_read(&lrng_pdrng_avail))
		return;

	/* Only trigger the DRNG reseed if we have collected enough IRQs. */
	if (atomic_read_u32(&lrng_pool.irq_info.num_events) <
	    atomic_read_u32(&lrng_pool.irq_info.num_events_thresh))
		return;

	/* Ensure that the seeding only occurs once at any given time. */
	if (atomic_cmpxchg(&lrng_pool.irq_info.reseed_in_progress, 0, 1))
		return;

	/* Seed the DRNG with IRQ noise. */
	schedule_work(&lrng_pdrng.lrng_seed_work);
}

/**
 * Hot code path - Callback for interrupt handler
 */
void add_interrupt_randomness(int irq, int irq_flags)
{
	u32 now_time = random_get_entropy();
	struct lrng_irq_info *irq_info = &lrng_pool.irq_info;

	lrng_pool_lfsr_u32(now_time);

	if (!irq_info->irq_highres_timer) {
		struct pt_regs *regs = get_irq_regs();
		static atomic_t reg_idx = ATOMIC_INIT(0);
		u64 ip;

		lrng_pool_lfsr_u32(jiffies);
		lrng_pool_lfsr_u32(irq);
		lrng_pool_lfsr_u32(irq_flags);

		if (regs) {
			u32 *ptr = (u32 *)regs;
			int reg_ptr = atomic_add_return(1, &reg_idx);

			ip = instruction_pointer(regs);
			if (reg_ptr >= (sizeof(struct pt_regs) / sizeof(u32))) {
				atomic_set(&reg_idx, 0);
				reg_ptr = 0;
			}
			lrng_pool_lfsr_u32(*(ptr + reg_ptr));
		} else
			ip = _RET_IP_;

		lrng_pool_lfsr_u32(ip >> 32);
		lrng_pool_lfsr_u32(ip);
	}

	if (!lrng_irq_stuck(irq_info, now_time))
		lrng_pool_mixin(atomic_add_return(1, &irq_info->num_events));
}
EXPORT_SYMBOL(add_interrupt_randomness);

/**
 * Callback for HID layer -- use the HID event values to stir the pool
 */
void add_input_randomness(unsigned int type, unsigned int code,
			  unsigned int value)
{
	static unsigned char last_value;

	/* ignore autorepeat and the like */
	if (value == last_value)
		return;

	last_value = value;

	lrng_pool_lfsr_u32((type << 4) ^ code ^ (code >> 4) ^ value);
}
EXPORT_SYMBOL_GPL(add_input_randomness);

/*
 * Add device- or boot-specific data to the input pool to help
 * initialize it.
 *
 * None of this adds any entropy; it is meant to avoid the problem of
 * the entropy pool having similar initial state across largely
 * identical devices.
 */
void add_device_randomness(const void *buf, unsigned int size)
{
	lrng_pool_lfsr_nonalinged((u8 *)&buf, size);
	lrng_pool_lfsr_u32(random_get_entropy());
	lrng_pool_lfsr_u32(jiffies);
}
EXPORT_SYMBOL(add_device_randomness);

/**
 * Read the entropy pool out for use. The caller must ensure this function
 * is only called once at a time.
 *
 * This function handles the translation from the number of received interrupts
 * into an entropy statement. The conversion depends on LRNG_IRQ_ENTROPY_BYTES
 * which defines how many interrupts must be received to obtain 256 bits of
 * entropy. With this value, the function lrng_data_to_entropy converts a given
 * data size (received interrupts, requested amount of data, etc.) into an
 * entropy statement. lrng_entropy_to_data does the reverse.
 *
 * Both functions are agnostic about the type of data: when the number of
 * interrupts is processed by these functions, the resulting entropy value is in
 * bits as we assume the entropy of interrupts is measured in bits. When data is
 * processed, the entropy value is in bytes as the data is measured in bytes.
 *
 * @outbuf: buffer to store data in with size LRNG_DRNG_SECURITY_STRENGTH_BYTES
 * @requested_entropy_bits: requested bits of entropy -- the function will
 *			    return at least this amount of entropy if available
 * @drain: boolean indicating that that all entropy of pool can be used
 *	   (otherwise some emergency amount of entropy is left)
 * @return: estimated entropy from the IRQs that was obtained
 */
static u32 lrng_get_pool(u8 *outbuf, u32 requested_entropy_bits, bool drain)
{
	u32 i, avail_entropy_bytes, irq_num_events_used, irq_num_event_back;
	/* How many unused interrupts are in entropy pool? */
	u32 irq_num_events = atomic_xchg_u32(&lrng_pool.irq_info.num_events, 0);
	/* Convert available interrupts into entropy statement */
	u32 avail_entropy_bits = lrng_data_to_entropy(irq_num_events);
	u32 digestsize = lrng_hash_digestsize(lrng_pool.lrng_hash);
	u8 digest[digestsize] __aligned(LRNG_KCAPI_ALIGN);

	/* Cap available entropy to pool size */
	avail_entropy_bits =
			min_t(u32, avail_entropy_bits, LRNG_POOL_SIZE_BITS);

	/* How much entropy we need to and can we use? */
	if (drain)
		/* read for the primary DRNG or not fully seeded 2ndary DRNG */
		avail_entropy_bits = min_t(u32, avail_entropy_bits,
					   requested_entropy_bits);
	else {
		/*
		 * Read for 2ndary DRNG: leave the emergency fill level.
		 *
		 * Only obtain data if we have at least the requested entropy
		 * available. The idea is to prevent the transfer of, say
		 * one byte at a time, because one byte of entropic data
		 * can be brute forced by an attacker.
		 */
		if ((requested_entropy_bits + LRNG_EMERG_ENTROPY) >
		     avail_entropy_bits) {
			avail_entropy_bits = 0;
			goto out;
		}
		avail_entropy_bits = requested_entropy_bits;
	}

	/* Hash is a compression function: we generate entropy amount of data */
	avail_entropy_bits = round_down(avail_entropy_bits, 8);
	avail_entropy_bytes = avail_entropy_bits >> 3;
	BUG_ON(avail_entropy_bytes > LRNG_DRNG_SECURITY_STRENGTH_BYTES);

	/* Hash the entire entropy pool */
	for (i = 0;
	     i < LRNG_DRNG_SECURITY_STRENGTH_BYTES && avail_entropy_bytes > 0;
	     i += digestsize) {
		u32 tocopy = min3(avail_entropy_bytes, digestsize,
				  (LRNG_DRNG_SECURITY_STRENGTH_BYTES - i));

		if (lrng_hash_buffer(lrng_pool.lrng_hash, (u8 *)lrng_pool.pool,
				     LRNG_POOL_SIZE_BYTES, digest)) {
			/* We report the successfully read entropy. */
			avail_entropy_bits = i<<3;
			memzero_explicit(digest, digestsize);
			goto out;
		}

		/* Mix read data back into pool for backtracking resistance */
		lrng_pool_lfsr(digest, digestsize);
		/* Copy the data out to the caller */
		memcpy(outbuf + i, digest, tocopy);
		avail_entropy_bytes -= tocopy;
	}
	memzero_explicit(digest, digestsize);

out:
	/* There may be new events that came in while we processed this logic */
	irq_num_events += atomic_xchg_u32(&lrng_pool.irq_info.num_events, 0);
	/* Convert used entropy into interrupt number for subtraction */
	irq_num_events_used = lrng_entropy_to_data(avail_entropy_bits);
	/* Cap the number of events we say we have left to not reuse events */
	irq_num_event_back = min_t(u32, irq_num_events - irq_num_events_used,
				   lrng_entropy_to_data(LRNG_POOL_SIZE_BITS) -
				    irq_num_events_used);
	/* Add the unused interrupt number back to the state variable */
	atomic_add(irq_num_event_back, &lrng_pool.irq_info.num_events);

	/* Obtain entropy statement in bits from the used entropy */
	pr_debug("obtained %u bits of entropy from %u newly collected "
		 "interrupts - not using %u interrupts\n", avail_entropy_bits,
		 irq_num_events_used, irq_num_event_back);

	return avail_entropy_bits;
}

/************************* primary DRNG processing ***************************/

static void invalidate_batched_entropy(void);

/**
 * Set the slow noise source reseed trigger threshold. The initial threshold
 * is set to the minimum data size that can be read from the pool: a word. Upon
 * reaching this value, the next seed threshold of 128 bits is set followed
 * by 256 bits.
 *
 * @entropy_bits: size of entropy currently injected into DRNG
 */
static void lrng_pdrng_init_ops(u32 entropy_bits)
{
	if (lrng_pdrng.pdrng_fully_seeded)
		return;

	/* DRNG is seeded with full security strength */
	if (entropy_bits >= LRNG_DRNG_SECURITY_STRENGTH_BITS) {
		invalidate_batched_entropy();
		lrng_pdrng.pdrng_fully_seeded = true;
		lrng_pdrng.pdrng_min_seeded = true;
		pr_info("primary DRNG fully seeded with %u bits of entropy\n",
			entropy_bits);
		lrng_set_entropy_thresh(LRNG_DRNG_SECURITY_STRENGTH_BITS);
		lrng_process_ready_list();
		wake_up_all(&lrng_pdrng_init_wait);

	} else if (!lrng_pdrng.pdrng_min_seeded) {

		/* DRNG is seeded with at least 128 bits of entropy */
		if (entropy_bits >= LRNG_MIN_SEED_ENTROPY_BITS) {
			invalidate_batched_entropy();
			lrng_pdrng.pdrng_min_seeded = true;
			pr_info("primary DRNG minimally seeded with %u bits of "
				"entropy\n", entropy_bits);
			lrng_set_entropy_thresh(
					LRNG_DRNG_SECURITY_STRENGTH_BITS);
			lrng_process_ready_list();
			wake_up_all(&lrng_pdrng_init_wait);

		/* DRNG is seeded with at least LRNG_INIT_ENTROPY_BITS bits */
		} else if (entropy_bits >= LRNG_INIT_ENTROPY_BITS) {
			pr_info("primary DRNG initially seeded with %u bits of "
				"entropy\n", entropy_bits);
			lrng_set_entropy_thresh(LRNG_MIN_SEED_ENTROPY_BITS);
		}
	}
}

/* Caller must hold lrng_pdrng.lock */
static int lrng_pdrng_generate(u8 *outbuf, u32 outbuflen, bool fullentropy)
{
	int ret;

	/* /dev/random only works from a fully seeded DRNG */
	if (fullentropy && !lrng_pdrng.pdrng_fully_seeded)
		return 0;

	/*
	 * Only deliver as many bytes as the DRNG is seeded with except during
	 * initialization to provide a first seed to the secondary DRNG.
	 */
	if (lrng_pdrng.pdrng_min_seeded)
		outbuflen = min_t(u32, outbuflen,
				  lrng_pdrng.pdrng_entropy_bits>>3);
	else
		outbuflen = min_t(u32, outbuflen,
				  LRNG_MIN_SEED_ENTROPY_BITS>>3);
	if (!outbuflen)
		return 0;

	ret = lrng_drng_generate_helper_full(lrng_pdrng.pdrng, outbuf,
					     outbuflen);
	if (ret != outbuflen) {
		pr_warn("getting random data from primary DRNG failed (%d)\n",
			ret);
		return ret;
	}

	if (lrng_pdrng.pdrng_entropy_bits > (u32)(ret<<3))
		lrng_pdrng.pdrng_entropy_bits -= ret<<3;
	else
		lrng_pdrng.pdrng_entropy_bits = 0;
	pr_debug("obtained %d bytes of random data from primary DRNG\n", ret);
	pr_debug("primary DRNG entropy level at %u bits\n",
		 lrng_pdrng.pdrng_entropy_bits);

	return ret;
}

/**
 * Inject data into the primary DRNG with a given entropy value. The function
 * calls the DRNG's update function. This function also generates random data
 * if requested by caller. The caller is only returned the amount of random
 * data that is at most equal to the amount of entropy that just seeded the
 * DRNG.
 *
 * Note, this function seeds the primary DRNG and generates data from it
 * in an atomic operation.
 *
 * @inbuf: buffer to inject
 * @inbuflen: length of inbuf
 * @entropy_bits: entropy value of the data in inbuf in bits
 * @outbuf: buffer to fill immediately after seeding to get full entropy
 * @outbuflen: length of outbuf
 * @fullentropy: start /dev/random output only after the DRNG was fully seeded
 * @return: number of bytes written to outbuf, 0 if outbuf is not supplied,
 *	    or < 0 in case of error
 */
static int lrng_pdrng_inject(const u8 *inbuf, u32 inbuflen, u32 entropy_bits,
			     u8 *outbuf, u32 outbuflen, bool fullentropy)
{
	int ret;
	unsigned long flags;

	/* cap the maximum entropy value to the provided data length */
	entropy_bits = min_t(u32, entropy_bits, inbuflen<<3);

	spin_lock_irqsave(&lrng_pdrng.lock, flags);
	ret = lrng_drng_seed_helper(lrng_pdrng.pdrng, inbuf, inbuflen);
	if (ret < 0) {
		pr_warn("(re)seeding of primary DRNG failed\n");
		goto unlock;
	}
	pr_debug("inject %u bytes with %u bits of entropy into primary DRNG\n",
		 inbuflen, entropy_bits);

	/* Adjust the fill level indicator to at most the DRNG sec strength */
	lrng_pdrng.pdrng_entropy_bits =
		min_t(u32, lrng_pdrng.pdrng_entropy_bits + entropy_bits,
		      LRNG_DRNG_SECURITY_STRENGTH_BITS);
	lrng_pdrng_init_ops(lrng_pdrng.pdrng_entropy_bits);

	if (outbuf && outbuflen)
		ret = lrng_pdrng_generate(outbuf, outbuflen, fullentropy);

unlock:
	spin_unlock_irqrestore(&lrng_pdrng.lock, flags);

	if (lrng_have_entropy_full()) {
		/* Wake readers */
		wake_up_interruptible(&lrng_read_wait);
		kill_fasync(&fasync, SIGIO, POLL_IN);
	}

	return ret;
}

/**
 * Seed the primary DRNG from the internal noise sources and generate
 * random data. The seeding and the generation of random data is an atomic
 * operation for the caller.
 *
 * lrng_pool.irq_info.reseed_in_progress must be held by caller.
 */
static int lrng_pdrng_seed_locked(u8 *outbuf, u32 outbuflen, bool fullentropy,
				  bool drain)
{
	u32 total_entropy_bits;
	struct {
		u8 a[LRNG_DRNG_SECURITY_STRENGTH_BYTES];
		u8 b[LRNG_DRNG_SECURITY_STRENGTH_BYTES];
		u8 c[LRNG_DRNG_SECURITY_STRENGTH_BYTES];
		u32 now;
	} entropy_buf __aligned(LRNG_KCAPI_ALIGN);
	int ret, retrieved = 0;

	/* Get available entropy in primary DRNG */
	if (lrng_pdrng.pdrng_entropy_bits>>3) {
		unsigned long flags;

		spin_lock_irqsave(&lrng_pdrng.lock, flags);
		ret = lrng_pdrng_generate(outbuf, outbuflen, fullentropy);
		spin_unlock_irqrestore(&lrng_pdrng.lock, flags);
		if (ret > 0) {
			retrieved += ret;
			if (ret == outbuflen)
				goto out;

			outbuf += ret;
			outbuflen -= ret;
		}
		/* Disregard error code as another generate request is below. */
	}

	/*
	 * drain the pool completely during init and when /dev/random calls.
	 *
	 * lrng_get_pool must be guaranteed to be called with multiples of 8
	 * (bits) of entropy as it can only operate byte-wise.
	 */
	total_entropy_bits = lrng_get_pool(entropy_buf.a,
					   LRNG_DRNG_SECURITY_STRENGTH_BITS,
					   drain);

	/*
	 * Concatenate the output of the noise sources. This would be the
	 * spot to add an entropy extractor logic if desired. Note, this
	 * entirety should have the ability to collect entropy equal or larger
	 * than the DRNG strength to be able to feed /dev/random.
	 */
	total_entropy_bits += lrng_get_arch(entropy_buf.b);
	total_entropy_bits += lrng_get_jent(entropy_buf.c,
					    LRNG_DRNG_SECURITY_STRENGTH_BYTES);

	pr_debug("reseed primary DRNG from internal noise sources with %u bits "
		 "of entropy\n", total_entropy_bits);

	/* also reseed the DRNG with the current time stamp */
	entropy_buf.now = random_get_entropy();

	ret = lrng_pdrng_inject((u8 *)&entropy_buf, sizeof(entropy_buf),
				total_entropy_bits,
				outbuf, outbuflen, fullentropy);

	memzero_explicit(&entropy_buf, sizeof(entropy_buf));

	if (ret > 0)
		retrieved += ret;

	/*
	 * Shall we wake up user space writers? This location covers
	 * /dev/urandom as well, but also ensures that the user space provider
	 * does not dominate the internal noise sources since in case the
	 * first call of this function finds sufficient entropy in the primary
	 * DRNG, it will not trigger the wakeup. This implies that when the next
	 * /dev/urandom read happens, the primary DRNG is drained and the
	 * internal noise sources are asked to feed the primary DRNG.
	 */
	if (lrng_need_entropy()) {
		wake_up_interruptible(&lrng_write_wait);
		kill_fasync(&fasync, SIGIO, POLL_OUT);
	}

out:
	/* Allow the seeding operation to be called again */
	atomic_set(&lrng_pool.irq_info.reseed_in_progress, 0);

	return (ret >= 0) ? retrieved : ret;
}

static int lrng_pdrng_seed(u8 *outbuf, u32 outbuflen, bool fullentropy,
			   bool drain)
{
	/* Ensure that the seeding only occurs once at any given time */
	if (atomic_cmpxchg(&lrng_pool.irq_info.reseed_in_progress, 0, 1))
		return -EINPROGRESS;
	return lrng_pdrng_seed_locked(outbuf, outbuflen, fullentropy, drain);
}

/**
 * Obtain random data from DRNG with information theoretical entropy by
 * triggering a reseed. The primary DRNG will only return as many random
 * bytes as it was seeded with.
 *
 * @outbuf: buffer to store the random data in
 * @outbuflen: length of outbuf
 * @return: < 0 on error
 *	    >= 0 the number of bytes that were obtained
 */
static int lrng_pdrng_get(u8 *outbuf, u32 outbuflen)
{
	int ret;

	if (!outbuf || !outbuflen)
		return 0;

	/* DRNG is not yet available */
	if (!atomic_read(&lrng_pdrng_avail))
		return 0;

	ret = lrng_pdrng_seed(outbuf, outbuflen, true, true);
	if (ret > 0)
		pr_debug("read %d bytes of full entropy data from primary "
			 "DRNG\n", ret);
	else
		pr_debug("reading data from primary DRNG failed: %d\n", ret);

	return ret;
}

/************************** initial RNG processing ***************************/

/**
 * Initial RNG provides random data with as much entropy as we have
 * at boot time until the DRNG becomes available during late_initcall() but
 * before user space boots. When the DRNG is initialized, the initial RNG
 * is retired.
 *
 * Note: until retirement of this RNG, the system did not generate too much
 * entropy yet. Hence, a proven DRNG like a DRBG is not necessary here anyway.
 *
 * The RNG is using the following as noise source:
 *	* high resolution time stamps
 *	* the collected IRQ state
 *	* CPU noise source if available
 *	* Jitter RNG if available
 *
 * Input/output: it is a drop-in replacement for lrng_sdrng_get.
 */
static u32 lrng_init_state[SHA_WORKSPACE_WORDS];
static int lrng_init_rng(u8 *outbuf, u32 outbuflen)
{
	u32 hash[SHA_DIGEST_WORDS], workspace[SHA_WORKSPACE_WORDS];
	u32 outbuflen_orig = outbuflen, i;

	BUILD_BUG_ON(sizeof(lrng_init_state[0]) != LRNG_POOL_WORD_BYTES);

	/* Seed with Jitter RNG entropy data once. */
	if (!atomic_read(&lrng_initrng_bytes)) {
		u32 jent = lrng_get_jent((u8 *)workspace,
					 (SHA_WORKSPACE_WORDS * sizeof(u32)));
		if (jent) {
			for (i = 0; i < SHA_WORKSPACE_WORDS; i++)
				lrng_init_state[i] ^= workspace[i];
		}
	}

	sha_init(hash);
	while (outbuflen) {
		unsigned int arch;
		u32 todo = min_t(u32, outbuflen,
				 SHA_WORKSPACE_WORDS * sizeof(u32));

		/* Update init RNG state with CPU RNG and timer data */
		for (i = 0; i < SHA_WORKSPACE_WORDS; i++) {
			if (arch_get_random_int(&arch))
				lrng_init_state[i] ^= arch;
			lrng_init_state[i] ^= random_get_entropy();
		}
		/* SHA-1 update using the init RNG state */
		sha_transform(hash, (u8 *)&lrng_init_state, workspace);

		/* SHA-1 update with all words of the entropy pool */
		BUILD_BUG_ON(LRNG_POOL_SIZE % 16);
		for (i = 0; i < LRNG_POOL_SIZE; i += 16)
			sha_transform(hash, (u8 *)(lrng_pool.pool + i),
				      workspace);

		/* Mix generated data into state for backtracking resistance */
		for (i = 0; i < SHA_DIGEST_WORDS; i++)
			lrng_init_state[i] ^= hash[i];

		memcpy(outbuf, hash, todo);
		outbuf += todo;
		outbuflen -= todo;
		atomic_add(todo, &lrng_initrng_bytes);
	}
	memzero_explicit(hash, sizeof(hash));
	memzero_explicit(workspace, sizeof(workspace));

	return outbuflen_orig;
}

/************************ secondary DRNG processing **************************/

/**
 * Inject a data buffer into the secondary DRNG
 *
 * @sdrng: reference to secondary DRNG
 * @inbuf: buffer with data to inject
 * @inbuflen: buffer length
 * @internal: did random data originate from internal sources? Update the
 *	      reseed threshold and the reseed timer when seeded with entropic
 *	      data from noise sources to prevent unprivileged users from
 *	      stopping reseeding the secondary DRNG with entropic data.
 */
static void lrng_sdrng_inject(struct lrng_sdrng *sdrng,
			      const u8 *inbuf, u32 inbuflen, bool internal)
{
	unsigned long flags;

	BUILD_BUG_ON(LRNG_DRNG_RESEED_THRESH > INT_MAX);
	pr_debug("seeding secondary DRNG with %u bytes\n", inbuflen);
	spin_lock_irqsave(&sdrng->lock, flags);
	if (lrng_drng_seed_helper(sdrng->sdrng, inbuf, inbuflen) < 0) {
		pr_warn("seeding of secondary DRNG failed\n");
		atomic_set(&sdrng->requests, 1);
	} else if (internal) {
		pr_debug("secondary DRNG stats since last seeding: %lu secs; "
			 "generate calls: %d\n",
			 (time_after(jiffies, sdrng->last_seeded) ?
			  (jiffies - sdrng->last_seeded) : 0) / HZ,
			 (LRNG_DRNG_RESEED_THRESH -
			  atomic_read(&sdrng->requests)));
		sdrng->last_seeded = jiffies;
		atomic_set(&sdrng->requests, LRNG_DRNG_RESEED_THRESH);
	}
	spin_unlock_irqrestore(&sdrng->lock, flags);
}

/**
 * Try to seed the secondary DRNG by pulling data from the primary DRNG
 *
 * @sdrng: reference to secondary DRNG
 * @seedfunc: function to use to seed and obtain random data from primary DRNG
 */
static void lrng_sdrng_seed(struct lrng_sdrng *sdrng,
	int (*seed_func)(u8 *outbuf, u32 outbuflen, bool fullentropy,
			 bool drain))
{
	u8 seedbuf[LRNG_DRNG_SECURITY_STRENGTH_BYTES]
						__aligned(LRNG_KCAPI_ALIGN);
	int ret;

	BUILD_BUG_ON(LRNG_MIN_SEED_ENTROPY_BITS >
		     LRNG_DRNG_SECURITY_STRENGTH_BITS);

	ret = seed_func(seedbuf, LRNG_DRNG_SECURITY_STRENGTH_BYTES, false,
			!sdrng->fully_seeded);
	/* Update the DRNG state even though we received zero random data */
	if (ret < 0) {
		/*
		 * Try to reseed at next round - note if EINPROGRESS is returned
		 * the request counter may fall below zero in case of parallel
		 * operations. We accept such "underflow" temporarily as the
		 * counter will be set back to a positive number in the course
		 * of the reseed. For these few generate operations under
		 * heavy parallel strain of /dev/urandom we therefore exceed
		 * the LRNG_DRNG_RESEED_THRESH threshold.
		 */
		if (ret != -EINPROGRESS)
			atomic_set(&sdrng->requests, 1);
		return;
	}

	lrng_sdrng_inject(sdrng, seedbuf, ret, true);
	memzero_explicit(seedbuf, ret);

	sdrng->force_reseed = false;

	if (ret >= LRNG_DRNG_SECURITY_STRENGTH_BYTES)
		sdrng->fully_seeded = true;
}

/**
 * DRNG reseed trigger: Kernel thread handler triggered by the schedule_work()
 */
static void lrng_sdrng_seed_work(struct work_struct *dummy)
{
	u32 node;

	for_each_online_node(node) {
		struct lrng_sdrng *sdrng = lrng_sdrng[node];

		if (!sdrng)
			continue;

		if (!sdrng->fully_seeded) {
			pr_debug("reseed triggered by interrupt noise source "
				 "for secondary DRNG on NUMA node %d\n", node);
			lrng_sdrng_seed(sdrng, lrng_pdrng_seed_locked);
			if (sdrng->fully_seeded) {
				/* Prevent reseed storm */
				sdrng->last_seeded += node * 100 * HZ;
				/* Prevent draining of pool on idle systems */
				lrng_sdrng_reseed_max_time += 100;
			}
			return;
		}
	}
	lrng_pool.all_online_numa_node_seeded = true;
	/* Allow the seeding operation to be called again */
	atomic_set(&lrng_pool.irq_info.reseed_in_progress, 0);
}

/**
 * Get the NUMA-node local secondary DRNG instance, if fully seeded. Otherwise
 * return the first secondary DRNG instance which will be seeded first.
 */
static inline struct lrng_sdrng *lrng_sdrng_get_local(void)
{
	struct lrng_sdrng *sdrng = lrng_sdrng[numa_node_id()];

	return (sdrng->fully_seeded) ? sdrng : lrng_sdrng[0];
}

/**
 * Get random data out of the secondary DRNG which is reseeded frequently. In
 * the worst case, the DRNG may generate random numbers without being reseeded
 * for LRNG_DRNG_RESEED_THRESH requests times LRNG_DRNG_MAX_REQSIZE bytes.
 *
 * If the DRNG is not yet initialized, use the initial RNG output.
 *
 * @outbuf: buffer for storing random data
 * @outbuflen: length of outbuf
 * @return: < 0 in error case (DRNG generation or update failed)
 *	    >=0 returning the returned number of bytes
 */
static int lrng_sdrng_get(u8 *outbuf, u32 outbuflen)
{
	u32 processed = 0;
	struct lrng_sdrng *sdrng;
	unsigned long flags;
	int ret;

	if (!outbuf || !outbuflen)
		return 0;

	outbuflen = min_t(size_t, outbuflen, INT_MAX);

	/* DRNG is not yet available */
	if (!atomic_read(&lrng_pdrng_avail)) {
		spin_lock_irqsave(&lrng_init_rng_lock, flags);
		/* Prevent race with lrng_init */
		if (!atomic_read(&lrng_pdrng_avail)) {
			ret = lrng_init_rng(outbuf, outbuflen);
			spin_unlock_irqrestore(&lrng_init_rng_lock, flags);
			return ret;
		}
		spin_unlock_irqrestore(&lrng_init_rng_lock, flags);
	}

	sdrng = lrng_sdrng_get_local();
	while (outbuflen) {
		unsigned long now = jiffies;
		u32 todo = min_t(u32, outbuflen, LRNG_DRNG_MAX_REQSIZE);

		if (sdrng->force_reseed ||
		    atomic_dec_and_test(&sdrng->requests) ||
		    time_after(now, sdrng->last_seeded +
			       lrng_sdrng_reseed_max_time * HZ))
			lrng_sdrng_seed(sdrng, lrng_pdrng_seed);

		spin_lock_irqsave(&sdrng->lock, flags);
		ret = lrng_drng_generate_helper(sdrng->sdrng,
						outbuf + processed, todo);
		spin_unlock_irqrestore(&sdrng->lock, flags);
		if (ret <= 0) {
			pr_warn("getting random data from secondary DRNG "
				"failed (%d)\n", ret);
			return -EFAULT;
		}
		processed += ret;
		outbuflen -= ret;
	}

	return processed;
}

/****************************** DRNG allocation ******************************/

static int lrng_drngs_alloc(void)
{
	unsigned long flags;
	void *pdrng;
	u32 node;
	int ret = 0;

	pdrng = lrng_drng_alloc(LRNG_DRNG_SECURITY_STRENGTH_BYTES);
	if (IS_ERR(pdrng))
		return PTR_ERR(pdrng);

	spin_lock_irqsave(&lrng_pdrng.lock, flags);
	if (lrng_pdrng.pdrng) {
		lrng_drng_dealloc(pdrng);
		kfree(pdrng);
		pdrng = NULL;
	} else {
		lrng_pdrng.pdrng = pdrng;
		INIT_WORK(&lrng_pdrng.lrng_seed_work, lrng_sdrng_seed_work);
		pr_info("primary DRNG allocated\n");
	}
	spin_unlock_irqrestore(&lrng_pdrng.lock, flags);

	lrng_sdrng = kcalloc(nr_node_ids, sizeof(void *),
			     GFP_KERNEL|__GFP_NOFAIL);
	for_each_online_node(node) {
		struct lrng_sdrng *sdrng;

		sdrng = kmalloc_node(sizeof(struct lrng_sdrng),
				     GFP_KERNEL|__GFP_NOFAIL, node);
		memset(sdrng, 0, sizeof(lrng_sdrng));

		sdrng->sdrng =
			lrng_drng_alloc(LRNG_DRNG_SECURITY_STRENGTH_BYTES);
		if (IS_ERR(sdrng->sdrng)) {
			ret = PTR_ERR(sdrng->sdrng);
			kfree(sdrng);
			goto err;
		}

		spin_lock_init(&sdrng->lock);
		atomic_set(&sdrng->requests, LRNG_DRNG_RESEED_THRESH);
		sdrng->last_seeded = jiffies;
		sdrng->fully_seeded = false;
		sdrng->force_reseed = true;

		lrng_sdrng[node] = sdrng;

		lrng_pool.numa_drngs++;
		pr_info("secondary DRNG for NUMA node %d allocated\n", node);
	}

	/* Ensure that all NUMA nodes receive changed memory here. */
	mb();

	return 0;

err:
	for_each_online_node(node) {
		struct lrng_sdrng *sdrng = lrng_sdrng[node];

		if (sdrng) {
			if (sdrng->sdrng)
				lrng_drng_dealloc(sdrng->sdrng);
			kfree(sdrng);
		}
	}
	kfree(lrng_sdrng);

	if (pdrng) {
		lrng_drng_dealloc(pdrng);
		kfree(pdrng);
	}

	return ret;
}

static int lrng_alloc(void)
{
	u8 key[LRNG_DRNG_SECURITY_STRENGTH_BYTES] __aligned(LRNG_KCAPI_ALIGN);
	int ret = lrng_drngs_alloc();

	if (ret)
		return ret;

	lrng_init_rng(key, sizeof(key));
	lrng_pool.lrng_hash = lrng_hash_alloc(key, sizeof(key));
	memzero_explicit(key, sizeof(key));
	return PTR_ERR_OR_ZERO(lrng_pool.lrng_hash);
}

/************************** LRNG kernel interfaces ***************************/

void get_random_bytes(void *buf, int nbytes)
{
	lrng_debug_report_seedlevel("get_random_bytes");
	lrng_sdrng_get((u8 *)buf, (u32)nbytes);
}
EXPORT_SYMBOL(get_random_bytes);

/**
 * Wait for the primary DRNG to be seeded and thus guaranteed to supply
 * cryptographically secure random numbers. This applies to: the /dev/urandom
 * device, the get_random_bytes function, and the get_random_{u32,u64,int,long}
 * family of functions. Using any of these functions without first calling
 * this function forfeits the guarantee of security.
 *
 * Returns: 0 if the primary DRNG has been seeded.
 *          -ERESTARTSYS if the function was interrupted by a signal.
 */
int wait_for_random_bytes(void)
{
	if (likely(lrng_pdrng.pdrng_min_seeded))
		return 0;
	return wait_event_interruptible(lrng_pdrng_init_wait,
					lrng_pdrng.pdrng_min_seeded);
}
EXPORT_SYMBOL(wait_for_random_bytes);

/**
 * This function will use the architecture-specific hardware random
 * number generator if it is available.  The arch-specific hw RNG will
 * almost certainly be faster than what we can do in software, but it
 * is impossible to verify that it is implemented securely (as
 * opposed, to, say, the AES encryption of a sequence number using a
 * key known by the NSA).  So it's useful if we need the speed, but
 * only if we're willing to trust the hardware manufacturer not to
 * have put in a back door.
 *
 * @buf: buffer allocated by caller to store the random data in
 * @nbytes: length of outbuf
 */
void get_random_bytes_arch(void *buf, int nbytes)
{
	u8 *p = buf;

	while (nbytes) {
		unsigned long v;
		int chunk = min_t(int, nbytes, sizeof(unsigned long));

		if (!arch_get_random_long(&v))
			break;

		memcpy(p, &v, chunk);
		p += chunk;
		nbytes -= chunk;
	}

	if (nbytes)
		lrng_sdrng_get((u8 *)p, (u32)nbytes);
}
EXPORT_SYMBOL(get_random_bytes_arch);

/**
 * Interface for in-kernel drivers of true hardware RNGs.
 * Those devices may produce endless random bits and will be throttled
 * when our pool is full.
 *
 * @buffer: buffer holding the entropic data from HW noise sources to be used to
 *	    (re)seed the DRNG.
 * @count: length of buffer
 * @entropy_bits: amount of entropy in buffer (value is in bits)
 */
void add_hwgenerator_randomness(const char *buffer, size_t count,
				size_t entropy_bits)
{
	/* DRNG is not yet online */
	if (!atomic_read(&lrng_pdrng_avail))
		return;
	/*
	 * Suspend writing if we are fully loaded with entropy.
	 * We'll be woken up again once below lrng_write_wakeup_thresh,
	 * or when the calling thread is about to terminate.
	 */
	wait_event_interruptible(lrng_write_wait,
				 kthread_should_stop() || lrng_need_entropy());
	lrng_pdrng_inject(buffer, count, entropy_bits, NULL, 0, false);
}
EXPORT_SYMBOL_GPL(add_hwgenerator_randomness);

/**
 * Delete a previously registered readiness callback function.
 */
void del_random_ready_callback(struct random_ready_callback *rdy)
{
	unsigned long flags;
	struct module *owner = NULL;

	spin_lock_irqsave(&lrng_ready_list_lock, flags);
	if (!list_empty(&rdy->list)) {
		list_del_init(&rdy->list);
		owner = rdy->owner;
	}
	spin_unlock_irqrestore(&lrng_ready_list_lock, flags);

	module_put(owner);
}
EXPORT_SYMBOL(del_random_ready_callback);

/**
 * Add a callback function that will be invoked when the DRNG is fully seeded.
 *
 * @return: 0 if callback is successfully added
 *          -EALREADY if pool is already initialised (callback not called)
 *	    -ENOENT if module for callback is not alive
 */
int add_random_ready_callback(struct random_ready_callback *rdy)
{
	struct module *owner;
	unsigned long flags;
	int err = -EALREADY;

	if (likely(lrng_pdrng.pdrng_min_seeded))
		return err;

	owner = rdy->owner;
	if (!try_module_get(owner))
		return -ENOENT;

	spin_lock_irqsave(&lrng_ready_list_lock, flags);
	if (lrng_pdrng.pdrng_min_seeded)
		goto out;

	owner = NULL;

	list_add(&rdy->list, &lrng_ready_list);
	err = 0;

out:
	spin_unlock_irqrestore(&lrng_ready_list_lock, flags);

	module_put(owner);

	return err;
}
EXPORT_SYMBOL(add_random_ready_callback);

/************************ LRNG user space interfaces *************************/

static ssize_t lrng_read_common(char __user *buf, size_t nbytes,
			int (*lrng_read_random)(u8 *outbuf, u32 outbuflen))
{
	ssize_t ret = 0;
	u8 tmpbuf[LRNG_DRNG_BLOCKSIZE] __aligned(LRNG_KCAPI_ALIGN);
	u8 *tmp_large = NULL;
	u8 *tmp = tmpbuf;
	u32 tmplen = sizeof(tmpbuf);

	if (nbytes == 0)
		return 0;

	/*
	 * Satisfy large read requests -- as the common case are smaller
	 * request sizes, such as 16 or 32 bytes, avoid a kmalloc overhead for
	 * those by using the stack variable of tmpbuf.
	 */
	if (nbytes > sizeof(tmpbuf)) {
		tmplen = min_t(u32, nbytes, LRNG_DRNG_MAX_REQSIZE);
		tmp_large = kmalloc(tmplen + LRNG_KCAPI_ALIGN, GFP_KERNEL);
		if (!tmp_large)
			tmplen = sizeof(tmpbuf);
		else
			tmp = PTR_ALIGN(tmp_large, LRNG_KCAPI_ALIGN);
	}

	while (nbytes) {
		u32 todo = min_t(u32, nbytes, tmplen);
		int rc = 0;

		/* Reschedule if we received a large request. */
		if ((tmp_large) && need_resched()) {
			if (signal_pending(current)) {
				if (ret == 0)
					ret = -ERESTARTSYS;
				break;
			}
			schedule();
		}

		rc = lrng_read_random(tmp, todo);
		if (rc <= 0)
			break;
		if (copy_to_user(buf, tmp, rc)) {
			ret = -EFAULT;
			break;
		}

		nbytes -= rc;
		buf += rc;
		ret += rc;
	}

	/* Wipe data just returned from memory */
	if (tmp_large)
		kzfree(tmp_large);
	else
		memzero_explicit(tmpbuf, sizeof(tmpbuf));

	return ret;
}

static ssize_t
lrng_pdrng_read_common(int nonblock, char __user *buf, size_t nbytes)
{
	if (nbytes == 0)
		return 0;

	nbytes = min_t(u32, nbytes, LRNG_DRNG_BLOCKSIZE);
	while (1) {
		ssize_t n;

		n = lrng_read_common(buf, nbytes, lrng_pdrng_get);
		if (n)
			return n;

		/* No entropy available.  Maybe wait and retry. */
		if (nonblock)
			return -EAGAIN;

		wait_event_interruptible(lrng_read_wait,
					 lrng_have_entropy_full());
		if (signal_pending(current))
			return -ERESTARTSYS;
	}
}

static ssize_t lrng_pdrng_read(struct file *file, char __user *buf,
			       size_t nbytes, loff_t *ppos)
{
	return lrng_pdrng_read_common(file->f_flags & O_NONBLOCK, buf, nbytes);
}

static unsigned int lrng_pdrng_poll(struct file *file, poll_table *wait)
{
	unsigned int mask;

	poll_wait(file, &lrng_read_wait, wait);
	poll_wait(file, &lrng_write_wait, wait);
	mask = 0;
	if (lrng_have_entropy_full())
		mask |= POLLIN | POLLRDNORM;
	if (lrng_need_entropy())
		mask |= POLLOUT | POLLWRNORM;
	return mask;
}

static ssize_t lrng_drng_write_common(const char __user *buffer, size_t count,
				      u32 entropy_bits)
{
	ssize_t ret = 0;
	u8 buf[64] __aligned(LRNG_KCAPI_ALIGN);
	const char __user *p = buffer;
	u32 node, orig_entropy_bits = entropy_bits;

	if (!atomic_read(&lrng_pdrng_avail))
		return -EAGAIN;

	count = min_t(size_t, count, INT_MAX);
	while (count > 0) {
		size_t bytes = min_t(size_t, count, sizeof(buf));
		u32 ent = min_t(u32, bytes<<3, entropy_bits);

		if (copy_from_user(&buf, p, bytes))
			return -EFAULT;
		/* Inject data into primary DRNG */
		lrng_pdrng_inject(buf, bytes, ent, NULL, 0, false);

		count -= bytes;
		p += bytes;
		ret += bytes;
		entropy_bits -= ent;

		cond_resched();
	}

	/*
	 * Force reseed of secondary DRNG during next data request. Data with
	 * entropy is assumed to be intended for the primary DRNG and thus
	 * will not cause a reseed of the secondary DRNGs.
	 */
	if (!orig_entropy_bits) {
		for_each_online_node(node) {
			struct lrng_sdrng *sdrng = lrng_sdrng[node];

			if (!sdrng)
				continue;

			sdrng->force_reseed = true;
			pr_debug("force reseed of secondary DRNG on node %u\n",
				 node);
		}
	}

	return ret;
}

static ssize_t lrng_sdrng_read(struct file *file, char __user *buf,
			       size_t nbytes, loff_t *ppos)
{
	if (!lrng_pdrng.pdrng_min_seeded)
		pr_notice_ratelimited("%s - use of insufficiently seeded DRNG "
				      "(%zu bytes read)\n", current->comm,
				      nbytes);
	else if (!lrng_pdrng.pdrng_fully_seeded)
		pr_debug_ratelimited("%s - use of not fully seeded DRNG (%zu "
				     "bytes read)\n", current->comm, nbytes);

	return lrng_read_common(buf, nbytes, lrng_sdrng_get);
}

static ssize_t lrng_drng_write(struct file *file, const char __user *buffer,
			       size_t count, loff_t *ppos)
{
	return lrng_drng_write_common(buffer, count, 0);
}

static long lrng_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int size, ent_count_bits;
	int __user *p = (int __user *)arg;

	switch (cmd) {
	case RNDGETENTCNT:
		ent_count_bits = lrng_avail_entropy();
		if (put_user(ent_count_bits, p))
			return -EFAULT;
		return 0;
	case RNDADDTOENTCNT:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count_bits, p))
			return -EFAULT;
		ent_count_bits = (int)lrng_avail_entropy() + ent_count_bits;
		if (ent_count_bits < 0)
			ent_count_bits = 0;
		if (ent_count_bits > LRNG_POOL_SIZE_BITS)
			ent_count_bits = LRNG_POOL_SIZE_BITS;
		atomic_set(&lrng_pool.irq_info.num_events,
			   lrng_entropy_to_data(ent_count_bits));
		return 0;
	case RNDADDENTROPY:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count_bits, p++))
			return -EFAULT;
		if (ent_count_bits < 0)
			return -EINVAL;
		if (get_user(size, p++))
			return -EFAULT;
		if (size < 0)
			return -EINVAL;
		/* there cannot be more entropy than data */
		ent_count_bits = min(ent_count_bits, size<<3);
		return lrng_drng_write_common((const char __user *)p, size,
					      ent_count_bits);
	case RNDZAPENTCNT:
	case RNDCLEARPOOL:
		/* Clear the entropy pool counter. */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		atomic_set(&lrng_pool.irq_info.num_events, 0);
		return 0;
	default:
		return -EINVAL;
	}
}

static int lrng_fasync(int fd, struct file *filp, int on)
{
	return fasync_helper(fd, filp, on, &fasync);
}

const struct file_operations random_fops = {
	.read  = lrng_pdrng_read,
	.write = lrng_drng_write,
	.poll  = lrng_pdrng_poll,
	.unlocked_ioctl = lrng_ioctl,
	.fasync = lrng_fasync,
	.llseek = noop_llseek,
};

const struct file_operations urandom_fops = {
	.read  = lrng_sdrng_read,
	.write = lrng_drng_write,
	.unlocked_ioctl = lrng_ioctl,
	.fasync = lrng_fasync,
	.llseek = noop_llseek,
};

SYSCALL_DEFINE3(getrandom, char __user *, buf, size_t, count,
		unsigned int, flags)
{
	if (flags & ~(GRND_NONBLOCK|GRND_RANDOM))
		return -EINVAL;

	if (count > INT_MAX)
		count = INT_MAX;

	if (flags & GRND_RANDOM)
		return lrng_pdrng_read_common(flags & GRND_NONBLOCK, buf,
					      count);

	if (unlikely(!lrng_pdrng.pdrng_fully_seeded)) {
		int ret;

		if (flags & GRND_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(lrng_pdrng_init_wait,
					       lrng_pdrng.pdrng_fully_seeded);
		if (unlikely(ret))
			return ret;
	}

	return lrng_sdrng_read(NULL, buf, count, NULL);
}

/*************************** LRNG proc interfaces ****************************/

#ifdef CONFIG_SYSCTL

#include <linux/sysctl.h>

static int lrng_min_read_thresh = LRNG_POOL_WORD_BITS;
static int lrng_min_write_thresh;
static int lrng_max_read_thresh = LRNG_POOL_SIZE_BITS;
static int lrng_max_write_thresh = LRNG_POOL_SIZE_BITS;
static char lrng_sysctl_bootid[16];
static int lrng_sdrng_reseed_max_min;

/*
 * This function is used to return both the bootid UUID, and random
 * UUID.  The difference is in whether table->data is NULL; if it is,
 * then a new UUID is generated and returned to the user.
 *
 * If the user accesses this via the proc interface, the UUID will be
 * returned as an ASCII string in the standard UUID format; if via the
 * sysctl system call, as 16 bytes of binary data.
 */
static int lrng_proc_do_uuid(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table fake_table;
	unsigned char buf[64], tmp_uuid[16], *uuid;

	uuid = table->data;
	if (!uuid) {
		uuid = tmp_uuid;
		generate_random_uuid(uuid);
	} else {
		static DEFINE_SPINLOCK(bootid_spinlock);

		spin_lock(&bootid_spinlock);
		if (!uuid[8])
			generate_random_uuid(uuid);
		spin_unlock(&bootid_spinlock);
	}

	sprintf(buf, "%pU", uuid);

	fake_table.data = buf;
	fake_table.maxlen = sizeof(buf);

	return proc_dostring(&fake_table, write, buffer, lenp, ppos);
}

static int lrng_proc_do_type(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table fake_table;
	unsigned char buf[150];

	snprintf(buf, sizeof(buf),
		 "DRNG security strength: %u bits\n"
		 "number of secondary DRNG instances: %u",
		 LRNG_DRNG_SECURITY_STRENGTH_BITS, lrng_pool.numa_drngs);

	fake_table.data = buf;
	fake_table.maxlen = sizeof(buf);

	return proc_dostring(&fake_table, write, buffer, lenp, ppos);
}

/* Return entropy available scaled to integral bits */
static int lrng_proc_do_entropy(struct ctl_table *table, int write,
				void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table fake_table;
	int entropy_count;

	entropy_count = lrng_avail_entropy();

	fake_table.data = &entropy_count;
	fake_table.maxlen = sizeof(entropy_count);

	return proc_dointvec(&fake_table, write, buffer, lenp, ppos);
}

static int lrng_proc_bool(struct ctl_table *table, int write,
			  void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table fake_table;
	int loc_boolean = 0;
	bool *boolean = (bool *)table->data;

	if (*boolean)
		loc_boolean = 1;

	fake_table.data = &loc_boolean;
	fake_table.maxlen = sizeof(loc_boolean);

	return proc_dointvec(&fake_table, write, buffer, lenp, ppos);
}

static int lrng_sysctl_poolsize = LRNG_POOL_SIZE_BITS;
static int pdrng_security_strength = LRNG_DRNG_SECURITY_STRENGTH_BYTES;
extern struct ctl_table random_table[];
struct ctl_table random_table[] = {
	{
		.procname	= "poolsize",
		.data		= &lrng_sysctl_poolsize,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "entropy_avail",
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= lrng_proc_do_entropy,
	},
	{
		.procname	= "read_wakeup_threshold",
		.data		= &lrng_read_wakeup_bits,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &lrng_min_read_thresh,
		.extra2		= &lrng_max_read_thresh,
	},
	{
		.procname	= "write_wakeup_threshold",
		.data		= &lrng_write_wakeup_bits,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &lrng_min_write_thresh,
		.extra2		= &lrng_max_write_thresh,
	},
	{
		.procname	= "boot_id",
		.data		= &lrng_sysctl_bootid,
		.maxlen		= 16,
		.mode		= 0444,
		.proc_handler	= lrng_proc_do_uuid,
	},
	{
		.procname	= "uuid",
		.maxlen		= 16,
		.mode		= 0444,
		.proc_handler	= lrng_proc_do_uuid,
	},
	{
		.procname       = "urandom_min_reseed_secs",
		.data           = &lrng_sdrng_reseed_max_time,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec,
		.extra1		= &lrng_sdrng_reseed_max_min,
	},
	{
		.procname	= "drng_fully_seeded",
		.data		= &lrng_pdrng.pdrng_fully_seeded,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= lrng_proc_bool,
	},
	{
		.procname	= "drng_minimally_seeded",
		.data		= &lrng_pdrng.pdrng_min_seeded,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= lrng_proc_bool,
	},
	{
		.procname	= "lrng_type",
		.maxlen		= 30,
		.mode		= 0444,
		.proc_handler	= lrng_proc_do_type,
	},
	{
		.procname	= "drng_security_strength",
		.data		= &pdrng_security_strength,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "high_resolution_timer",
		.data		= &lrng_pool.irq_info.irq_highres_timer,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= lrng_proc_bool,
	},
	{ }
};
#endif /* CONFIG_SYSCTL */

/************************ LRNG auxiliary interfaces **************************/

struct batched_entropy {
	union {
		u64 entropy_u64[LRNG_DRNG_BLOCKSIZE / sizeof(u64)];
		u32 entropy_u32[LRNG_DRNG_BLOCKSIZE / sizeof(u32)];
	};
	unsigned int position;
};

static rwlock_t
batched_entropy_reset_lock = __RW_LOCK_UNLOCKED(batched_entropy_reset_lock);

/*
 * Get a random word for internal kernel use only. The quality of the random
 * number is either as good as RDRAND or as good as /dev/urandom, with the
 * goal of being quite fast and not depleting entropy.
 */
static DEFINE_PER_CPU(struct batched_entropy, batched_entropy_u64);
u64 get_random_u64(void)
{
	u64 ret;
	bool use_lock = !lrng_pdrng.pdrng_fully_seeded;
	unsigned long flags = 0;
	struct batched_entropy *batch;

#if BITS_PER_LONG == 64
	if (arch_get_random_long((unsigned long *)&ret))
		return ret;
#else
	if (arch_get_random_long((unsigned long *)&ret) &&
	    arch_get_random_long((unsigned long *)&ret + 1))
		return ret;
#endif

	lrng_debug_report_seedlevel("get_random_u64");

	batch = &get_cpu_var(batched_entropy_u64);
	if (use_lock)
		read_lock_irqsave(&batched_entropy_reset_lock, flags);
	if (batch->position % ARRAY_SIZE(batch->entropy_u64) == 0) {
		lrng_sdrng_get((u8 *)batch->entropy_u64, LRNG_DRNG_BLOCKSIZE);
		batch->position = 0;
	}
	ret = batch->entropy_u64[batch->position++];
	if (use_lock)
		read_unlock_irqrestore(&batched_entropy_reset_lock, flags);
	put_cpu_var(batched_entropy_u64);
	return ret;
}
EXPORT_SYMBOL(get_random_u64);

static DEFINE_PER_CPU(struct batched_entropy, batched_entropy_u32);
u32 get_random_u32(void)
{
	u32 ret;
	bool use_lock = !lrng_pdrng.pdrng_fully_seeded;
	unsigned long flags = 0;
	struct batched_entropy *batch;

	if (arch_get_random_int(&ret))
		return ret;

	lrng_debug_report_seedlevel("get_random_u32");

	batch = &get_cpu_var(batched_entropy_u32);
	if (use_lock)
		read_lock_irqsave(&batched_entropy_reset_lock, flags);
	if (batch->position % ARRAY_SIZE(batch->entropy_u32) == 0) {
		lrng_sdrng_get((u8 *)batch->entropy_u32, LRNG_DRNG_BLOCKSIZE);
		batch->position = 0;
	}
	ret = batch->entropy_u32[batch->position++];
	if (use_lock)
		read_unlock_irqrestore(&batched_entropy_reset_lock, flags);
	put_cpu_var(batched_entropy_u32);
	return ret;
}
EXPORT_SYMBOL(get_random_u32);

/*
 * It's important to invalidate all potential batched entropy that might
 * be stored before the crng is initialized, which we can do lazily by
 * simply resetting the counter to zero so that it's re-extracted on the
 * next usage.
 */
static void invalidate_batched_entropy(void)
{
	int cpu;
	unsigned long flags;

	write_lock_irqsave(&batched_entropy_reset_lock, flags);
	for_each_possible_cpu(cpu) {
		per_cpu_ptr(&batched_entropy_u32, cpu)->position = 0;
		per_cpu_ptr(&batched_entropy_u64, cpu)->position = 0;
	}
	write_unlock_irqrestore(&batched_entropy_reset_lock, flags);
}

/**
 * randomize_page - Generate a random, page aligned address
 * @start:	The smallest acceptable address the caller will take.
 * @range:	The size of the area, starting at @start, within which the
 *		random address must fall.
 *
 * If @start + @range would overflow, @range is capped.
 *
 * NOTE: Historical use of randomize_range, which this replaces, presumed that
 * @start was already page aligned.  We now align it regardless.
 *
 * Return: A page aligned address within [start, start + range).  On error,
 * @start is returned.
 */
unsigned long
randomize_page(unsigned long start, unsigned long range)
{
	if (!PAGE_ALIGNED(start)) {
		range -= PAGE_ALIGN(start) - start;
		start = PAGE_ALIGN(start);
	}

	if (start > ULONG_MAX - range)
		range = ULONG_MAX - start;

	range >>= PAGE_SHIFT;

	if (range == 0)
		return start;

	return start + (get_random_long() % range << PAGE_SHIFT);
}

/***************************** Initialize LRNG *******************************/

static int __init lrng_init(void)
{
	unsigned long flags;

	BUG_ON(lrng_alloc());

	spin_lock_irqsave(&lrng_init_rng_lock, flags);

	if (random_get_entropy() || random_get_entropy()) {
		/*
		 * As the highres timer is identified here, previous interrupts
		 * obtained during boot time are treated like a lowres timer
		 * would have been present.
		 */
		lrng_pool.irq_info.irq_highres_timer = true;
		lrng_pool.irq_info.irq_entropy_bits = LRNG_IRQ_ENTROPY_BITS;
	} else {
		lrng_pool.irq_info.stuck_test = false;
		lrng_pool.irq_info.irq_entropy_bits =
			LRNG_IRQ_ENTROPY_BITS * LRNG_IRQ_OVERSAMPLING_FACTOR;
		pr_warn("operating without high-resolution timer and applying "
			"IRQ oversampling factor %u\n",
			LRNG_IRQ_OVERSAMPLING_FACTOR);
	}
	lrng_set_entropy_thresh(LRNG_INIT_ENTROPY_BITS);

	/*
	 * As we use the IRQ entropic input data processed by the init RNG again
	 * during lrng_pdrng_seed, we must not claim that the init RNG state has
	 * any entropy when using it as an initial seed to the DRNG.
	 */
	lrng_pdrng_inject((u8 *)&lrng_init_state,
			  SHA_WORKSPACE_WORDS * sizeof(lrng_init_state[0]),
			  0, NULL, 0, false);
	lrng_sdrng_seed(lrng_sdrng[0], lrng_pdrng_seed);
	atomic_inc(&lrng_pdrng_avail);
	memzero_explicit(&lrng_init_state,
			 SHA_WORKSPACE_WORDS * sizeof(lrng_init_state[0]));
	spin_unlock_irqrestore(&lrng_init_rng_lock, flags);
	pr_info("deactivating initial RNG - %d bytes delivered\n",
		atomic_read(&lrng_initrng_bytes));
	return 0;
}

/* A late init implies that more interrupts are collected for initial seeding */
late_initcall(lrng_init);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Linux Random Number Generator");
