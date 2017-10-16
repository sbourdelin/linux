# mem-phys-addr.py: Resolve physical address samples
# Copyright (c) 2017, Intel Corporation.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
#
# This program is distributed in the hope it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.

from __future__ import division
import os
import sys
import struct
import re
import bisect
import collections

sys.path.append(os.environ['PERF_EXEC_PATH'] + \
	'/scripts/python/Perf-Trace-Util/lib/Perf/Trace')

system_ram = []
pmem = []
f = None
load_event = ('mem-loads', '0x1cd')
store_event = ('mem-stores', '0x82d0');
load_mem_type_cnt = collections.Counter()
store_mem_type_cnt = collections.Counter()

def parse_iomem():
	global f
	f = open('/proc/iomem', 'r')
	for i, j in enumerate(f):
		m = re.split('-|:',j,2)
		if m[2].strip() == 'System RAM':
			system_ram.append(long(m[0], 16))
			system_ram.append(long(m[1], 16))
		if m[2].strip() == 'Persistent Memory':
			pmem.append(long(m[0], 16))
			pmem.append(long(m[1], 16))

def print_memory_type():
	print "Memory type summary\n"
	print "Event: mem-loads"
	print "%-40s  %10s  %10s\n" % ("Memory type", "count", "percentage"),
	print "%-40s  %10s  %10s\n" % ("----------------------------------------", \
					"-----------", "-----------"),
	total = sum(load_mem_type_cnt.values())
	for mem_type, count in sorted(load_mem_type_cnt.most_common(), \
					key = lambda(k, v): (v, k), reverse = True):
		print "%-40s  %10d  %10.1f%%\n" % (mem_type, count, 100 * count / total),
	print "\n\n"
	print "Event: mem-stores"
	print "%-40s  %10s  %10s\n" % ("Memory type", "count", "percentage"),
	print "%-40s  %10s  %10s\n" % ("----------------------------------------", \
					"-----------", "-----------"),
	total = sum(store_mem_type_cnt.values())
	for mem_type, count in sorted(store_mem_type_cnt.most_common(), \
					key = lambda(k, v): (v, k), reverse = True):
		print "%-40s  %10d  %10.1f%%\n" % (mem_type, count, 100 * count / total),

def trace_begin():
	parse_iomem()

def trace_end():
	print_memory_type()
	f.close()

def is_system_ram(phys_addr):
	#/proc/iomem is sorted
	position = bisect.bisect(system_ram, phys_addr)
	if position % 2 == 0:
		return False
	return True

def is_persistent_mem(phys_addr):
	position = bisect.bisect(pmem, phys_addr)
	if position % 2 == 0:
		return False
	return True

def find_memory_type(phys_addr):
	if phys_addr == 0:
		return "N/A"
	if is_system_ram(phys_addr):
		return "System RAM"

	if is_persistent_mem(phys_addr):
		return "Persistent Memory"

	#slow path, search all
	f.seek(0, 0)
	for j in f:
		m = re.split('-|:',j,2)
		if long(m[0], 16) <= phys_addr <= long(m[1], 16):
			return m[2]
	return "N/A"

def process_event(param_dict):
	name       = param_dict["ev_name"]
	sample     = param_dict["sample"]
	phys_addr  = sample["phys_addr"]

	if any(x in name for x in load_event):
		load_mem_type_cnt[find_memory_type(phys_addr)] += 1
	if any(x in name for x in store_event):
		store_mem_type_cnt[find_memory_type(phys_addr)] += 1
