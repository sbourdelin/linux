:y;type filter hook prerouting priority -150

*ip6;x;y

tproxy;ok
tproxy to 192.0.2.1;fail
tproxy to [2001:db8::1];ok
meta l4proto 6 tproxy to :50080;ok
meta l4proto 6 tproxy to 192.0.2.1:50080;fail
meta l4proto 6 tproxy to [2001:db8::1]:50080;ok
tproxy to :50080;fail


