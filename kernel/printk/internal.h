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

#ifdef CONFIG_PRINTK_NMI

extern atomic_t nmi_message_lost;
static inline int get_nmi_message_lost(void)
{
	return atomic_xchg(&nmi_message_lost, 0);
}

#else /* CONFIG_PRINTK_NMI */

static inline int get_nmi_message_lost(void)
{
	return 0;
}

#endif /* CONFIG_PRINTK_NMI */

#ifdef CONFIG_PRINTK

#define ALT_PRINTK_CONTEXT_MASK		0x07ffffff
#define ALT_PRINTK_NMI_CONTEXT_MASK	0x08000000

extern raw_spinlock_t logbuf_lock;

__printf(1, 0) int vprintk_default(const char *fmt, va_list args);
__printf(1, 0) int vprintk_func(const char *fmt, va_list args);
void alt_printk_enter(void);
void alt_printk_exit(void);

#else

__printf(1, 0) int vprintk_func(const char *fmt, va_list args) { return 0; }
void alt_printk_enter(void) { }
void alt_printk_exit(void) { }

#endif /* CONFIG_PRINTK */
