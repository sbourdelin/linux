/*
 * Aptina Sensor PLL Configuration
 *
 * Copyright (C) 2012 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 * Copyright (C) 2018 Intenta GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>

#include "aptina-pll.h"

/* A variant of mult_frac that works even when (x % denom) * numer produces a
 * 32bit overflow.
 */
#define mult_frac_exact(x, numer, denom) \
	((u32)div_u64(mul_u32_u32((x), (numer)), (denom)))

static unsigned int absdiff(unsigned int x, unsigned int y)
{
	return x > y ? x - y : y - x;
}

/* Find n_min <= *np <= n_max and d_min <= *dp <= d_max such that *np / *dp
 * approximates n_target / d_target.
 */
static int approximate_fraction(unsigned int n_min, unsigned int n_max,
				unsigned int d_min, unsigned int d_max,
				unsigned int n_target, unsigned int d_target,
				unsigned int *np, unsigned int *dp)
{
	unsigned int d, best_err = UINT_MAX;

	d_min = max(d_min, mult_frac_exact(n_min, d_target, n_target));
	d_max = min(d_max, mult_frac_exact(n_max, d_target, n_target) + 1);
	if (d_min > d_max)
		return -EINVAL;

	for (d = d_min; d < d_max; ++d) {
		unsigned int n = mult_frac_exact(d, n_target, d_target);

		if (n >= n_min) {
			unsigned int err = absdiff(
				n_target, mult_frac_exact(n, d_target, d));

			if (err < best_err) {
				best_err = err;
				*np = n;
				*dp = d;
			}
			if (err == 0)
				return 0;
		}
		++n;
		if (n <= n_max) {
			unsigned int err = absdiff(
				n_target, mult_frac_exact(n, d_target, d));

			if (err < best_err) {
				best_err = err;
				*np = n;
				*dp = d;
			}
		}
	}
	return best_err == UINT_MAX ? -EINVAL : 0;
}

/* Find parameters n, m, p1 such that:
 *   n_min <= n <= n_max
 *   m_min <= m <= m_max
 *   p1_min <= p1 <= p1_max, even
 *   int_clock_min <= ext_clock / n <= int_clock_max
 *   out_clock_min <= ext_clock / n * m <= out_clock_max
 *   pix_clock = ext_clock / n * m / p1 (approximately)
 *   p1 is maximized
 * Report the achieved pix_clock (input/output parameter).
 */
int aptina_pll_calculate(struct device *dev,
			 const struct aptina_pll_limits *limits,
			 struct aptina_pll *pll)
{
	unsigned int n_min, n_max, m_min, m_max, p1_min, p1_max, p1;
	unsigned int clock_err = UINT_MAX;

	dev_dbg(dev, "PLL: ext clock %u pix clock %u\n",
		pll->ext_clock, pll->pix_clock);

	if (pll->ext_clock < limits->ext_clock_min ||
	    pll->ext_clock > limits->ext_clock_max) {
		dev_err(dev, "pll: invalid external clock frequency.\n");
		return -EINVAL;
	}

	if (pll->pix_clock == 0 || pll->pix_clock > limits->pix_clock_max) {
		dev_err(dev, "pll: invalid pixel clock frequency.\n");
		return -EINVAL;
	}

	/* int_clock_min <= ext_clock / N <= int_clock_max */
	n_min = max(limits->n_min,
		    DIV_ROUND_UP(pll->ext_clock, limits->int_clock_max));
	n_max = min(limits->n_max,
		    pll->ext_clock / limits->int_clock_min);

	if (n_min > n_max) {
		dev_err(dev,
			"pll: no divisor N results in a valid int_clock.\n");
		return -EINVAL;
	}

	/* out_clock_min <= ext_clock / N * M <= out_clock_max */
	m_min = max(limits->m_min,
		    mult_frac(limits->out_clock_min, n_min, pll->ext_clock));
	m_max = min(limits->m_max,
		    mult_frac(limits->out_clock_max, n_max, pll->ext_clock));
	if (m_min > m_max) {
		dev_err(dev,
			"pll: no multiplier M results in a valid out_clock.\n");
		return -EINVAL;
	}

	/* Using limits of m, we can further shrink the range of n. */
	n_min = max(n_min,
		    mult_frac(pll->ext_clock, m_min, limits->out_clock_max));
	n_max = max(n_max,
		    mult_frac(pll->ext_clock, m_max, limits->out_clock_min));
	if (n_min > n_max) {
		dev_err(dev,
			"pll: no divisor N results in a valid out_clock.\n");
		return -EINVAL;
	}

	dev_dbg(dev, "pll: %u <= N <= %u\n", n_min, n_max);
	dev_dbg(dev, "pll: %u <= M <= %u\n", m_min, m_max);

	/* out_clock_min <= pix_clock * P1 <= out_clock_max */
	p1_min = max(limits->p1_min,
		     DIV_ROUND_UP(limits->out_clock_min, pll->pix_clock));
	p1_max = min(limits->p1_max,
		     limits->out_clock_max / pll->pix_clock);
	/* pix_clock = ext_clock / N * M / P1 */
	p1_min = max(p1_min, DIV_ROUND_UP(pll->ext_clock * m_min,
					  pll->pix_clock * n_max));
	p1_max = min(p1_max,
		     pll->ext_clock * m_max / (pll->pix_clock * n_min));
	if (p1_min > p1_max) {
		dev_err(dev, "pll: no valid P1 divisor.\n");
		return -EINVAL;
	}

	dev_dbg(dev, "pll: %u <= P1 <= %u\n", p1_min, p1_max);

	for (p1 = p1_max & ~1; p1 >= p1_min; p1 -= 2) {
		unsigned int n = 0, m = UINT_MAX, out_clock, err;
		const unsigned int target_out_clock = pll->pix_clock * p1;

		/* target_out_clock = ext_clock / N * M */
		if (approximate_fraction(n_min, n_max, m_min, m_max,
					 pll->ext_clock, target_out_clock,
					 &n, &m) < 0)
			continue;

		/* We must check all conditions due to possible rounding
		 * errors:
		 *   int_clock_min <= ext_clock / N <= int_clock_max
		 *   out_clock_min <= ext_clock / N * M <= out_clock_max
		 */
		out_clock = mult_frac(pll->ext_clock, m, n);
		if (pll->ext_clock < limits->int_clock_min * n ||
		    pll->ext_clock > limits->int_clock_max * n ||
		    out_clock < limits->out_clock_min ||
		    out_clock > limits->out_clock_max)
			continue;

		err = absdiff(out_clock / p1, pll->pix_clock);
		if (err < clock_err) {
			pll->n = n;
			pll->m = m;
			pll->p1 = p1;
			clock_err = err;
		}
		if (err == 0) {
			dev_dbg(dev, "pll: N %u M %u P1 %u exact\n",
				pll->n, pll->m, pll->p1);
			return 0;
		}
	}
	if (clock_err == UINT_MAX) {
		dev_err(dev, "pll: no valid parameters found.\n");
		return -EINVAL;
	}
	pll->pix_clock = mult_frac(pll->ext_clock, pll->m, pll->n * pll->p1);
	dev_dbg(dev, "pll: N %u M %u P1 %u pix_clock %u Hz error %u Hz\n",
		pll->n, pll->m, pll->p1, pll->pix_clock, clock_err);
	return 0;
}
EXPORT_SYMBOL_GPL(aptina_pll_calculate);

MODULE_DESCRIPTION("Aptina PLL Helpers");
MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_LICENSE("GPL v2");
