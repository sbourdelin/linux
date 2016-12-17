#ifndef __LINUX_PRIME_NUMBERS_H
#define __LINUX_PRIME_NUMBERS_H

#include <linux/types.h>

bool is_prime_number(unsigned long x);
unsigned long next_prime_number(unsigned long x);

/* A useful white-lie here is that 1 is prime. */
#define for_each_prime_number(prime, max) \
	for (prime = 1;	prime < (max); prime = next_prime_number(prime))

#endif /* !__LINUX_PRIME_NUMBERS_H */
