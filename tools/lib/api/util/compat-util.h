#ifndef __API_UTIL_COMPAT_UTIL_H
#define __API_UTIL_COMPAT_UTIL_H

#ifndef FLEX_ARRAY
/*
 * See if our compiler is known to support flexible array members.
 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define FLEX_ARRAY /* empty */
#elif defined(__GNUC__)
# if (__GNUC__ >= 3)
#  define FLEX_ARRAY /* empty */
# else
#  define FLEX_ARRAY 0 /* older GNU extension */
# endif
#endif

/*
 * Otherwise, default to safer but a bit wasteful traditional style
 */
#ifndef FLEX_ARRAY
# define FLEX_ARRAY 1
#endif
#endif

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

#ifdef __GNUC__
#define TYPEOF(x) (__typeof__(x))
#else
#define TYPEOF(x)
#endif

#define MSB(x, bits) ((x) & TYPEOF(x)(~0ULL << (sizeof(x) * 8 - (bits))))
#define HAS_MULTI_BITS(i)  ((i) & ((i) - 1))  /* checks if an integer has more than 1 bit set */

/* Approximation of the length of the decimal representation of this type. */
#define decimal_length(x)	((int)(sizeof(x) * 2.56 + 0.5) + 1)

#define _ALL_SOURCE 1
#define _BSD_SOURCE 1
/* glibc 2.20 deprecates _BSD_SOURCE in favour of _DEFAULT_SOURCE */
#define _DEFAULT_SOURCE 1
#define HAS_BOOL

#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <fnmatch.h>
#include <assert.h>
#include <regex.h>
#include <utime.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/types.h>
#include <sys/ttydefaults.h>
#include <termios.h>
#include <linux/bitops.h>
#include <termios.h>

/* On most systems <limits.h> would have given us this, but
 * not on some systems (e.g. GNU/Hurd).
 */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef PRIuMAX
#define PRIuMAX "llu"
#endif

#ifndef PRIu32
#define PRIu32 "u"
#endif

#ifndef PRIx32
#define PRIx32 "x"
#endif

#ifndef PATH_SEP
#define PATH_SEP ':'
#endif

#ifndef STRIP_EXTENSION
#define STRIP_EXTENSION ""
#endif

#ifndef has_dos_drive_prefix
#define has_dos_drive_prefix(path) 0
#endif

#ifndef is_dir_sep
#define is_dir_sep(c) ((c) == '/')
#endif

#ifdef __GNUC__
#define NORETURN __attribute__((__noreturn__))
#else
#define NORETURN
#ifndef __attribute__
#define __attribute__(x)
#endif
#endif

#ifndef NSEC_PER_MSEC
#define NSEC_PER_MSEC	1000000L
#endif

#endif /* __API_UTIL_COMPAT_UTIL_H */
