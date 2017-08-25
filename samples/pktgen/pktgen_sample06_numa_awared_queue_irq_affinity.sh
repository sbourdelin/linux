#!/bin/bash
#
# Multiqueue: Using pktgen threads for sending on multiple CPUs
#  * adding devices to kernel threads which are in the same NUMA node
#  * bound devices queue's irq affinity to the threads, 1:1 mapping
#  * notice the naming scheme for keeping device names unique
#  * nameing scheme: dev@thread_number
#  * flow variation via random UDP source port
#
basedir=`dirname $0`
source ${basedir}/functions.sh
root_check_run_with_sudo "$@"
#
# Required param: -i dev in $DEV
source ${basedir}/parameters.sh

get_iface_node()
{
	echo `cat /sys/class/net/$1/device/numa_node`
}

get_iface_irqs()
{
	local IFACE=$1
	local queues="${IFACE}-.*TxRx"

	irqs=$(grep "$queues" /proc/interrupts | cut -f1 -d:)
	[ -z "$irqs" ] && irqs=$(grep $IFACE /proc/interrupts | cut -f1 -d:)
	[ -z "$irqs" ] && irqs=$(for i in `ls -Ux /sys/class/net/$IFACE/device/msi_irqs` ;\
		do grep "$i:.*TxRx" /proc/interrupts | grep -v fdir | cut -f 1 -d : ;\
	    done)
	[ -z "$irqs" ] && echo "Error: Could not find interrupts for $IFACE"

	echo $irqs
}

get_node_cpus()
{
	local node=$1
	local node_cpu_list
	local node_cpu_range_list=`cut -f1- -d, --output-delimiter=" " \
			/sys/devices/system/node/node$node/cpulist`

	for cpu_range in $node_cpu_range_list
	do
		node_cpu_list="$node_cpu_list "`seq -s " " ${cpu_range//-/ }`
	done

	echo $node_cpu_list
}


# Base Config
DELAY="0"        # Zero means max speed
COUNT="20000000"   # Zero means indefinitely
[ -z "$CLONE_SKB" ] && CLONE_SKB="0"

# Flow variation random source port between min and max
UDP_MIN=9
UDP_MAX=109

node=`get_iface_node $DEV`
irq_array=(`get_iface_irqs $DEV`)
cpu_array=(`get_node_cpus $node`)

[ $THREADS -gt ${#irq_array[*]} -o $THREADS -gt ${#cpu_array[*]}  ] && \
	err 1 "Thread number $THREADS exceeds: min (${#irq_array[*]},${#cpu_array[*]})"

# (example of setting default params in your script)
if [ -z "$DEST_IP" ]; then
    [ -z "$IP6" ] && DEST_IP="198.18.0.42" || DEST_IP="FD00::1"
fi
[ -z "$DST_MAC" ] && DST_MAC="90:e2:ba:ff:ff:ff"

# General cleanup everything since last run
pg_ctrl "reset"

# Threads are specified with parameter -t value in $THREADS
for ((i = 0; i < $THREADS; i++)); do
    # The device name is extended with @name, using thread number to
    # make then unique, but any name will do.
    # Set the queue's irq affinity to this $thread (processor)
    thread=${cpu_array[$i]}
    dev=${DEV}@${thread}
    echo $thread > /proc/irq/${irq_array[$i]}/smp_affinity_list
    echo "irq ${irq_array[$i]} is set affinity to `cat /proc/irq/${irq_array[$i]}/smp_affinity_list`"

    # Add remove all other devices and add_device $dev to thread
    pg_thread $thread "rem_device_all"
    pg_thread $thread "add_device" $dev

    # select queue and bind the queue and $dev in 1:1 relationship
    queue_num=$i
    echo "queue number is $queue_num"
    pg_set $dev "queue_map_min $queue_num"
    pg_set $dev "queue_map_max $queue_num"

    # Notice config queue to map to cpu (mirrors smp_processor_id())
    # It is beneficial to map IRQ /proc/irq/*/smp_affinity 1:1 to CPU number
    pg_set $dev "flag QUEUE_MAP_CPU"

    # Base config of dev
    pg_set $dev "count $COUNT"
    pg_set $dev "clone_skb $CLONE_SKB"
    pg_set $dev "pkt_size $PKT_SIZE"
    pg_set $dev "delay $DELAY"

    # Flag example disabling timestamping
    pg_set $dev "flag NO_TIMESTAMP"

    # Destination
    pg_set $dev "dst_mac $DST_MAC"
    pg_set $dev "dst$IP6 $DEST_IP"

    # Setup random UDP port src range
    pg_set $dev "flag UDPSRC_RND"
    pg_set $dev "udp_src_min $UDP_MIN"
    pg_set $dev "udp_src_max $UDP_MAX"
done

# start_run
echo "Running... ctrl^C to stop" >&2
pg_ctrl "start"
echo "Done" >&2

# Print results
for ((i = 0; i < $THREADS; i++)); do
    thread=${cpu_array[$i]}
    dev=${DEV}@${thread}
    echo "Device: $dev"
    cat /proc/net/pktgen/$dev | grep -A2 "Result:"
done
