#!/bin/sh
#
# This test is for checking network interface
# For the moment it tests only ethernet interface (but wifi could be easily added)
#
# We assume that all network driver are loaded
# if not they probably have failed earlier in the boot process and their logged error will be catched by another test
#

# this function will try to up the interface
# if already up, nothing done
# arg1: network interface name
kci_net_start()
{
	netdev=$1

	ip link show "$netdev" |grep -q UP
	if [ $? -eq 0 ];then
		echo "SKIP: interface $netdev already up"
		return 0
	fi

	ip link set "$netdev" up
	if [ $? -ne 0 ];then
		echo "FAIL: Fail to up $netdev"
		return 1
	else
		echo "PASS: set interface $netdev up"
		NETDEV_STARTED=1
	fi
	return 0
}

# this function will try to setup an IP and MAC address on a network interface
# Doing nothing if the interface was already up
# arg1: network interface name
kci_net_setup()
{
	netdev=$1

	# do nothing if the interface was already up
	if [ $NETDEV_STARTED -eq 0 ];then
		return 0
	fi

	ip link set dev $netdev address 02:03:04:05:06:07
	if [ $? -ne 0 ];then
		echo "FAIL: Cannot set MAC address to $netdev"
		return 1
	fi
	echo "PASS: set MAC address to $netdev"

	#check that the interface did not already have an IP
	ip address show "$netdev" |grep '^[[:space:]]*inet'
	if [ $? -eq 0 ];then
		echo "SKIP: $netdev already have an IP"
		return 0
	fi

	# TODO what ipaddr to set ? DHCP ?
	echo "SKIP: set IP address to $netdev"
	return 0
}

# test an ethtool command
# arg1: return code for not supported (see ethtool code source)
# arg2: summary of the command
# arg3: command to execute
kci_netdev_ethtool_test()
{
	if [ $# -le 2 ];then
		echo "SKIP: invalid number of arguments"
		return 1
	fi
	$3 >/dev/null
	ret=$?
	if [ $ret -ne 0 ];then
		if [ $ret -eq "$1" ];then
			echo "SKIP: ethtool $2 $netdev not supported"
		else
			echo "FAIL: ethtool $2 $netdev"
			return 1
		fi
	else
		echo "PASS: ethtool $2 $netdev"
	fi
	return 0
}

# test ethtool commands
# arg1: network interface name
kci_netdev_ethtool()
{
	netdev=$1

	#check presence of ethtool
	ethtool --version 2>/dev/null >/dev/null
	if [ $? -ne 0 ];then
		echo "SKIP: ethtool not present"
		return 1
	fi

	TMP_ETHTOOL_FEATURES="$(mktemp)"
	if [ ! -e "$TMP_ETHTOOL_FEATURES" ];then
		echo "SKIP: Cannot create a tmp file"
		return 1
	fi

	ethtool -k "$netdev" > "$TMP_ETHTOOL_FEATURES"
	if [ $? -ne 0 ];then
		echo "FAIL: ethtool -k $netdev"
		rm "$TMP_ETHTOOL_FEATURES"
		return 1
	fi
	#TODO for each non fixed features, try to turn them on/off
	rm "$TMP_ETHTOOL_FEATURES"

	kci_netdev_ethtool_test 74 'dump' "ethtool -d $netdev"
	kci_netdev_ethtool_test 94 'stats' "ethtool -S $netdev"
	return 0
}

# stop a netdev
# arg1: network interface name
kci_netdev_stop()
{
	netdev=$1

	if [ $NETDEV_STARTED -eq 0 ];then
		echo "SKIP: interface $netdev kept up"
		return 0
	fi

	ip link set "$netdev" down
	if [ $? -ne 0 ];then
		echo "FAIL: stop $netdev"
		return 1
	fi
	echo "PASS: interface $netdev stop"
	return 0
}

# run all test on a netdev
# arg1: network interface name
kci_test_netdev()
{
	netdev=$1

	NETDEV_STARTED=0
	kci_net_start "$netdev"

	kci_net_setup "$netdev"

	kci_netdev_ethtool "$netdev"

	kci_netdev_stop "$netdev"
	return 0
}

#check for needed privileges
if [ "$(id -u)" -ne 0 ];then
	echo "SKIP: Need root privileges"
	exit 0
fi

ip -Version 2>/dev/null >/dev/null
if [ $? -ne 0 ];then
	echo "SKIP: Could not run test without the ip tool"
	exit 0
fi

TMP_LIST_NETDEV="$(mktemp)"
if [ ! -e "$TMP_LIST_NETDEV" ];then
	echo "FAIL: Cannot create a tmp file"
	exit 1
fi

ls /sys/class/net/ |grep -vE '^lo|^tun' | grep -E '^eth|enp[0-9]s[0-9]' > "$TMP_LIST_NETDEV"
while read netdev
do
	kci_test_netdev "$netdev"
done < "$TMP_LIST_NETDEV"

rm "$TMP_LIST_NETDEV"
exit 0
