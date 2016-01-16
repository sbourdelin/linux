/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#ifndef __XEN_PUBLIC_IO_CLKIF_H__
#define __XEN_PUBLIC_IO_CLKIF_H__

#include <xen/interface/io/ring.h>
#include <xen/interface/grant_table.h>

/**/
enum {
	XENCLK_PREPARE,		/* clk_prepare_enable */
	XENCLK_UNPREPARE,	/* clk_unprepare_disable */
	XENCLK_GET_RATE,	/* clk_get_rate */
	XENCLK_SET_RATE,	/* clk_set_rate */
	XENCLK_END,
};

struct xen_clkif_request {
	int id;
	unsigned long rate;
	char clk_name[32];
};

struct xen_clkif_response {
	int id;
	int success;
	unsigned long rate;
	char clk_name[32];
};

DEFINE_RING_TYPES(xen_clkif, struct xen_clkif_request, struct xen_clkif_response);
#define XEN_CLK_RING_SIZE __CONST_RING_SIZE(xen_clkif, PAGE_SIZE)

#endif
