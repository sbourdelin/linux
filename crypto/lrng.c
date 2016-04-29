/*
 * Linux Random Number Generator (LRNG)
 *
 * Documentation and test code: http://www.chronox.de/lrng.html
 *
 * Copyright (C) 2016, Stephan Mueller <smueller@chronox.de>
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

#include <crypto/drbg.h>

/*
 * Define one DRBG out of each type with 256 bits of security strength.
 *
 * This definition is allowed to be changed.
 */
#ifdef CONFIG_CRYPTO_DRBG_HMAC
# if 0
#  define LRNG_DRBG_BLOCKLEN_BYTES 64
#  define LRNG_DRBG_SECURITY_STRENGTH_BYTES 32
#  define LRNG_DRBG_CORE "drbg_nopr_hmac_sha512"	/* HMAC DRBG SHA-512 */
# else
#  define LRNG_DRBG_BLOCKLEN_BYTES 32
#  define LRNG_DRBG_SECURITY_STRENGTH_BYTES 32
#  define LRNG_DRBG_CORE "drbg_nopr_hmac_sha256"	/* HMAC DRBG SHA-256 */
# endif
#elif defined CONFIG_CRYPTO_DRBG_HASH
# if 0
#  define LRNG_DRBG_BLOCKLEN_BYTES 64
#  define LRNG_DRBG_SECURITY_STRENGTH_BYTES 32
#  define LRNG_DRBG_CORE "drbg_nopr_sha512"		/* Hash DRBG SHA-512 */
# else
#  define LRNG_DRBG_BLOCKLEN_BYTES 32
#  define LRNG_DRBG_SECURITY_STRENGTH_BYTES 32
#  define LRNG_DRBG_CORE "drbg_nopr_sha256"		/* Hash DRBG SHA-256 */
# endif
#elif defined CONFIG_CRYPTO_DRBG_CTR
# define LRNG_DRBG_BLOCKLEN_BYTES 16
# define LRNG_DRBG_SECURITY_STRENGTH_BYTES 32
# define LRNG_DRBG_CORE "drbg_nopr_ctr_aes256"		/* CTR DRBG AES-256 */
#else
# error "LRNG requires the presence of a DRBG"
#endif

/* Primary DRBG state handle */
struct lrng_pdrbg {
	struct drbg_state *pdrbg;	/* DRBG handle */
	bool pdrbg_fully_seeded;	/* Is DRBG fully seeded? */
	bool pdrbg_min_seeded;		/* Is DRBG minimally seeded? */
	u32 pdrbg_entropy_bits;		/* Is DRBG entropy level */
	struct work_struct lrng_seed_work;	/* (re)seed work queue */
	spinlock_t lock;
};

/* Secondary DRBG state handle */
struct lrng_sdrbg {
	struct drbg_state *sdrbg;	/* DRBG handle */
	atomic_t requests;		/* Number of DRBG requests */
	unsigned long last_seeded;	/* Last time it was seeded */
	bool fully_seeded;		/* Is DRBG fully seeded? */
	spinlock_t lock;
};

#define LRNG_DRBG_BLOCKLEN_BITS (LRNG_DRBG_BLOCKLEN_BYTES * 8)
#define LRNG_DRBG_SECURITY_STRENGTH_BITS (LRNG_DRBG_SECURITY_STRENGTH_BYTES * 8)

/*
 * SP800-90A defines a maximum request size of 1<<16 bytes. The given value is
 * considered a safer margin. This applies to secondary DRBG.
 *
 * This value is allowed to be changed.
 */
#define LRNG_DRBG_MAX_REQSIZE (1<<12)

/*
 * SP800-90A defines a maximum number of requests between reseeds of 1<<48.
 * The given value is considered a much safer margin, balancing requests for
 * frequent reseeds with the need to conserve entropy. This value MUST NOT be
 * larger than INT_MAX because it is used in an atomic_t. This applies to
 * secondary DRBG.
 *
 * This value is allowed to be changed.
 */
#define LRNG_DRBG_RESEED_THRESH (1<<12)

/* Status information about IRQ noise source */
struct lrng_irq_info {
	atomic_t num_events;	/* Number of non-stuck IRQs since last read */
	atomic_t num_events_thresh;	/* Reseed threshold */
	atomic_t pool_ptr;	/* Ptr into pool for next IRQ bit injection */
	u32 irq_pool_reader;	/* Current word of pool to be read */
	atomic_t last_time;	/* Stuck test: time of previous IRQ */
	atomic_t last_delta;	/* Stuck test: delta of previous IRQ */
	atomic_t last_delta2;	/* Stuck test: 2. time derivation of prev IRQ */
	atomic_t reseed_in_progress;	/* Flag for on executing reseed */
	atomic_t crngt_ctr;	/* FIPS 140-2 CRNGT counter */
	bool irq_highres_timer;	/* Is high-resolution timer available? */
};

/*
 * According to FIPS 140-2 IG 9.8, our C threshold is at 3 back to back stuck
 * values. It should be highly unlikely that we see three consecutive
 * identical time stamps.
 *
 * This value is allowed to be changed.
 */
#define LRNG_FIPS_CRNGT 3

/*
 * This is the entropy pool used by the slow noise source. Its size should
 * be at least as large as the interrupt entropy estimate.
 *
 * LRNG_POOL_SIZE is allowed to be changed.
 */
struct lrng_pool {
#define LRNG_POOL_SIZE 128
#define LRNG_POOL_WORD_BYTES (sizeof(atomic_t))
#define LRNG_POOL_SIZE_BYTES (LRNG_POOL_SIZE * LRNG_POOL_WORD_BYTES)
#define LRNG_POOL_SIZE_BITS (LRNG_POOL_SIZE_BYTES * 8)
#define LRNG_POOL_WORD_BITS (LRNG_POOL_WORD_BYTES * 8)
	atomic_t pool[LRNG_POOL_SIZE];	/* Pool holing the slow noise */
	struct lrng_irq_info irq_info;	/* IRQ noise source status info */
	u32 last_numa_node;	/* Last NUMA node */
};

/*
 * Number of interrupts to be recorded to assume that DRBG security strength
 * bits of entropy are received. Dividing a DRBG security strength of 256 by
 * this value gives the entropy value for one interrupt (i.e. a folded one bit).
 * The default value implies that we have 256 / 288 = 0.89 bits of entropy per
 * interrupt.
 * Note: a value below the DRBG security strength should not be defined as this
 *	 may imply the DRBG can never be fully seeded in case other noise
 *	 sources are unavailable.
 * Note 2: This value must be multiples of LRNG_POOL_WORD_BYTES
 *
 * This value is allowed to be changed.
 */
#define LRNG_IRQ_ENTROPY_BYTES \
	(LRNG_DRBG_SECURITY_STRENGTH_BYTES + LRNG_POOL_WORD_BYTES)
#define LRNG_IRQ_ENTROPY_BITS (LRNG_IRQ_ENTROPY_BYTES * 8)

/*
 * Leave given amount of entropy in bits entropy pool to serve /dev/random while
 * /dev/urandom is stressed.
 *
 * This value is allowed to be changed.
 */
#define LRNG_EMERG_ENTROPY (LRNG_DRBG_SECURITY_STRENGTH_BITS * 2)

/*
 * Min required seed entropy is 112 bits as per FIPS 140-2 and AIS20/31.
 *
 * This value is allowed to be changed.
 */
#define LRNG_MIN_SEED_ENTROPY_BITS 112

/*
 * LRNG_MIN_SEED_ENTROPY_BITS rounded up to next LRNG_POOL_WORD multiple.
 *
 * This value must be changed with the following considerations:
 * If LRNG_MIN_SEED_ENTROPY_BITS, LRNG_IRQ_ENTROPY_BITS or LRNG_POOL_WORD_BITS
 * is changed, make sure LRNG_IRQ_MIN_NUM is changed such that
 * (LRNG_MIN_SEED_ENTROPY_BITS * LRNG_IRQ_ENTROPY_BITS /
 * LRNG_DRBG_SECURITY_STRENGTH_BITS) is rounded up to the next full
 * LRNG_POOL_WORD_BITS multiple.
 */
#define LRNG_IRQ_MIN_NUM (LRNG_POOL_WORD_BITS * 4)

/*
 * Oversampling factor of IRQ events to obtain
 * LRNG_DRBG_SECURITY_STRENGTH_BYTES. This factor is used when a
 * high-resolution time stamp is not available. In this case, jiffies and
 * register contents are used to fill the entropy pool. These noise sources
 * are much less entropic than the high-resolution timer. The entropy content
 * is the entropy content assumed with LRNG_IRQ_ENTROPY_BYTES divided by
 * LRNG_IRQ_OVERSAMPLING_FACTOR.
 *
 * This value is allowed to be changed. Note,
 * LRNG_IRQ_ENTROPY_BYTES * LRNG_IRQ_OVERSAMPLING_FACTOR must be smaller than
 * LRNG_POOL_SIZE_BYTES.
 */
#define LRNG_IRQ_OVERSAMPLING_FACTOR 3

static struct lrng_pdrbg lrng_pdrbg = {
	.lock = __SPIN_LOCK_UNLOCKED(lrng.pdrbg.lock)
};

static struct lrng_sdrbg **lrng_sdrbg __read_mostly;

static struct lrng_pool lrng_pool = {
	.irq_info = {
		.num_events		= ATOMIC_INIT(0),
		.num_events_thresh	= ATOMIC_INIT(LRNG_POOL_WORD_BITS),
		.pool_ptr		= ATOMIC_INIT(0),
		.irq_pool_reader	= 0,
		.last_time		= ATOMIC_INIT(0),
		.last_delta		= ATOMIC_INIT(0),
		.last_delta2		= ATOMIC_INIT(0),
		.reseed_in_progress	= ATOMIC_INIT(0),
		.crngt_ctr		= ATOMIC_INIT(LRNG_FIPS_CRNGT),
	},
};

static LIST_HEAD(lrng_ready_list);
static DEFINE_SPINLOCK(lrng_ready_list_lock);

static atomic_t lrng_pdrbg_avail = ATOMIC_INIT(0);
static atomic_t lrng_initrng_bytes = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(lrng_init_rng_lock);	/* Lock the init RNG state */

static DECLARE_WAIT_QUEUE_HEAD(lrng_read_wait);
static DECLARE_WAIT_QUEUE_HEAD(lrng_write_wait);
static DECLARE_WAIT_QUEUE_HEAD(lrng_pdrbg_init_wait);
static struct fasync_struct *fasync;

/*
 * Estimated entropy of data is a 32th of LRNG_DRBG_SECURITY_STRENGTH_BITS.
 * As we have no ability to review the implementation of those noise sources,
 * it is prudent to have a conservative estimate here.
 */
static u32 archrandom = LRNG_DRBG_SECURITY_STRENGTH_BITS>>5;
module_param(archrandom, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(archrandom, "Entropy in bits of 256 data bits from CPU noise source (e.g. RDRAND)");

/*
 * If the entropy count falls under this number of bits, then we
 * should wake up processes which are selecting or polling on write
 * access to /dev/random.
 * The value is set to a fourth of the LRNG_POOL_SIZE_BITS.
 */
static u32 lrng_write_wakeup_bits = LRNG_POOL_SIZE_BITS / 4;

/*
 * The minimum number of bits of entropy before we wake up a read on
 * /dev/random.  Should be enough to do a significant reseed where
 * it is technically possible that the entropy estimate is to be above the
 * DRBG security strength.
 */
static u32 lrng_read_wakeup_bits = LRNG_IRQ_ENTROPY_BITS;

/*
 * Maximum number of seconds between DRBG reseed intervals of the secondary
 * DRBG. Note, this is enforced with the next request of random numbers from
 * the secondary DRBG. Setting this value to zero implies a reseeding attempt
 * before every generated random number.
 */
static int lrng_sdrbg_reseed_max_time = 600;

/********************************** Helper ***********************************/

static inline u32 atomic_read_u32(atomic_t *v)
{
	return (u32)atomic_read(v);
}

static inline u32 atomic_xchg_u32(atomic_t *v, u32 x)
{
	return (u32)atomic_xchg(v, x);
}

/* Is the entropy pool fill level too low and is the DRBG not fully seeded? */
static inline bool lrng_need_entropy(void)
{
	return ((atomic_read_u32(&lrng_pool.irq_info.num_events) <
		 lrng_write_wakeup_bits) &&
		lrng_pdrbg.pdrbg_entropy_bits <
					LRNG_DRBG_SECURITY_STRENGTH_BITS);
}

/* Is the entropy pool filled for /dev/random pull or DRBG fully seeded? */
static inline bool lrng_have_entropy_full(void)
{
	return ((atomic_read_u32(&lrng_pool.irq_info.num_events) >=
		 lrng_read_wakeup_bits) ||
		lrng_pdrbg.pdrbg_entropy_bits >=
					LRNG_DRBG_SECURITY_STRENGTH_BITS);
}

/*********************** Fast soise source processing ************************/

/**
 * Get CPU noise source entropy
 *
 * @outbuf buffer to store entropy of size LRNG_DRBG_SECURITY_STRENGTH_BYTES
 * @return > 0 on success where value provides the added entropy in bits
 *	   0 if no fast source was available
 */
static inline u32 lrng_get_arch(u8 *outbuf)
{
	u32 i;
	u32 ent_bits = archrandom;

	/* operate on full blocks */
	BUILD_BUG_ON(LRNG_DRBG_SECURITY_STRENGTH_BYTES % sizeof(unsigned long));

	if (!ent_bits)
		return 0;

	for (i = 0;
	     i < (LRNG_DRBG_SECURITY_STRENGTH_BYTES / sizeof(unsigned long));
	     i += 2) {
		if (!arch_get_random_long((unsigned long *)outbuf)) {
			archrandom = 0;
			return 0;
		}
		outbuf += sizeof(unsigned long);
	}

	/* Obtain entropy statement -- cap entropy to buffer size in bits */
	ent_bits = min_t(u32, ent_bits, LRNG_DRBG_SECURITY_STRENGTH_BITS);
	pr_debug("obtained %u bits of entropy from CPU RNG noise source\n",
		 ent_bits);
	return ent_bits;
}

/************************ Slow noise source processing ************************/

/**
 * Hot code path - This function XORs all bits with each other. Effectively
 * it calculates the parity of the given value.
 *
 * The implementation is taken from
 * https://graphics.stanford.edu/~seander/bithacks.html
 *
 * @x value to be collapsed to one bit
 * @return collapsed value
 */
static inline u32 lrng_xor_all_bits(u32 x)
{
	x ^= x >> 1;
	x ^= x >> 2;
	x = (x & 0x11111111U) * 0x11111111U;

	return (x >> 28) & 1;
}

/**
 * Hot code path - Stuck test by checking the:
 *      1st derivation of the event occurrence (time delta)
 *      2nd derivation of the event occurrence (delta of time deltas)
 *      3rd derivation of the event occurrence (delta of delta of time deltas)
 *
 * All values must always be non-zero. This is also the FIPS 140-2 CRNGT.
 *
 * @irq_info Reference to IRQ information
 * @now Event time
 * @return 0 event occurrence not stuck (good bit)
 *	   1 event occurrence stuck (reject bit)
 */
static int lrng_irq_stuck(struct lrng_irq_info *irq_info, u32 now_time)
{
	u32 delta = now_time - atomic_xchg_u32(&irq_info->last_time, now_time);
	int delta2 = delta - atomic_xchg_u32(&irq_info->last_delta, delta);
	int delta3 = delta2 - atomic_xchg(&irq_info->last_delta2, delta2);

#ifdef CONFIG_CRYPTO_FIPS
	if (fips_enabled) {
		if (!delta) {
			if (atomic_dec_and_test(&irq_info->crngt_ctr))
				panic("FIPS 140-2 continuous random number generator test failed\n");
		} else
			atomic_set(&irq_info->crngt_ctr, LRNG_FIPS_CRNGT);
	}
#endif

	if (!delta || !delta2 || !delta3)
		return 1;

	return 0;
}

/**
 * Hot code path - mix bit into entropy pool
 */
static inline void lrng_mixin_bit(u32 folded_bit, u32 pool_ptr, u32 irq_num)
{
	pool_ptr %= LRNG_POOL_SIZE_BITS;

	/*
	 * Mix in the folded bit into the bit location pointed to by
	 * pool_ptr. The bit location is calculated by finding the right
	 * pool word (pool_ptr / LRNG_POOL_WORD_BITS) and the right bit
	 * location in the word where the bit should be XORed into
	 * (pool_ptr % LRNG_POOL_WORD_BITS).
	 */
	atomic_xor(folded_bit << (pool_ptr % LRNG_POOL_WORD_BITS),
		   &lrng_pool.pool[pool_ptr / LRNG_POOL_WORD_BITS]);

	/* Should we wake readers? */
	if (irq_num == lrng_read_wakeup_bits) {
		wake_up_interruptible(&lrng_read_wait);
		kill_fasync(&fasync, SIGIO, POLL_IN);
	}

	/* Only try to reseed if the DRBG is alive. */
	if (!atomic_read(&lrng_pdrbg_avail))
		return;

	/*
	 * Once all secondary DRBGs are fully seeded, the interrupt noise
	 * sources will not trigger any reseeding any more.
	 */
	if (lrng_sdrbg[lrng_pool.last_numa_node]->fully_seeded)
		return;

	/* Only trigger the DRBG reseed if we have collected enough IRQs. */
	if (atomic_read_u32(&lrng_pool.irq_info.num_events) <
	    atomic_read_u32(&lrng_pool.irq_info.num_events_thresh))
		return;

	/* Ensure that the seeding only occurs once at any given time. */
	if (atomic_cmpxchg(&lrng_pool.irq_info.reseed_in_progress, 0, 1))
		return;

	/* Seed the DRBG with IRQ noise. */
	schedule_work(&lrng_pdrbg.lrng_seed_work);
}

/**
 * Hot code path - IRQ handler for systems without fast noise sources.
 */
static u32 lrng_irq_value_process(int irq, int irq_flags)
{
	u32 folded_bit = lrng_xor_all_bits(jiffies);
	struct pt_regs *regs = get_irq_regs();
	static unsigned short reg_idx = 0;

	/* Add IRQ number to folded bit */
	folded_bit ^= lrng_xor_all_bits(irq);
	/* Add IRQ flags to folded bit */
	folded_bit ^= lrng_xor_all_bits(irq_flags);

	if (regs) {
		u32 *ptr = (u32 *)regs;
		u64 ip = instruction_pointer(regs);

		/* Add instruction pointer to folded bit */
		folded_bit ^= lrng_xor_all_bits(ip);
		folded_bit ^= lrng_xor_all_bits(ip>>32);

		/* Add one register to folded bit */
		if (reg_idx >= sizeof(struct pt_regs) / sizeof(u32))
			reg_idx = 0;
		folded_bit ^= lrng_xor_all_bits(*(ptr + reg_idx++));
	}

	return folded_bit;
}

/**
 * Hot code path - Callback for interrupt handler
 */
void add_interrupt_randomness(int irq, int irq_flags)
{
	u32 now_time = random_get_entropy();
	struct lrng_irq_info *irq_info = &lrng_pool.irq_info;
	u32 folded_bit, pool_ptr, irq_num;

	if (now_time || lrng_pool.irq_info.irq_highres_timer) {
		if (lrng_irq_stuck(irq_info, now_time))
			return;
		folded_bit = lrng_xor_all_bits(now_time);
	} else
		folded_bit = lrng_irq_value_process(irq, irq_flags);

	/*
	 * If obtained measurement is not stuck, advance pool_ptr to XOR
	 * new folded bit into new location.
	 */
	pool_ptr = (u32)atomic_add_return(1, &irq_info->pool_ptr);
	irq_num = (u32)atomic_add_return(1, &irq_info->num_events);

	lrng_mixin_bit(folded_bit, pool_ptr, irq_num);
}
EXPORT_SYMBOL(add_interrupt_randomness);

/**
 * Callback for HID layer
 */
void add_input_randomness(unsigned int type, unsigned int code,
			  unsigned int value)
{
	static unsigned char last_value;
	u32 folded_bit, pool_ptr;

	/* ignore autorepeat and the like */
	if (value == last_value)
		return;

	last_value = value;

	folded_bit =
		lrng_xor_all_bits((type << 4) ^ code ^ (code >> 4) ^ value);
	pool_ptr = atomic_read_u32(&lrng_pool.irq_info.pool_ptr);

	lrng_mixin_bit(folded_bit, pool_ptr, 0);
}
EXPORT_SYMBOL_GPL(add_input_randomness);

static inline u32 lrng_irq_entropy_bytes(void)
{
	/* entropy pool is read word-wise */
	BUILD_BUG_ON(LRNG_IRQ_ENTROPY_BYTES % LRNG_POOL_WORD_BYTES);
	BUILD_BUG_ON(LRNG_IRQ_ENTROPY_BYTES > LRNG_POOL_SIZE_BYTES);
	BUILD_BUG_ON((LRNG_IRQ_ENTROPY_BYTES * LRNG_IRQ_OVERSAMPLING_FACTOR) >
		     LRNG_POOL_SIZE_BYTES);

	if (lrng_pool.irq_info.irq_highres_timer)
		return LRNG_IRQ_ENTROPY_BYTES;
	else
		return (LRNG_IRQ_ENTROPY_BYTES * LRNG_IRQ_OVERSAMPLING_FACTOR);
}

static inline u32 lrng_irq_entropy_bits(void)
{
	BUILD_BUG_ON((LRNG_MIN_SEED_ENTROPY_BITS * LRNG_IRQ_ENTROPY_BITS /
		     LRNG_DRBG_SECURITY_STRENGTH_BITS) > LRNG_IRQ_MIN_NUM);

	if (lrng_pool.irq_info.irq_highres_timer)
		return LRNG_IRQ_ENTROPY_BITS;
	else
		return (LRNG_IRQ_ENTROPY_BITS * LRNG_IRQ_OVERSAMPLING_FACTOR);
}

static inline u32 lrng_irq_min_num(void)
{
	if (lrng_pool.irq_info.irq_highres_timer)
		return LRNG_IRQ_MIN_NUM;
	else
		return (LRNG_IRQ_MIN_NUM * LRNG_IRQ_OVERSAMPLING_FACTOR);
}

static inline u32 lrng_entropy_to_irqnum(u32 entropy_bits)
{
	return ((entropy_bits * lrng_irq_entropy_bits()) /
		LRNG_DRBG_SECURITY_STRENGTH_BITS);
}

static inline u32 lrng_irqnum_to_entropy(u32 irqnum)
{
	return ((irqnum * LRNG_DRBG_SECURITY_STRENGTH_BITS) /
		lrng_irq_entropy_bits());
}

/**
 * Read the entropy pool out for use. The caller must ensure this function
 * is only called once at a time. "noinline" needed for SystemTap testing.
 *
 * @outbuf buffer to store data in
 * @outbuflen size of outbuf -- if not multiple of LRNG_POOL_WORD_BITS we will
 *	      return a number of bits rounded down to nearest
 *	      LRNG_POOL_WORD_BITS
 * @requested_entropy_bits requested bits of entropy -- the function will return
 *			   at least this amount of entropy if available
 * @drain boolean indicating that that all entropy of pool can be used
 *	  (otherwise some emergency amount of entropy is left)
 * @return estimated entropy from the IRQs that went into the pool since last
 *	   readout.
 */
static noinline u32 lrng_get_pool(u8 *outbuf, u32 outbuflen,
				  u32 requested_entropy_bits, bool drain)
{
	u32 i, irq_num_events_used, irq_num_event_back, ent_bits, words_to_copy;
	/* How many interrupts are in buffer? */
	u32 irq_num_events = atomic_xchg_u32(&lrng_pool.irq_info.num_events, 0);

	irq_num_events = min_t(u32, irq_num_events, LRNG_POOL_SIZE_BITS);

	/* Translate requested entropy bits into data bits */
	requested_entropy_bits = lrng_entropy_to_irqnum(requested_entropy_bits);
	/* Round up such that we are able to collect requested entropy */
	requested_entropy_bits += LRNG_POOL_WORD_BITS - 1;

	/* How many interrupts do we need to and can we use? */
	if (drain)
		irq_num_events_used = min_t(u32, irq_num_events,
					    requested_entropy_bits);
	else
		irq_num_events_used = min_t(u32,
					    (irq_num_events -
			 min_t(u32, lrng_entropy_to_irqnum(LRNG_EMERG_ENTROPY),
			       irq_num_events)),
					    requested_entropy_bits);
	/* Translate entropy in number of complete words to be read */
	words_to_copy = irq_num_events_used / LRNG_POOL_WORD_BITS;
	irq_num_events_used = words_to_copy * LRNG_POOL_WORD_BITS;
	BUG_ON(irq_num_events_used > outbuflen<<3);

	/* Read out the words from the pool */
	for (i = lrng_pool.irq_info.irq_pool_reader;
	     i < (lrng_pool.irq_info.irq_pool_reader + words_to_copy); i++) {
		BUILD_BUG_ON(LRNG_POOL_SIZE_BYTES % sizeof(atomic_t));
		/* write the result from atomic_read directly into buffer */
		*((u32 *)outbuf) =
			atomic_read(&lrng_pool.pool[(i % LRNG_POOL_SIZE)]);
		outbuf += sizeof(atomic_t);
	}

	/* SystemTap: IRQ Raw Entropy Hook line */
	/* There may be new events that came in while we processed this logic */
	irq_num_events += atomic_xchg_u32(&lrng_pool.irq_info.num_events, 0);
	/* Cap the number of events we say we have left to not reuse events */
	irq_num_event_back = min_t(u32, irq_num_events - irq_num_events_used,
				   LRNG_POOL_SIZE_BITS - irq_num_events_used);
	/* Add the unused interrupt number back to the state variable */
	atomic_add(irq_num_event_back, &lrng_pool.irq_info.num_events);

	/* Advance the read pointer */
	lrng_pool.irq_info.irq_pool_reader += words_to_copy;
	lrng_pool.irq_info.irq_pool_reader %= LRNG_POOL_SIZE;

	/* Turn event numbers into entropy statement */
	ent_bits = min_t(int, LRNG_POOL_SIZE_BITS,
			 lrng_irqnum_to_entropy(irq_num_events_used));
	pr_debug("obtained %u bits of entropy from %u newly collected interrupts - not using %u interrupts\n",
		 ent_bits, irq_num_events_used, irq_num_event_back);
	return ent_bits;
}

/****************************** DRBG processing *******************************/

/* Helper to seed the DRBG */
static inline int lrng_drbg_seed_helper(struct drbg_state *drbg,
					const u8 *inbuf, u32 inbuflen)
{
	LIST_HEAD(seedlist);
	struct drbg_string data;

	drbg_string_fill(&data, inbuf, inbuflen);
	list_add_tail(&data.list, &seedlist);
	return drbg->d_ops->update(drbg, &seedlist, drbg->seeded);
}

/* Helper to generate random numbers from the DRBG */
static inline int lrng_drbg_generate_helper(struct drbg_state *drbg, u8 *outbuf,
					    u32 outbuflen)
{
	return drbg->d_ops->generate(drbg, outbuf, outbuflen, NULL);
}

/**
 * Ping all kernel internal callers waiting until the DRBG is fully
 * seeded that the DRBG is now fully seeded.
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

/**
 * Set the slow noise source reseed trigger threshold. The initial threshold
 * is set to the minimum data size that can be read from the pool: a word. Upon
 * reaching this value, the next seed threshold of 112 bits is set followed
 * by 256 bits.
 *
 * @entropy_bits size of entropy currently injected into DRBG
 */
static void lrng_pdrbg_init_ops(u32 entropy_bits)
{
	if (lrng_pdrbg.pdrbg_fully_seeded)
		return;

	BUILD_BUG_ON(LRNG_IRQ_MIN_NUM % LRNG_POOL_WORD_BITS);

	/* DRBG is seeded with full security strength */
	if (entropy_bits >= LRNG_DRBG_SECURITY_STRENGTH_BITS) {
		lrng_pdrbg.pdrbg_fully_seeded = true;
		lrng_pdrbg.pdrbg_min_seeded = true;
		pr_info("primary DRBG fully seeded\n");
		lrng_process_ready_list();
		wake_up_all(&lrng_pdrbg_init_wait);
	} else if (!lrng_pdrbg.pdrbg_min_seeded) {
		/* DRBG is seeded with at least 112 bits of entropy */
		if (entropy_bits >= LRNG_MIN_SEED_ENTROPY_BITS) {
			lrng_pdrbg.pdrbg_min_seeded = true;
			pr_info("primary DRBG minimally seeded\n");
			atomic_set(&lrng_pool.irq_info.num_events_thresh,
				   lrng_irq_entropy_bits());
		/* DRBG is seeded with at least LRNG_POOL_WORD_BITS data bits */
		} else if (entropy_bits >=
			   lrng_irqnum_to_entropy(LRNG_POOL_WORD_BITS)) {
			pr_info("primary DRBG initially seeded\n");
			atomic_set(&lrng_pool.irq_info.num_events_thresh,
				   lrng_irq_min_num());
		}
	}
}

/* Caller must hold lrng_pdrbg.lock */
static int lrng_pdrbg_generate(u8 *outbuf, u32 outbuflen, bool fullentropy)
{
	struct drbg_state *drbg = lrng_pdrbg.pdrbg;
	int ret;

	/* /dev/random only works from a fully seeded DRBG */
	if (fullentropy && !lrng_pdrbg.pdrbg_fully_seeded)
		return 0;

	/*
	 * Only deliver as many bytes as the DRBG is seeded with except during
	 * initialization to provide a first seed to the secondary DRBG.
	 */
	if (lrng_pdrbg.pdrbg_min_seeded)
		outbuflen = min_t(u32, outbuflen,
				  lrng_pdrbg.pdrbg_entropy_bits>>3);
	else
		outbuflen = min_t(u32, outbuflen,
				  LRNG_MIN_SEED_ENTROPY_BITS>>3);
	ret = lrng_drbg_generate_helper(drbg, outbuf, outbuflen);
	if (ret != outbuflen) {
		pr_warn("getting random data from primary DRBG failed (%d)\n",
			ret);
		return ret;
	}
	if (lrng_pdrbg.pdrbg_entropy_bits > (u32)(ret<<3))
		lrng_pdrbg.pdrbg_entropy_bits -= ret<<3;
	else
		lrng_pdrbg.pdrbg_entropy_bits = 0;
	pr_debug("obtained %d bytes of random data from primary DRBG\n", ret);
	pr_debug("primary DRBG entropy level at %u bits\n",
		 lrng_pdrbg.pdrbg_entropy_bits);
	return ret;
}

/**
 * Inject data into the primary DRBG with a given entropy value. The function
 * calls the DRBG's update function. This function also generates random data
 * if requested by caller. The caller is only returned the amount of random
 * data that is at most equal to the amount of entropy that just seeded the
 * DRBG.
 *
 * @inbuf buffer to inject
 * @inbuflen length of inbuf
 * @entropy_bits entropy value of the data in inbuf in bits
 * @outbuf buffer to fill immediately after seeding to get full entropy
 * @outbuflen length of outbuf
 * @fullentropy start /dev/random output only after the DRBG was fully seeded
 * @return number of bytes written to outbuf, 0 if outbuf is not supplied,
 *	   or < 0 in case of error
 */
static int lrng_pdrbg_inject(const u8 *inbuf, u32 inbuflen, u32 entropy_bits,
			     u8 *outbuf, u32 outbuflen, bool fullentropy)
{
	struct drbg_state *drbg = lrng_pdrbg.pdrbg;
	int ret;
	unsigned long flags;

	/* cap the maximum entropy value to the provided data length */
	entropy_bits = min_t(u32, entropy_bits, inbuflen<<3);

	spin_lock_irqsave(&lrng_pdrbg.lock, flags);
	ret = lrng_drbg_seed_helper(drbg, inbuf, inbuflen);
	if (ret < 0) {
		pr_warn("(re)seeding of primary DRBG failed\n");
		goto unlock;
	}
	pr_debug("inject %u bytes with %u bits of entropy into primary DRBG\n",
		 inbuflen, entropy_bits);
	drbg->seeded = true;

	/* Adjust the fill level indicator to at most the DRBG sec strength */
	lrng_pdrbg.pdrbg_entropy_bits =
		min_t(u32, lrng_pdrbg.pdrbg_entropy_bits + entropy_bits,
		      LRNG_DRBG_SECURITY_STRENGTH_BITS);
	lrng_pdrbg_init_ops(lrng_pdrbg.pdrbg_entropy_bits);

	if (outbuf && outbuflen)
		ret = lrng_pdrbg_generate(outbuf, outbuflen, fullentropy);

unlock:
	spin_unlock_irqrestore(&lrng_pdrbg.lock, flags);

	if (lrng_have_entropy_full()) {
		/* Wake readers */
		wake_up_interruptible(&lrng_read_wait);
		kill_fasync(&fasync, SIGIO, POLL_IN);
	}

	return ret;
}

/**
 * Seed the DRBG from the internal noise sources.
 */
static int lrng_pdrbg_seed_internal(u8 *outbuf, u32 outbuflen, bool fullentropy,
				    bool drain)
{
	u8 entropy_buf[LRNG_DRBG_SECURITY_STRENGTH_BYTES +
		       lrng_irq_entropy_bytes()];
	u32 total_entropy_bits;
	int ret;

	/* No reseeding if sufficient entropy in primary DRBG */
	if (lrng_pdrbg.pdrbg_entropy_bits >= outbuflen<<3) {
		unsigned long flags;

		spin_lock_irqsave(&lrng_pdrbg.lock, flags);
		ret = lrng_pdrbg_generate(outbuf, outbuflen, fullentropy);
		spin_unlock_irqrestore(&lrng_pdrbg.lock, flags);
		if (ret == outbuflen)
			goto out;
	}

	/*
	 * The pool should be large enough to allow fully seeding the DRBG with
	 * its security strength if fast noise sources are not available.
	 */
	BUILD_BUG_ON(LRNG_POOL_SIZE_BYTES < LRNG_DRBG_SECURITY_STRENGTH_BYTES);
	BUILD_BUG_ON(LRNG_DRBG_SECURITY_STRENGTH_BYTES % LRNG_POOL_WORD_BYTES);

	/*
	 * Concatenate the output of the noise sources. This would be the
	 * spot to add an entropy extractor logic if desired. Note, this
	 * entirety should have the ability to collect entropy equal or larger
	 * than the DRBG strength to be able to feed /dev/random.
	 */
	total_entropy_bits = lrng_get_arch(entropy_buf);
	/* drain the pool completely during init and when /dev/random calls */
	total_entropy_bits += lrng_get_pool(
			entropy_buf + LRNG_DRBG_SECURITY_STRENGTH_BYTES,
			lrng_irq_entropy_bytes(),
			LRNG_DRBG_SECURITY_STRENGTH_BITS - total_entropy_bits,
			drain);

	pr_debug("reseed primary DRBG from internal noise sources with %u bits of entropy\n",
		 total_entropy_bits);

	ret = lrng_pdrbg_inject(entropy_buf, sizeof(entropy_buf),
				total_entropy_bits,
				outbuf, outbuflen, fullentropy);
	memzero_explicit(entropy_buf, sizeof(entropy_buf));

out:
	/* Allow the seeding operation to be called again */
	atomic_set(&lrng_pool.irq_info.reseed_in_progress, 0);

	return ret;
}

/**
 * Inject a data buffer into the secondary DRBG
 *
 * @sdrbg reference to secondary DRBG
 * @inbuf buffer with data to inject
 * @inbuflen buffer length
 * @internal did random data originate from internal sources? Update the
 *	     reseed threshold and the reseed timer when seeded with entropic
 *	     data from noise sources to prevent unprivileged users from
 *	     stopping reseeding the secondary DRBG with entropic data.
 */
static void lrng_sdrbg_inject(struct lrng_sdrbg *sdrbg,
			      u8 *inbuf, u32 inbuflen, bool internal)
{
	unsigned long flags;

	BUILD_BUG_ON(LRNG_DRBG_RESEED_THRESH > INT_MAX);
	pr_debug("seeding secondary DRBG with %u bytes\n", inbuflen);
	spin_lock_irqsave(&sdrbg->lock, flags);
	if (lrng_drbg_seed_helper(sdrbg->sdrbg, inbuf, inbuflen) < 0) {
		pr_warn("seeding of secondary DRBG failed\n");
		atomic_set(&sdrbg->requests, 1);
	} else if (internal) {
		pr_debug("secondary DRBG stats since last seeding: %lu secs; generate calls: %d\n",
			 (jiffies - sdrbg->last_seeded) / HZ,
			 (LRNG_DRBG_RESEED_THRESH -
			  atomic_read(&sdrbg->requests)));
		sdrbg->last_seeded = jiffies;
		atomic_set(&sdrbg->requests, LRNG_DRBG_RESEED_THRESH);
	}
	spin_unlock_irqrestore(&sdrbg->lock, flags);
}

/**
 * Try to seed the secondary DRBG
 *
 * @sdrbg reference to secondary DRBG
 * @seedfunc function to use to seed and obtain random data from primary DRBG
 */
static void lrng_sdrbg_seed(struct lrng_sdrbg *sdrbg,
	int (*seed_func)(u8 *outbuf, u32 outbuflen, bool fullentropy,
			 bool drain))
{
	u8 seedbuf[LRNG_DRBG_SECURITY_STRENGTH_BYTES];
	int ret;

	BUILD_BUG_ON(LRNG_MIN_SEED_ENTROPY_BITS >
		     LRNG_DRBG_SECURITY_STRENGTH_BITS);

	pr_debug("reseed of secondary DRBG triggered\n");
	ret = seed_func(seedbuf, LRNG_DRBG_SECURITY_STRENGTH_BYTES, false,
			!sdrbg->fully_seeded);
	/* Update the DRBG state even though we received zero random data */
	if (ret < 0) {
		/*
		 * Try to reseed at next round - note if EINPROGRESS is returned
		 * the request counter may fall below zero in case of parallel
		 * operations. We accept such "underflow" temporarily as the
		 * counter will be set back to a positive number in the course
		 * of the reseed. For these few generate operations under
		 * heavy parallel strain of /dev/urandom we therefore exceed
		 * the LRNG_DRBG_RESEED_THRESH threshold.
		 */
		if (ret != -EINPROGRESS)
			atomic_set(&sdrbg->requests, 1);
		return;
	}

	lrng_sdrbg_inject(sdrbg, seedbuf, ret, true);
	memzero_explicit(seedbuf, ret);

	if (ret >= LRNG_DRBG_SECURITY_STRENGTH_BYTES)
		sdrbg->fully_seeded = true;
}

/**
 * DRBG reseed trigger: Kernel thread handler triggered by the schedule_work()
 */
static void lrng_pdrbg_seed_work(struct work_struct *dummy)
{
	u32 node;

	for (node = 0; node <= lrng_pool.last_numa_node; node++) {
		struct lrng_sdrbg *sdrbg = lrng_sdrbg[node];

		if (!sdrbg->fully_seeded) {
			pr_debug("reseed triggered by interrupt noise source for secondary DRBG on NUMA node %d\n", node);
			lrng_sdrbg_seed(sdrbg, lrng_pdrbg_seed_internal);
			if (node && sdrbg->fully_seeded) {
				/* Prevent reseed storm */
				sdrbg->last_seeded += node * 100 * HZ;
				/* Prevent draining of pool on idle systems */
				lrng_sdrbg_reseed_max_time += 100;
			}
			return;
		}
	}
}

/**
 * DRBG reseed trigger: Synchronous reseed request
 */
static int lrng_pdrbg_seed(u8 *outbuf, u32 outbuflen, bool fullentropy,
			   bool drain)
{
	/* Ensure that the seeding only occurs once at any given time */
	if (atomic_cmpxchg(&lrng_pool.irq_info.reseed_in_progress, 0, 1))
		return -EINPROGRESS;
	return lrng_pdrbg_seed_internal(outbuf, outbuflen, fullentropy, drain);
}

static inline struct lrng_sdrbg *lrng_get_sdrbg(void)
{
	struct lrng_sdrbg *sdrbg = lrng_sdrbg[numa_node_id()];

	if (sdrbg->fully_seeded)
		return sdrbg;
	else
		return lrng_sdrbg[0];
}

/**
 * Allocation of the DRBG state
 */
static struct drbg_state *lrng_drbg_alloc(void)
{
	struct drbg_state *drbg = NULL;
	int coreref = -1;
	bool pr = false;
	int ret = 0;

	drbg_convert_tfm_core(LRNG_DRBG_CORE, &coreref, &pr);
	if (coreref < 0)
		return NULL;

	drbg = kzalloc(sizeof(struct drbg_state), GFP_KERNEL);
	if (!drbg)
		return NULL;

	drbg->core = &drbg_cores[coreref];
	drbg->seeded = false;
	ret = drbg_alloc_state(drbg);
	if (ret)
		goto err;

	ret = drbg->d_ops->crypto_init(drbg);
	if (ret == 0)
		return drbg;

	drbg_dealloc_state(drbg);
err:
	kfree(drbg);
	return NULL;
}

static int lrng_drbgs_alloc(void)
{
	unsigned long flags;
	struct drbg_state *pdrbg;
	u32 node;
	u32 num_nodes = num_possible_nodes();

	pdrbg = lrng_drbg_alloc();
	if (!pdrbg)
		return -EFAULT;

	spin_lock_irqsave(&lrng_pdrbg.lock, flags);
	if (lrng_pdrbg.pdrbg) {
		drbg_dealloc_state(pdrbg);
		kfree(pdrbg);
	} else {
		lrng_pdrbg.pdrbg = pdrbg;
		INIT_WORK(&lrng_pdrbg.lrng_seed_work, lrng_pdrbg_seed_work);
		pr_info("primary DRBG with %s core allocated\n",
			lrng_pdrbg.pdrbg->core->backend_cra_name);
	}

	lrng_pool.last_numa_node = num_nodes - 1;

	spin_unlock_irqrestore(&lrng_pdrbg.lock, flags);

	lrng_sdrbg = kmalloc(num_nodes * sizeof(void *),
			     GFP_KERNEL|__GFP_NOFAIL);
	for (node = 0; node < num_nodes; node++) {
		struct lrng_sdrbg *sdrbg;

		sdrbg = kzalloc(sizeof(struct lrng_sdrbg), GFP_KERNEL);
		if (!sdrbg)
			goto err;
		lrng_sdrbg[node] = sdrbg;

		sdrbg->sdrbg = lrng_drbg_alloc();
		if (!sdrbg->sdrbg)
			goto err;

		atomic_set(&sdrbg->requests, 1);
		spin_lock_init(&sdrbg->lock);
		sdrbg->last_seeded = jiffies;
		sdrbg->fully_seeded = false;

		pr_info("secondary DRBG with %s core for NUMA node %d allocated\n",
			sdrbg->sdrbg->core->backend_cra_name, node);
	}

	return 0;

err:
	for (node = 0; node < num_nodes; node++) {
		struct lrng_sdrbg *sdrbg = lrng_sdrbg[node];

		if (sdrbg) {
			if (sdrbg->sdrbg)
				drbg_dealloc_state(sdrbg->sdrbg);
			kfree(sdrbg);
		}
	}
	kfree(lrng_sdrbg);

	drbg_dealloc_state(pdrbg);
	kfree(pdrbg);

	return -EFAULT;
}

/**
 * Obtain random data from DRBG with information theoretical entropy by
 * triggering a reseed. The primary DRBG will only return as many random
 * bytes as it was seeded with.
 *
 * @outbuf buffer to store the random data in
 * @outbuflen length of outbuf
 * @return: < 0 on error
 *	    >= 0 the number of bytes that were obtained
 */
static int lrng_pdrbg_get(u8 *outbuf, u32 outbuflen)
{
	int ret;

	if (!outbuf || !outbuflen)
		return 0;

	/* DRBG is not yet available */
	if (!atomic_read(&lrng_pdrbg_avail))
		return 0;

	ret = lrng_pdrbg_seed(outbuf, outbuflen, true, true);
	pr_debug("read %u bytes of full entropy data from primary DRBG\n", ret);

	/* Shall we wake up user space writers? */
	if (lrng_need_entropy()) {
		wake_up_interruptible(&lrng_write_wait);
		kill_fasync(&fasync, SIGIO, POLL_OUT);
	}

	return ret;
}

/**
 * Initial RNG provides random data with as much entropy as we have
 * at boot time until the DRBG becomes available during late_initcall() but
 * before user space boots. When the DRBG is initialized, the initial RNG
 * is retired.
 *
 * Note: until retirement of this RNG, the system did not generate too much
 * entropy yet. Hence, a proven DRNG like a DRBG is not necessary here anyway.
 *
 * The RNG is using the following as noise source:
 *	* high resolution time stamps
 *	* the collected IRQ state
 *	* CPU noise source if available
 *
 * Input/output: it is a drop-in replacement for lrng_sdrbg_get.
 */
static u32 lrng_init_state[SHA_WORKSPACE_WORDS];
static int lrng_init_rng(u8 *outbuf, u32 outbuflen)
{
	u32 hash[SHA_DIGEST_WORDS];
	u32 outbuflen_orig = outbuflen;
	u32 workspace[SHA_WORKSPACE_WORDS];

	BUILD_BUG_ON(sizeof(lrng_init_state[0]) != LRNG_POOL_WORD_BYTES);

	sha_init(hash);
	while (outbuflen) {
		unsigned int arch;
		u32 i;
		u32 todo = min_t(u32, outbuflen,
				 SHA_WORKSPACE_WORDS * sizeof(u32));

		for (i = 0; i < SHA_WORKSPACE_WORDS; i++) {
			if (arch_get_random_int(&arch))
				lrng_init_state[i] ^= arch;
			lrng_init_state[i] ^= random_get_entropy();
			if (i < LRNG_POOL_SIZE)
				lrng_init_state[i] ^=
					atomic_read_u32(&lrng_pool.pool[i]);
		}
		sha_transform(hash, (u8 *)&lrng_init_state, workspace);
		/* Mix generated data back in for backtracking resistance */
		for (i = 0; i < SHA_DIGEST_WORDS; i++)
			lrng_init_state[i] ^= hash[0];

		memcpy(outbuf, hash, todo);
		outbuf += todo;
		outbuflen -= todo;
		atomic_add(todo, &lrng_initrng_bytes);
	}
	memzero_explicit(hash, sizeof(hash));
	memzero_explicit(workspace, sizeof(workspace));

	return outbuflen_orig;
}

/**
 * Get random data out of the secondary DRBG which is reseeded frequently. In
 * the worst case, the DRBG may generate random numbers without being reseeded
 * for LRNG_DRBG_RESEED_THRESH requests times LRNG_DRBG_MAX_REQSIZE bytes.
 *
 * If the DRBG is not yet initialized, use the initial RNG output.
 *
 * @outbuf buffer for storing random data
 * @outbuflen length of outbuf
 * @return < 0 in error case (DRBG generation or update failed)
 *	   >=0 returning the returned number of bytes
 */
static int lrng_sdrbg_get(u8 *outbuf, u32 outbuflen)
{
	u32 processed = 0;
	struct lrng_sdrbg *sdrbg;
	unsigned long flags;
	int ret;

	if (!outbuf || !outbuflen)
		return 0;

	outbuflen = min_t(size_t, outbuflen, INT_MAX);

	/* DRBG is not yet available */
	if (!atomic_read(&lrng_pdrbg_avail)) {
		spin_lock_irqsave(&lrng_init_rng_lock, flags);
		/* Prevent race with lrng_init */
		if (!atomic_read(&lrng_pdrbg_avail)) {
			ret = lrng_init_rng(outbuf, outbuflen);
			spin_unlock_irqrestore(&lrng_init_rng_lock, flags);
			return ret;
		}
		spin_unlock_irqrestore(&lrng_init_rng_lock, flags);
	}

	sdrbg = lrng_get_sdrbg();
	while (outbuflen) {
		unsigned long now = jiffies;
		u32 todo = min_t(u32, outbuflen, LRNG_DRBG_MAX_REQSIZE);

		if (atomic_dec_and_test(&sdrbg->requests) ||
		    time_after(now, sdrbg->last_seeded +
			       lrng_sdrbg_reseed_max_time * HZ))
			lrng_sdrbg_seed(sdrbg, lrng_pdrbg_seed);

		spin_lock_irqsave(&sdrbg->lock, flags);
		ret = lrng_drbg_generate_helper(sdrbg->sdrbg,
						outbuf + processed, todo);
		spin_unlock_irqrestore(&sdrbg->lock, flags);
		if (ret <= 0) {
			pr_warn("getting random data from secondary DRBG failed (%d)\n",
				ret);
			return -EFAULT;
		}
		processed += ret;
		outbuflen -= ret;
	}

	return processed;
}

/************************** LRNG kernel interfaces ***************************/

void get_random_bytes(void *buf, int nbytes)
{
	lrng_sdrbg_get((u8 *)buf, (u32)nbytes);
}
EXPORT_SYMBOL(get_random_bytes);

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
 * @buf buffer allocated by caller to store the random data in
 * @nbytes length of outbuf
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
		lrng_sdrbg_get((u8 *)p, (u32)nbytes);
}
EXPORT_SYMBOL(get_random_bytes_arch);

/**
 * Interface for in-kernel drivers of true hardware RNGs.
 * Those devices may produce endless random bits and will be throttled
 * when our pool is full.
 *
 * @buffer buffer holding the entropic data from HW noise sources to be used to
 *	   (re)seed the DRBG.
 * @count length of buffer
 * @entropy_bits amount of entropy in buffer (value is in bits)
 */
void add_hwgenerator_randomness(const char *buffer, size_t count,
				size_t entropy_bits)
{
	/* DRBG is not yet online */
	if (!atomic_read(&lrng_pdrbg_avail))
		return;
	/*
	 * Suspend writing if we are fully loaded with entropy.
	 * We'll be woken up again once below lrng_write_wakeup_thresh,
	 * or when the calling thread is about to terminate.
	 */
	wait_event_interruptible(lrng_write_wait,
				 kthread_should_stop() || lrng_need_entropy());
	lrng_pdrbg_inject(buffer, count, entropy_bits, NULL, 0, false);
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
 * Add a callback function that will be invoked when the DRBG is fully seeded.
 *
 * returns: 0 if callback is successfully added
 *          -EALREADY if pool is already initialised (callback not called)
 *	    -ENOENT if module for callback is not alive
 */
int add_random_ready_callback(struct random_ready_callback *rdy)
{
	struct module *owner;
	unsigned long flags;
	int err = -EALREADY;

	if (likely(lrng_pdrbg.pdrbg_fully_seeded))
		return err;

	owner = rdy->owner;
	if (!try_module_get(owner))
		return -ENOENT;

	spin_lock_irqsave(&lrng_ready_list_lock, flags);
	if (lrng_pdrbg.pdrbg_fully_seeded)
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
	u8 tmpbuf[LRNG_DRBG_BLOCKLEN_BYTES];
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
	if (nbytes > LRNG_DRBG_BLOCKLEN_BYTES) {
		tmplen = min_t(u32, nbytes, LRNG_DRBG_MAX_REQSIZE);
		tmp_large = kmalloc(tmplen, GFP_KERNEL);
		if (!tmp_large)
			tmplen = sizeof(tmpbuf);
		else
			tmp = tmp_large;
	}

	while (nbytes) {
		u32 todo = min_t(u32, nbytes, tmplen);
		int rc = 0;

		if (tmp_large && need_resched()) {
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
lrng_pdrbg_read_common(int nonblock, char __user *buf, size_t nbytes)
{
	ssize_t n;

	if (nbytes == 0)
		return 0;

	nbytes = min_t(u32, nbytes, LRNG_DRBG_BLOCKLEN_BYTES);
	while (1) {
		n = lrng_read_common(buf, nbytes, lrng_pdrbg_get);
		if (n < 0)
			return n;
		if (n > 0)
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

static ssize_t lrng_pdrbg_read(struct file *file, char __user *buf,
			       size_t nbytes, loff_t *ppos)
{
	return lrng_pdrbg_read_common(file->f_flags & O_NONBLOCK, buf, nbytes);
}

static unsigned int lrng_pdrbg_poll(struct file *file, poll_table *wait)
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

static ssize_t lrng_drbg_write_common(const char __user *buffer, size_t count,
				      u32 entropy_bits, bool sdrbg)
{
	ssize_t ret = 0;
	u8 buf[64];
	const char __user *p = buffer;

	if (!atomic_read(&lrng_pdrbg_avail))
		return -EAGAIN;

	count = min_t(size_t, count, INT_MAX);
	while (count > 0) {
		size_t bytes = min_t(size_t, count, sizeof(buf));
		u32 ent = min_t(u32, bytes<<3, entropy_bits);

		if (copy_from_user(&buf, p, bytes))
			return -EFAULT;
		/* Inject data into primary DRBG */
		lrng_pdrbg_inject(buf, bytes, ent, NULL, 0, false);
		/* Data from /dev/[|u]random is injected into secondary DRBG */
		if (sdrbg) {
			u32 node;
			int num_nodes = num_possible_nodes();

			for (node = 0; node < num_nodes; node++)
				lrng_sdrbg_inject(lrng_sdrbg[node], buf, bytes,
						  false);
		}

		count -= bytes;
		p += bytes;
		ret += bytes;
		entropy_bits -= ent;

		cond_resched();
	}

	return ret;
}

static ssize_t lrng_sdrbg_read(struct file *file, char __user *buf,
			       size_t nbytes, loff_t *ppos)
{
	return lrng_read_common(buf, nbytes, lrng_sdrbg_get);
}

static ssize_t lrng_drbg_write(struct file *file, const char __user *buffer,
			       size_t count, loff_t *ppos)
{
	return lrng_drbg_write_common(buffer, count, 0, true);
}

static long lrng_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int size, ent_count;
	int __user *p = (int __user *)arg;

	switch (cmd) {
	case RNDGETENTCNT:
		ent_count = atomic_read(&lrng_pool.irq_info.num_events);
		if (put_user(ent_count, p))
			return -EFAULT;
		return 0;
	case RNDADDTOENTCNT:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count, p))
			return -EFAULT;
		if (ent_count < 0) {
			/* ensure that entropy count cannot go below zero */
			ent_count = -ent_count;
			ent_count = min(ent_count,
				atomic_read(&lrng_pool.irq_info.num_events));
			atomic_sub(ent_count, &lrng_pool.irq_info.num_events);
		} else {
			ent_count = min_t(int, ent_count, LRNG_POOL_SIZE_BITS);
			atomic_add(ent_count, &lrng_pool.irq_info.num_events);
		}
		return 0;
	case RNDADDENTROPY:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count, p++))
			return -EFAULT;
		if (ent_count < 0)
			return -EINVAL;
		if (get_user(size, p++))
			return -EFAULT;
		if (size < 0)
			return -EINVAL;
		/* there cannot be more entropy than data */
		ent_count = min(ent_count, size);
		/* ent_count is in bytes, but lrng_drbg_write requires bits */
		return lrng_drbg_write_common((const char __user *)p, size,
					      ent_count<<3, false);
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
	.read  = lrng_pdrbg_read,
	.write = lrng_drbg_write,
	.poll  = lrng_pdrbg_poll,
	.unlocked_ioctl = lrng_ioctl,
	.fasync = lrng_fasync,
	.llseek = noop_llseek,
};

const struct file_operations urandom_fops = {
	.read  = lrng_sdrbg_read,
	.write = lrng_drbg_write,
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
		return lrng_pdrbg_read_common(flags & GRND_NONBLOCK, buf,
					      count);

	if (unlikely(!lrng_pdrbg.pdrbg_fully_seeded)) {
		if (flags & GRND_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(lrng_pdrbg_init_wait,
					 lrng_pdrbg.pdrbg_fully_seeded);
		if (signal_pending(current))
			return -ERESTARTSYS;
	}
	return lrng_sdrbg_read(NULL, buf, count, NULL);
}

/*************************** LRNG proc interfaces ****************************/

#ifdef CONFIG_SYSCTL

#include <linux/sysctl.h>

static int lrng_min_read_thresh = LRNG_POOL_WORD_BITS;
static int lrng_min_write_thresh;
static int lrng_max_read_thresh = LRNG_POOL_SIZE_BITS;
static int lrng_max_write_thresh = LRNG_POOL_SIZE_BITS;
static char lrng_sysctl_bootid[16];
static int lrng_sdrbg_reseed_max_min;

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
	unsigned char buf[30];

	snprintf(buf, sizeof(buf), "%s: %s",
#ifdef CONFIG_CRYPTO_DRBG_HMAC
		 "HMAC DRBG",
#elif defined CONFIG_CRYPTO_DRBG_CTR
		 "CTR DRBG",
#elif defined CONFIG_CRYPTO_DRBG_HASH
		 "HASH DRBG",
#else
		 "unknown",
#endif
		 lrng_pdrbg.pdrbg->core->backend_cra_name);

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

	entropy_count = atomic_read((atomic_t *)table->data);
	if (table->extra2)
		entropy_count = min_t(int, entropy_count,
				      *(int *)table->extra2);

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
static int pdrbg_security_strength = LRNG_DRBG_SECURITY_STRENGTH_BYTES;
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
		.data		= &lrng_pool.irq_info.num_events,
		.extra2		= &lrng_max_write_thresh,
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
		.data           = &lrng_sdrbg_reseed_max_time,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec,
		.extra1		= &lrng_sdrbg_reseed_max_min,
	},
	{
		.procname	= "drbg_fully_seeded",
		.data		= &lrng_pdrbg.pdrbg_fully_seeded,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= lrng_proc_bool,
	},
	{
		.procname	= "drbg_minimally_seeded",
		.data		= &lrng_pdrbg.pdrbg_min_seeded,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= lrng_proc_bool,
	},
	{
		.procname	= "drbg_type",
		.maxlen		= 30,
		.mode		= 0444,
		.proc_handler	= lrng_proc_do_type,
	},
	{
		.procname	= "drbg_security_strength",
		.data		= &pdrbg_security_strength,
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

/***************************** Initialize DRBG *******************************/

static int __init lrng_init(void)
{
	unsigned long flags;

	BUG_ON(lrng_drbgs_alloc());
	BUG_ON(LRNG_DRBG_BLOCKLEN_BYTES !=
	       lrng_pdrbg.pdrbg->core->blocklen_bytes);
	BUG_ON(LRNG_DRBG_SECURITY_STRENGTH_BYTES !=
	       drbg_sec_strength(lrng_pdrbg.pdrbg->core->flags));

	/*
	 * As we use the IRQ entropic input data processed by the init RNG
	 * again during lrng_pdrbg_seed_internal, we must not claim that
	 * the init RNG state has any entropy when injecting its contents as
	 * an initial seed into the DRBG.
	 */
	spin_lock_irqsave(&lrng_init_rng_lock, flags);

	if (random_get_entropy() || random_get_entropy())
		lrng_pool.irq_info.irq_highres_timer = true;
#ifdef CONFIG_CRYPTO_FIPS
	else {
		if (fips_enabled) {
			pr_warn("LRNG not suitable for FIPS 140-2 use cases\n");
			WARN_ON(1);
		}
	}
#endif

	lrng_pdrbg_inject((u8 *)&lrng_init_state,
			  SHA_WORKSPACE_WORDS * sizeof(lrng_init_state[0]),
			  0, NULL, 0, false);
	lrng_sdrbg_seed(lrng_sdrbg[0], lrng_pdrbg_seed);
	atomic_inc(&lrng_pdrbg_avail);
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
