/*
 * internal.h - printk internal definitions
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/percpu.h>

#ifdef CONFIG_PRINTK

#define PRINTK_SAFE_CONTEXT_MASK	0x7fffffff
#define PRINTK_NMI_CONTEXT_MASK	0x80000000

extern raw_spinlock_t logbuf_lock;
extern bool printk_safe_init_done;

__printf(1, 0) int vprintk_default(const char *fmt, va_list args);
__printf(1, 0) int vprintk_func(const char *fmt, va_list args);
void __printk_safe_enter(void);
void __printk_safe_exit(void);

#define printk_safe_enter(flags)		\
	do {					\
		local_irq_save(flags);		\
		__printk_safe_enter();		\
	} while (0)

#define printk_safe_exit(flags)		\
	do {					\
		__printk_safe_exit();		\
		local_irq_restore(flags);	\
	} while (0)

#else

__printf(1, 0) int vprintk_func(const char *fmt, va_list args) { return 0; }

#define printk_safe_enter(f)	((void)(f))
#define printk_safe_exit(f)	((void)(f))

#endif /* CONFIG_PRINTK */
