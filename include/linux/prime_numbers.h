#ifndef __LINUX_PRIME_NUMBERS_H
#define __LINUX_PRIME_NUMBERS_H

#include <linux/types.h>

bool is_prime_number(unsigned long x);
unsigned long next_prime_number(unsigned long x);

/**
 * for_each_prime_number - iterate over each prime upto a value
 * @prime: the current prime number in this iteration
 * @max: the upper limit
 *
 * Starting from 1 (which is only considered prime for convenience
 * of using for_each_prime_number(), a useful white lie), iterate over each
 * prime number up to the @max value. On each iteration, @prime is set to the
 * current prime number. @max should be less than ULONG_MAX to ensure
 * termination.
 */
#define for_each_prime_number(prime, max) \
	for (prime = 1;	prime <= (max); prime = next_prime_number(prime))

#endif /* !__LINUX_PRIME_NUMBERS_H */
