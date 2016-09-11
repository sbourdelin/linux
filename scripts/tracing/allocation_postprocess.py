#!/usr/bin/env python
# python 2.7

"""
This script uses function graph and some of the existing
tracepoints to help people observe how long page allocations and some
functions in the direct reclaim and compaction paths take.
The functions and tracepoints enabled can be seen in setup_alloc_trace.sh.
It reads from trace_pipe of the tracing directory and prints
only those tracepoints and functions which are associated
with latencies greater than the threshold specified.
When CTRL+C is pressed, the times spent in the various shrinkers are displayed.
The setup of trace is done in setup_alloc_trace.sh, from where this script is
invoked.
"""

import argparse
import re
import sys
import signal

from collections import defaultdict

# Constants for tracepoints

DIRECT_RECLAIM_BEGIN        = 1
DIRECT_RECLAIM_END          = 2
SHRINK_SLAB_START           = 3
SHRINK_INACTIVE_LIST        = 5
TRY_TO_COMPACT              = 6
COMPACTION_BEGIN            = 7
COMPACTION_END              = 8

SECS_TO_US                  = 1000000
US_TO_MS                    = 1000

# Parse command line arguments
parser = argparse.ArgumentParser(description='Parser for latency analyzer')

parser.add_argument('-s', '--source', action='store',
                    default='/sys/kernel/debug/tracing',
                    dest='source_path',
                    help='Specify source file to read trace output')
parser.add_argument('-t', '--threshold', action='store', default=0.0,
                    dest='threshold', type=float)
args = parser.parse_args()

source_path = ''.join((args.source_path,'/trace_pipe'))
threshold = args.threshold

# Regexes
line_pattern = re.compile(r'\s*(\d+\.\d+)\s+\|\s+\d*\)*\s+([\w-]+)\s+\|\s+.*\s+(\d*\.*\d*)\s+[us]{0,2}\s+\|\s+(.*)')
tracepoint_pattern = re.compile(r'\/\*\s*([\w]*):\s*(.*)\s*\*\/')
shrinker_pattern = re.compile(r'\s*([\w]*)\+(.*)\s*')
function_end_pattern = re.compile(r'.*\/\*\s*([\w]*)\s*\*\/')

# The dictionary which holds tracepoint information for all processes
all_information = defaultdict(dict)

# The dictionary which holds shrinker latencies
shrinker_latencies = defaultdict(float)
shrinker_max_latencies = defaultdict(float)

def print_shrinker_latencies(signum, frame):
    """ This function prints the time spent in each shrinker, when CTRL+C is 
    pressed.
    """

    signal.signal(signal.SIGINT, original_sigint)
    for key, value in shrinker_latencies.iteritems():
        print '%s : total time = %f ms, max latency = %f ms' %(key,
                                        value*US_TO_MS,
                                        shrinker_max_latencies[key]*US_TO_MS)
    sys.exit(0)

original_sigint = signal.getsignal(signal.SIGINT)
signal.signal(signal.SIGINT, print_shrinker_latencies)


def set_begin_info(process, EVENT, timestamp, info):
    """ This function sets information associated with mm_shrink_slab_start.
    It sets the entire tracepoint info, along with the timestamp, which will
    be used to calculate latencies when corresponding mm_ shrink_slab_end
    tracepoints are encountered.
    """

    per_process_dict = all_information[process]
    begin_info = {}
    begin_info["data"] = info
    begin_info["time"] = timestamp
    per_process_dict[EVENT] = begin_info


def set_trace_info(process, EVENT, info):
    """ This function sets trace information associated with specific events.
    """

    per_process_dict = all_information[process]
    per_process_dict[EVENT] = info


def store_max_latency(shrinker_name, latency):
    """ This function stores the maximum latency encountered in a shrinker. """

    max_latency = shrinker_max_latencies[shrinker_name]
    if latency > max_latency:
        shrinker_max_latencies[shrinker_name] = latency


def find_latency(process, BEGIN_EVENT, timestamp):
    """ This function calculates shrinker latencies."""

    per_process_dict = all_information.get(process, None)
    if per_process_dict:
        begin_info = per_process_dict.get(BEGIN_EVENT, None)
        if begin_info:
            begin_data = begin_info.get("data", None)
            begin_time = begin_info.get("time", None)
            if begin_time:
                time_elapsed = float(timestamp) - float(begin_time)
                if time_elapsed*SECS_TO_US > threshold:
                    return (True, begin_data, time_elapsed)
                return (False, begin_data, time_elapsed)
    return (False, None, 0.0)


def print_line(line_info):
    print line_info,


def print_tracepoint(process, EVENT, info):
    if info:
        print info,
    else:
        per_process_dict = all_information.get(process, None)
        TP_info = per_process_dict.get(EVENT, None)
        if TP_info:
            print TP_info,
        per_process_dict.pop(EVENT, None)

with open(source_path) as f:
    for line in f:
        line_match = re.match(line_pattern, line)
        if line_match:
            timestamp = line_match.group(1)
            process_info = line_match.group(2)
            function_match = re.match(function_end_pattern, line_match.group(4))
            tracepoint_match = re.match(tracepoint_pattern, line_match.group(4))
            if tracepoint_match:
                TP_whole = line_match.group(4)
                TP_name = tracepoint_match.group(1)
                TP_info = tracepoint_match.group(2)


                def call_set_trace_info(EVENT):
                    set_trace_info(process_info, EVENT, line)


                def direct_reclaim_b():
                    call_set_trace_info(DIRECT_RECLAIM_BEGIN)


                def direct_reclaim_e():
                    call_set_trace_info(DIRECT_RECLAIM_END)


                def shrink_inactive_list():
                    call_set_trace_info(SHRINK_INACTIVE_LIST)


                def shrink_slab_b():
                    set_begin_info(process_info, SHRINK_SLAB_START, timestamp,
                                    line)


                def shrink_slab_e():
                    delay_status, begin_data, time_elapsed = find_latency(
                                                                process_info,
                                                                SHRINK_SLAB_START,
                                                                timestamp)
                    shrinker_match = re.match(shrinker_pattern, TP_info)
                    if shrinker_match:
                        shrinker_name = shrinker_match.group(1)
                        shrinker_latencies[shrinker_name] += time_elapsed
                        store_max_latency(shrinker_name, time_elapsed)

                    if delay_status:
                        print_tracepoint(process_info,
                                         SHRINK_SLAB_START,
                                         begin_data)
                        print_tracepoint(process_info,
                                         None,
                                         line)


                def try_to_compact():
                    call_set_trace_info(TRY_TO_COMPACT)


                def compact_b():
                    call_set_trace_info(COMPACTION_BEGIN)


                def compact_e():
                    call_set_trace_info(COMPACTION_END)


                trace_match = {'mm_vmscan_direct_reclaim_begin' : direct_reclaim_b,
                               'mm_vmscan_direct_reclaim_end'   : direct_reclaim_e,
                               'mm_shrink_slab_start'           : shrink_slab_b,
                               'mm_shrink_slab_end'             : shrink_slab_e,
                               'mm_vmscan_lru_shrink_inactive'  :
                                                              shrink_inactive_list,
                               'mm_compaction_try_to_compact_pages':
                                                              try_to_compact,
                               'mm_compaction_begin'            : compact_b,
                               'mm_compaction_end'              : compact_e}

                if TP_name in trace_match:
                    trace_match[TP_name]()
                else:
                    pass

            else:
                function_match = re.match(function_end_pattern,
                                          line_match.group(4))
                if function_match:
                    function_name = function_match.group(1)


                    def alloc_pages():
                        print_line(line)
                        all_information.pop(process_info, None)


                    def try_to_free_pages():
                        print_tracepoint(process_info, DIRECT_RECLAIM_BEGIN, None)
                        print_tracepoint(process_info, DIRECT_RECLAIM_END, None)
                        print_line(line)


                    def shrink_inactive_list():
                        print_tracepoint(process_info, SHRINK_INACTIVE_LIST, None)
                        print_line(line)


                    def try_to_compact():
                        print_tracepoint(process_info, TRY_TO_COMPACT, None)
                        print_line(line)


                    def compact_zone():
                        print_tracepoint(process_info, COMPACTION_BEGIN, None)
                        print_tracepoint(process_info, COMPACTION_END, None)
                        print_line(line)


                    f_match = {'__alloc_pages_nodemask' : alloc_pages,
                               'try_to_free_pages'      : try_to_free_pages,
                               'shrink_inactive_list'   : shrink_inactive_list,
                               'try_to_compact'         : try_to_compact,
                               'compact_zone'           : compact_zone}

                    if function_name in f_match:
                        f_match[function_name]()
                    else:
                        print_line(line)
