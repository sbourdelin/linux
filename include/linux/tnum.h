/* tnum: tracked (or tristate) numbers
 *
 * A tnum tracks knowledge about the bits of a value.  Each bit can be either
 * known (0 or 1), or unknown (x).  Arithmetic operations on tnums will
 * propagate the unknown bits such that the tnum result represents all the
 * possible results for possible values of the operands.
 */
#include <linux/types.h>

struct tnum {
	u64 value;
	u64 mask;
};

/* Constructors */
/* Represent a known constant as a tnum. */
struct tnum tn_const(u64 value);
/* A completely unknown value */
extern const struct tnum tn_unknown;

/* Arithmetic and logical ops */
/* Shift a tnum left (by a fixed shift) */
struct tnum tn_sl(struct tnum a, u8 shift);
/* Shift a tnum right (by a fixed shift) */
struct tnum tn_sr(struct tnum a, u8 shift);
/* Add two tnums, return %a + %b */
struct tnum tn_add(struct tnum a, struct tnum b);
/* Subtract two tnums, return %a - %b */
struct tnum tn_sub(struct tnum a, struct tnum b);
/* Bitwise-AND, return %a & %b */
struct tnum tn_and(struct tnum a, struct tnum b);
/* Bitwise-OR, return %a | %b */
struct tnum tn_or(struct tnum a, struct tnum b);
/* Bitwise-XOR, return %a ^ %b */
struct tnum tn_xor(struct tnum a, struct tnum b);
/* Multiply two tnums, return %a * %b */
struct tnum tn_mul(struct tnum a, struct tnum b);

/* Return a tnum representing numbers satisfying both %a and %b */
struct tnum tn_intersect(struct tnum a, struct tnum b);

/* Returns true if %a is known to be a multiple of %size.
 * %size must be a power of two.
 */
bool tn_is_aligned(struct tnum a, u64 size);

/* Returns true if %b represents a subset of %a. */
bool tn_in(struct tnum a, struct tnum b);

/* Formatting functions.  These have snprintf-like semantics: they will write
 * up to size bytes (including the terminating NUL byte), and return the number
 * of bytes (excluding the terminating NUL) which would have been written had
 * sufficient space been available.  (Thus tn_sbin always returns 64.)
 */
/* Format a tnum as a pair of hex numbers (value; mask) */
int tn_strn(char *str, size_t size, struct tnum a);
/* Format a tnum as tristate binary expansion */
int tn_sbin(char *str, size_t size, struct tnum a);
