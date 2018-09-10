/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TERMIOS_CONV_H
#define _LINUX_TERMIOS_CONV_H

#include <linux/uaccess.h>
#include <asm/termios.h>

#ifndef INIT_C_CC
/*	intr=^C		quit=^\		erase=del	kill=^U
	eof=^D		vtime=\0	vmin=\1		sxtc=\0
	start=^Q	stop=^S		susp=^Z		eol=\0
	reprint=^R	discard=^U	werase=^W	lnext=^V
	eol2=\0
*/
#define INIT_C_CC {		\
	[VINTR] = 'C'-0x40,	\
	[VQUIT] = '\\'-0x40,	\
	[VERASE] = '\177',	\
	[VKILL] = 'U'-0x40,	\
	[VEOF] = 'D'-0x40,	\
	[VSTART] = 'Q'-0x40,	\
	[VSTOP] = 'S'-0x40,	\
	[VSUSP] = 'Z'-0x40,	\
	[VREPRINT] = 'R'-0x40,	\
	[VDISCARD] = 'O'-0x40,	\
	[VWERASE] = 'W'-0x40,	\
	[VLNEXT] = 'V'-0x40,	\
	[VMIN] = 1 }
#endif

#ifndef user_termio_to_kernel_termios
/*
 * Translate a "termio" structure into a "termios". Ugh.
 */
static inline int user_termio_to_kernel_termios(struct ktermios *termios,
						struct termio __user *termio)
{
	struct termio v;

	if (copy_from_user(&v, termio, sizeof(struct termio)))
		return -EFAULT;

	termios->c_iflag = (0xffff0000 & termios->c_iflag) | v.c_iflag;
	termios->c_oflag = (0xffff0000 & termios->c_oflag) | v.c_oflag;
	termios->c_cflag = (0xffff0000 & termios->c_cflag) | v.c_cflag;
	termios->c_lflag = (0xffff0000 & termios->c_lflag) | v.c_lflag;
	termios->c_line = (0xffff0000 & termios->c_lflag) | v.c_line;
	memcpy(termios->c_cc, v.c_cc, NCC);
	return 0;
}
#endif

#ifndef kernel_termios_to_user_termio
/*
 * Translate a "termios" structure into a "termio". Ugh.
 */
static inline int kernel_termios_to_user_termio(struct termio __user *termio,
						struct ktermios *termios)
{
	struct termio v;
	memset(&v, 0, sizeof(struct termio));
	v.c_iflag = termios->c_iflag;
	v.c_oflag = termios->c_oflag;
	v.c_cflag = termios->c_cflag;
	v.c_lflag = termios->c_lflag;
	v.c_line = termios->c_line;
	memcpy(v.c_cc, termios->c_cc, NCC);
	return copy_to_user(termio, &v, sizeof(struct termio));
}
#endif

#ifdef TCGETS2
#ifndef user_termios_to_kernel_termios
static inline int user_termios_to_kernel_termios(struct ktermios *k,
						 struct termios2 __user *u)
{
	return copy_from_user(k, u, sizeof(struct termios2));
}
#endif
#ifndef kernel_termios_to_user_termios
static inline int kernel_termios_to_user_termios(struct termios2 __user *u,
						 struct ktermios *k)
{
	return copy_to_user(u, k, sizeof(struct termios2));
}
#endif
#ifndef user_termios_to_kernel_termios_1
static inline int user_termios_to_kernel_termios_1(struct ktermios *k,
						   struct termios __user *u)
{
	return copy_from_user(k, u, sizeof(struct termios));
}
#endif

#ifndef kernel_termios_to_user_termios_1
static inline int kernel_termios_to_user_termios_1(struct termios __user *u,
						   struct ktermios *k)
{
	return copy_to_user(u, k, sizeof(struct termios));
}
#endif

#else

#ifndef user_termios_to_kernel_termios
static inline int user_termios_to_kernel_termios(struct ktermios *k,
						 struct termios __user *u)
{
	return copy_from_user(k, u, sizeof(struct termios));
}
#endif
#ifndef kernel_termios_to_user_termios
static inline int kernel_termios_to_user_termios(struct termios __user *u,
						 struct ktermios *k)
{
	return copy_to_user(u, k, sizeof(struct termios));
}
#endif

#endif /* TCGETS2 */

#endif /* _LINUX_TERMIOS_CONV_H */
