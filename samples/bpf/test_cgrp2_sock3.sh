#!/bin/sh

# Verify socket options inherited by bpf programs attached
# to a cgroup.

CGRP_MNT="/tmp/cgroupv2-test_cgrp2_sock"

################################################################################
#
print_result()
{
	printf "%50s    [%4s]\n" "$1" "$2"
}

check_sock()
{
	out=$(test_cgrp2_sock)
	echo $out | grep -q "$1"
	if [ $? -ne 0 ]; then
		print_result "IPv4: $2" "FAIL"
		echo "    expected: $1"
		echo "        have: $out"
		rc=1
	else
		print_result "IPv4: $2" "OK"
	fi
}

check_sock6()
{
	out=$(test_cgrp2_sock -6)
	echo $out | grep -q "$1"
	if [ $? -ne 0 ]; then
		print_result "IPv6: $2" "FAIL"
		echo "    expected: $1"
		echo "        have: $out"
		rc=1
	else
		print_result "IPv6: $2" "OK"
	fi
}

################################################################################
#
setup()
{
	ip li add cgrp2_sock type dummy

	set -e

	mkdir -p ${CGRP_MNT}
	mount -t cgroup2 none ${CGRP_MNT}

	mkdir -p ${CGRP_MNT}/cgrp_sock_test/prio/mark/dev

	test_cgrp2_sock -p 123 ${CGRP_MNT}/cgrp_sock_test/prio
	test_cgrp2_sock -m 666 ${CGRP_MNT}/cgrp_sock_test/prio/mark
	test_cgrp2_sock -b cgrp2_sock ${CGRP_MNT}/cgrp_sock_test/prio/mark/dev

	set +e
}

cleanup()
{
	ip li del cgrp2_sock

	echo $$ >> ${CGRP_MNT}/cgroup.procs
	rmdir ${CGRP_MNT}/cgrp_sock_test/prio/mark/dev
	rmdir ${CGRP_MNT}/cgrp_sock_test/prio/mark
	rmdir ${CGRP_MNT}/cgrp_sock_test/prio
	rmdir ${CGRP_MNT}/cgrp_sock_test

	umount ${CGRP_MNT}
}

################################################################################
# main

rc=0

setup

# set pid into first cgroup. socket should show it
# has a priority but not a mark or device bind
echo $$ > ${CGRP_MNT}/cgrp_sock_test/prio/cgroup.procs
check_sock "dev , mark 0, priority 123" "Priority only"


# set pid into second group. socket should show it
# has a priority and mark but not a device bind
echo $$ > ${CGRP_MNT}/cgrp_sock_test/prio/mark/cgroup.procs
check_sock "dev , mark 666, priority 123" "Priority + mark"

# set pid into inner group. socket should show it
# has a priority, mark and a device bind
echo $$ > ${CGRP_MNT}/cgrp_sock_test/prio/mark/dev/cgroup.procs
check_sock "dev cgrp2_sock, mark 666, priority 123" "Priority + mark + dev"

echo

# set pid into first cgroup. socket should show it
# has a priority but not a mark or device bind
echo $$ > ${CGRP_MNT}/cgrp_sock_test/prio/cgroup.procs
check_sock6 "dev , mark 0, priority 123" "Priority only"

# set pid into second group. socket should show it
# has a priority and mark but not a device bind
echo $$ > ${CGRP_MNT}/cgrp_sock_test/prio/mark/cgroup.procs
check_sock6 "dev , mark 666, priority 123" "Priority + mark"

# set pid into inner group. socket should show it
# has a priority, mark and a device bind
echo $$ > ${CGRP_MNT}/cgrp_sock_test/prio/mark/dev/cgroup.procs
check_sock6 "dev cgrp2_sock, mark 666, priority 123" "Priority + mark + dev"

cleanup

exit $rc
