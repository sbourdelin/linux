#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ALL_TESTS="ping_ipv4"
NUM_NETIFS=4
source lib.sh

h1_create()
{
	vrf_create "vrf-h1"
	ip link set dev $h1 master vrf-h1

	ip link set dev vrf-h1 up
	ip link set dev $h1 up

	ip address add 192.0.2.2/24 dev $h1
	ip route add 198.51.100.0/24 vrf vrf-h1 nexthop via 192.0.2.1
}

h1_destroy()
{
	ip route del 198.51.100.0/24 vrf vrf-h1
	ip address del 192.0.2.2/24 dev $h1

	ip link set dev $h1 down
	vrf_destroy "vrf-h1"
}

h2_create()
{
	ip link set dev $h2 up

	ip address add 198.51.100.2/24 dev $h2
	ip route add 192.0.2.0/24 dev $h2 via 198.51.100.1
}

h2_destroy()
{
	ip route del 192.0.2.0/24 dev $h2 via 198.51.100.1
	ip address del 198.51.100.2/24 dev $h2

	ip link set dev $h2 down
}

router_create()
{
	vrf_create "vrf-r1"
	ip link set dev $rp1 master vrf-r1
	ip link set dev $rp2 master vrf-r1

	ip link set dev vrf-r1 up
	ip link set dev $rp1 up
	ip link set dev $rp2 up

	ip address add 192.0.2.1/24 dev $rp1
	ip address add 198.51.100.1/24 dev $rp2
}

router_destroy()
{
	ip address del 198.51.100.1/24 dev $rp2
	ip address del 192.0.2.1/24 dev $rp1

	ip link set dev $rp2 down
	ip link set dev $rp1 down
	vrf_destroy "vrf-r1"
}

bc_forwarding_disable()
{
	sysctl_set net.ipv4.conf.all.bc_forwarding 0
	sysctl_set net.ipv4.conf.$rp1.bc_forwarding 0
}

bc_forwarding_enable()
{
	sysctl_set net.ipv4.conf.all.bc_forwarding 1
	sysctl_set net.ipv4.conf.$rp1.bc_forwarding 1
}

bc_forwarding_restore()
{
	sysctl_restore net.ipv4.conf.$rp1.bc_forwarding
	sysctl_restore net.ipv4.conf.all.bc_forwarding
}

setup_prepare()
{
	h1=${NETIFS[p1]}
	rp1=${NETIFS[p2]}

	rp2=${NETIFS[p3]}
	h2=${NETIFS[p4]}

	vrf_prepare

	h1_create
	h2_create

	router_create

	forwarding_enable
}

cleanup()
{
	pre_cleanup

	forwarding_restore

	router_destroy

	h2_destroy
	h1_destroy

	vrf_cleanup
}

ping_ipv4()
{
	sysctl_set net.ipv4.icmp_echo_ignore_broadcasts 0
	bc_forwarding_disable
	ping_test $h1 198.51.100.255

	iptables -A INPUT -i vrf-r1 -p icmp -j DROP
	bc_forwarding_restore
	bc_forwarding_enable
	ping_test $h1 198.51.100.255

	bc_forwarding_restore
	iptables -D INPUT -i vrf-r1 -p icmp -j DROP
	sysctl_restore net.ipv4.icmp_echo_ignore_broadcasts
}

trap cleanup EXIT

setup_prepare
setup_wait

tests_run

exit $EXIT_STATUS
