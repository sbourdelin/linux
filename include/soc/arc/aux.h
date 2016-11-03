/*
 * Copyright (C) 2016-2017 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef __SOC_ARC_AUX_H__
#define __SOC_ARC_AUX_H__

#ifdef CONFIG_ARC

#define read_aux_reg(r)		__builtin_arc_lr(r)

/* gcc builtin sr needs reg param to be long immediate */
#define write_aux_reg(r, v)	__builtin_arc_sr((unsigned int)(v), r)

#else

#define read_aux_reg(r)		0
#define write_aux_reg(r, v)

#endif

#define READ_BCR(reg, into)				\
{							\
	unsigned int tmp;				\
	tmp = read_aux_reg(reg);			\
	if (sizeof(tmp) == sizeof(into)) {		\
		into = *((typeof(into) *)&tmp);		\
	} else {					\
		extern void bogus_undefined(void);	\
		bogus_undefined();			\
	}						\
}

#define WRITE_AUX(reg, into)				\
{							\
	unsigned int tmp;				\
	if (sizeof(tmp) == sizeof(into)) {		\
		tmp = (*(unsigned int *)&(into));	\
		write_aux_reg(reg, tmp);		\
	} else  {					\
		extern void bogus_undefined(void);	\
		bogus_undefined();			\
	}						\
}


#endif
