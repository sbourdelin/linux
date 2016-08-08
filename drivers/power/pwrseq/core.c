/*
 * core.c	power sequence core file
 *
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 * Author: Peter Chen <peter.chen@nxp.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/power/pwrseq.h>

static DEFINE_MUTEX(pwrseq_list_mutex);
static LIST_HEAD(pwrseq_list);

int pwrseq_get(struct device_node *np, struct pwrseq *p)
{
	if (p && p->get)
		return p->get(np, p);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(pwrseq_get);

int pwrseq_on(struct device_node *np, struct pwrseq *p)
{
	if (p && p->on)
		return p->on(np, p);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(pwrseq_on);

void pwrseq_off(struct pwrseq *p)
{
	if (p && p->off)
		p->off(p);
}
EXPORT_SYMBOL(pwrseq_off);

void pwrseq_put(struct pwrseq *p)
{
	if (p && p->put)
		p->put(p);
}
EXPORT_SYMBOL(pwrseq_put);

void pwrseq_free(struct pwrseq *p)
{
	if (p && p->free)
		p->free(p);
}
EXPORT_SYMBOL(pwrseq_free);
