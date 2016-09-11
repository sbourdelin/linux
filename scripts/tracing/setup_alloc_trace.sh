#! /bin/bash

# This script does all the basic setup necessary for allocation_postprocess.py
# and then invokes the script. All the setup that is done at the beginning
# is cleared on exit.

# Usage: # ./setup_alloc_trace.sh -t THRESHOLD_IN_US -s
# path/to/tracing/directory > path/to/output/file.

while getopts :t:s: name
do
	case $name in
		t)threshold=$OPTARG;;
		s)trace_dir=$OPTARG;;
		*) echo "Usage: ./setup_alloc_trace.sh -t THRESHOLD_IN_US -s path/to/tracing/directory"
		esac
done

if [[ -z $threshold ]]
then
	echo "Must specify threshold."
	exit 1
fi

if [[ -z $trace_dir ]]
then
	trace_dir="/sys/kernel/debug/tracing"
fi

pwd=`pwd`
cd $trace_dir
echo 0 > tracing_on

echo function_graph > current_tracer
echo funcgraph-proc > trace_options
echo funcgraph-abstime > trace_options

# set filter functions
echo __alloc_pages_nodemask > set_ftrace_filter
echo try_to_free_pages >> set_ftrace_filter
echo mem_cgroup_soft_limit_reclaim >> set_ftrace_filter
echo shrink_node >> set_ftrace_filter
echo shrink_node_memcg >> set_ftrace_filter
echo shrink_slab >> set_ftrace_filter
echo shrink_active_list >> set_ftrace_filter
echo shrink_inactive_list >> set_ftrace_filter
echo compact_zone >> set_ftrace_filter
echo try_to_compact_pages >> set_ftrace_filter

echo $threshold > tracing_thresh

# set tracepoints
echo 1 > events/vmscan/mm_shrink_slab_start/enable
echo 1 > events/vmscan/mm_shrink_slab_end/enable
echo 1 > events/vmscan/mm_vmscan_direct_reclaim_begin/enable
echo 1 > events/vmscan/mm_vmscan_direct_reclaim_end/enable
echo 1 > events/vmscan/mm_vmscan_lru_shrink_inactive/enable
echo 1 > events/compaction/mm_compaction_begin/enable
echo 1 > events/compaction/mm_compaction_end/enable
echo 1 > events/compaction/mm_compaction_try_to_compact_pages/enable
echo 1 > tracing_on

cd $pwd

./allocation_postprocess.py -t $threshold -s $trace_dir

function cleanup {
	cd $trace_dir
	echo 0 > tracing_on
	echo nop > current_tracer
	echo > set_ftrace_filter
	echo 0 > tracing_thresh

	echo 0 > events/vmscan/mm_shrink_slab_start/enable
	echo 0 > events/vmscan/mm_shrink_slab_end/enable
	echo 0 > events/vmscan/mm_vmscan_direct_reclaim_begin/enable
	echo 0 > events/vmscan/mm_vmscan_direct_reclaim_end/enable
	echo 0 > events/vmscan/mm_vmscan_lru_shrink_inactive/enable
	echo 0 > events/compaction/mm_compaction_begin/enable
	echo 0 > events/compaction/mm_compaction_end/enable
	echo 0 > events/compaction/mm_compaction_try_to_compact_pages/enable

	exit $?
}

trap cleanup SIGINT
trap cleanup SIGTERM
trap cleanup EXIT
