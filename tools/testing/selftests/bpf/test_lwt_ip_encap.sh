#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Setup:
# - create VETH1/VETH2 veth
# - VETH1 gets IP_SRC
# - create netns NS
# - move VETH2 to NS, add IP_DST
# - in NS, create gre tunnel GREDEV, add IP_GRE
# - in NS, configure GREDEV to route to IP_DST from IP_SRC
# - configure route to IP_GRE via VETH1
#   (note: there is no route to IP_DST from root/init ns)
#
# Test:
# - listen on IP_DST
# - send a packet to IP_DST: the listener does not get it
# - add LWT_XMIT bpf to IP_DST that gre-encaps all packets to IP_GRE
# - send a packet to IP_DST: the listener gets it


# set -x  # debug ON
set +x  # debug OFF
set -e  # exit on error

if [[ $EUID -ne 0 ]]; then
	echo "This script must be run as root"
	echo "FAIL"
	exit 1
fi

readonly NS="ns-ip-encap-$(mktemp -u XXXXXX)"
readonly OUT=$(mktemp /tmp/test_lwt_ip_incap.XXXXXX)

readonly NET_SRC="172.16.1.0"

readonly IP_SRC="172.16.1.100"
readonly IP_DST="172.16.2.100"
readonly IP_GRE="172.16.3.100"

readonly PORT=5555
readonly MSG="foo_bar"

PID1=0
PID2=0

setup() {
	ip link add veth1 type veth peer name veth2

	ip netns add "${NS}"
	ip link set veth2 netns ${NS}

	ip link set dev veth1 up
	ip -netns ${NS} link set dev veth2 up

	ip addr add dev veth1 ${IP_SRC}/24
	ip -netns ${NS} addr add dev veth2 ${IP_DST}/24

	ip -netns ${NS} tunnel add gre_dev mode gre remote ${IP_SRC} local ${IP_GRE} ttl 255
	ip -netns ${NS} link set gre_dev up
	ip -netns ${NS} addr add ${IP_GRE} dev gre_dev
	ip -netns ${NS} route add ${NET_SRC}/24 dev gre_dev

	ip route add ${IP_GRE}/32 dev veth1
}

cleanup() {
	ip link del veth1
	ip netns del ${NS}
	if [ $PID1 -ne 0 ] ; then kill $PID1 ; fi
	if [ $PID2 -ne 0 ] ; then kill $PID2 ; fi
	rm $OUT
}

trap cleanup EXIT
setup

# start the listener
ip netns exec ${NS} nc -ul ${IP_DST} $PORT > $OUT &
PID1=$!
usleep 100000

# send a packet
echo -ne "${MSG}" | nc -u ${IP_DST} $PORT &
PID2=$!
usleep 1000000
kill $PID2
PID2=0

# confirm the packet was not delivered
if [ "$(<$OUT)" != "" ]; then
	echo "FAIL: unexpected packet"
	exit 1
fi

# install an lwt/bpf encap prog
ip route add ${IP_DST} encap bpf xmit obj test_lwt_ip_encap.o sec encap_gre dev veth1
usleep 100000

# send a packet
echo -ne "${MSG}" | nc -u ${IP_DST} $PORT &
PID2=$!
usleep 1000000
kill $PID2
PID2=0
kill $PID1
PID1=0

if [ "$(<$OUT)" != "$MSG" ]; then
	echo "FAIL"
	exit 1
fi

echo "PASS"

