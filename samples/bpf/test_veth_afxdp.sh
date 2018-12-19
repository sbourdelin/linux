#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# The script runs the sender at the root namespace, and
# the receiver at the namespace at_ns0, with different mode
#  1. XDP_DROP
#  2. XDP_TX
#  3. XDP_PASS
#  4. XDP_REDIRECT
#  5. Generic XDP

XDPSOCK=./xdpsock
XDP_RXQ_INFO=./xdp_rxq_info

ip netns add at_ns0
ip link add p0 type veth peer name p1
ip link set p0 netns at_ns0
ip link set dev p1 up
ip netns exec at_ns0 ip link set dev p0 up

# send at root namespace
test_xdp_drop()
{
	echo "[Peer] XDP_DROP"
	ip netns exec at_ns0 $XDP_RXQ_INFO --dev p0 --action XDP_DROP &
	$XDPSOCK -i p1 -t -N -z &> /tmp/t &> /dev/null &
}

test_xdp_pass()
{
	echo "[Peer] XDP_PASS"
	ip netns exec at_ns0 $XDP_RXQ_INFO --dev p0 --action XDP_PASS &
	$XDPSOCK -i p1 -t -N -z &> /tmp/t &> /dev/null &
}

test_xdp_tx()
{
	echo "[Peer] XDP_TX"
	ip netns exec at_ns0 $XDP_RXQ_INFO --dev p0 --action XDP_TX &
	$XDPSOCK -i p1 -t -N -z &> /tmp/t &> /dev/null &
}

test_generic_xdp()
{
	echo "[Peer] Generic XDP"
	ip netns exec at_ns0 $XDPSOCK -i p0 -r -S &
	$XDPSOCK -i p1 -t -N -z &> /tmp/t &> /dev/null &
}

test_xdp_redirect()
{
	echo "[Peer] XDP_REDIRECT"
	ip netns exec at_ns0 $XDPSOCK -i p0 -r -N &
	$XDPSOCK -i p1 -t -N -z &> /tmp/t &> /dev/null &
}

cleanup() {
    killall xdpsock
    killall xdp_rxq_info
    ip netns del at_ns0
    ip link del p1
}

trap cleanup 0 3 6

test_xdp_drop
cleanup
