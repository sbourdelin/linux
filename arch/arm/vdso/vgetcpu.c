/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/compiler.h>
#include <asm/topology.h>

struct getcpu_cache;

notrace int __vdso_getcpu(unsigned int *cpup, unsigned int *nodep,
			  struct getcpu_cache *tcache)
{
	unsigned long node_and_cpu;

	asm("mrc p15, 0, %0, c13, c0, 2\n" : "=r"(node_and_cpu));

	if (nodep)
		*nodep = cpu_to_node(node_and_cpu >> 16);
	if (cpup)
		*cpup  = node_and_cpu & 0xffffUL;

	return 0;
}

