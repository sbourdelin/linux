#!/bin/bash

function config_device {
	ip netns add at_ns0
	ip link add veth0 type veth peer name veth0b
	ip link set veth0b up
	ip link set veth0 netns at_ns0
	ip netns exec at_ns0 ip addr add 172.16.1.100/24 dev veth0
	ip netns exec at_ns0 ip addr add 2401:db00::1/64 dev veth0 nodad
	ip netns exec at_ns0 ip link set dev veth0 up
	ip addr add 172.16.1.101/24 dev veth0b
	ip addr add 2401:db00::2/64 dev veth0b nodad
}

function attach_bpf {
	rm -rf /tmp/cgroupv2
	mkdir -p /tmp/cgroupv2
	mount -t cgroup2 none /tmp/cgroupv2
	mkdir -p /tmp/cgroupv2/foo
	test_cgrp2_sock -c /tmp/cgroupv2/foo -d veth0b
	echo $$ >> /tmp/cgroupv2/foo/cgroup.procs
}

function cleanup {
	set +ex
	ip link del veth0b
	ip netns delete at_ns0
	umount /tmp/cgroupv2
	rm -rf /tmp/cgroupv2
	set -ex
}

function do_test {
	test_cgrp2_sock -4 -d veth0b
	test_cgrp2_sock -6 -d veth0b
}

function do_neg_test {
	ip netns exec at_ns0 test_cgrp2_sock -4 -n -d veth0b
	ip netns exec at_ns0 test_cgrp2_sock -6 -n -d veth0b
}

cleanup 2>/dev/null
config_device
attach_bpf
do_test
do_neg_test
cleanup
echo "*** PASS ***"
