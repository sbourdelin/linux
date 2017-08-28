#!/bin/bash

# This example script activates an interface based on the specified
# configuration.
#
# In the interest of keeping the KVP daemon code free of distro specific
# information; the kvp daemon code invokes this external script to configure
# the interface.
#
# The only argument to this script is the configuration file that is to
# be used to configure the interface.
#
# Each Distro is expected to implement this script in a distro specific
# fashion. For instance on Distros that ship with Network Manager enabled,
# this script can be based on the Network Manager APIs for configuring the
# interface.
#
# This example script is based on a RHEL environment.
#
# Here is the format of the ip configuration file:
#
# HWADDR=macaddr
# DEVICE=interface name
# BOOTPROTO=<protocol> (where <protocol> is "dhcp" if DHCP is configured
#                       or "none" if no boot-time protocol should be used)
#
# IPADDR0=ipaddr1
# IPADDR1=ipaddr2
# IPADDRx=ipaddry (where y = x + 1)
#
# NETMASK0=netmask1
# NETMASKx=netmasky (where y = x + 1)
#
# GATEWAY=ipaddr1
# GATEWAYx=ipaddry (where y = x + 1)
#
# DNSx=ipaddrx (where first DNS address is tagged as DNS1 etc)
#
# IPV6 addresses will be tagged as IPV6ADDR, IPV6 gateway will be
# tagged as IPV6_DEFAULTGW and IPV6 NETMASK will be tagged as
# IPV6NETMASK.
#
# The host can specify multiple ipv4 and ipv6 addresses to be
# configured for the interface. Furthermore, the configuration
# needs to be persistent. A subsequent GET call on the interface
# is expected to return the configuration that is set via the SET
# call.
#
interface=$(echo $1 | awk -F - '{ print $2 }')

current_ip=$(ip addr show $interface|grep "inet ");
config_file_ip=$(grep IPADDR $1|cut -d"=" -f2);

current_ipv6=$(ip addr show $interface|grep "inet6 ");
config_file_ipv6=$(grep IPV6ADDR $1|cut -d"=" -f2);
config_file_ipv6_netmask=$(grep IPV6NETMASK $1|cut -d"=" -f2);
config_file_ipv6=${config_file_ipv6}/${config_file_ipv6_netmask};

network_service_state=$(/bin/systemctl is-active network);

while [[ ${network_service_state} == "activating" \
   || ${network_service_state} == "deactivating" ]]; do
    # Network script is still working. let's wait a bit.
    # The default timeout for systemd is 90s.
    sleep 30s;
    ((i++));
    network_service_state=$(/bin/systemctl is-active network);

    # If network service doens't come up or down in 90s we log the
    # error and give up.
    if [[ $i == 3 ]]; then
        logger "Couldn't set IP address for fail-over interface"\
            " because network daemon might be busy. Try to"\
            " if-down $interface && if-up $interface"\
            " manually later.";
        exit 1;
    fi
done

# Only set the IP if it's not configured yet.
if [[ $(test "${current_ip#*$config_file_ip}") == "$config_file_ip" \
    || $(test "${current_ipv6#*$config_file_ipv6}") == "$current_ipv6" ]]; then
    echo "IPV6INIT=yes" >> $1
    echo "NM_CONTROLLED=no" >> $1
    echo "PEERDNS=yes" >> $1
    echo "ONBOOT=yes" >> $1

    cp $1 /etc/sysconfig/network-scripts/

    /sbin/ifdown $interface 2>/dev/null
    /sbin/ifup $interface 2>/dev/null
fi
