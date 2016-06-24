/*
 * See include/linux/clk-provider.h for more information.
 */

#ifndef __DT_BINDINGS_CLK_CLK_H
#define __DT_BINDINGS_CLK_CLK_H

#define BIT(nr)	(1UL << (nr))

#define CLK_SET_RATE_GATE		BIT(0)
#define CLK_SET_PARENT_GATE		BIT(1)
#define CLK_SET_RATE_PARENT		BIT(2)
#define CLK_IGNORE_UNUSED		BIT(3)
#define CLK_IS_BASIC			BIT(5)
#define CLK_GET_RATE_NOCACHE		BIT(6)
#define CLK_SET_RATE_NO_REPARENT	BIT(7)
#define CLK_GET_ACCURACY_NOCACHE	BIT(8)
#define CLK_RECALC_NEW_RATES		BIT(9)
#define CLK_SET_RATE_UNGATE		BIT(10)
#define CLK_IS_CRITICAL			BIT(11)

#endif
