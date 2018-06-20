:y;type filter hook prerouting priority -150

*ip;x;y

tproxy;ok
tproxy to 192.0.2.1;ok
tproxy to [2001:db8::1];fail
meta l4proto 6 tproxy to :50080;ok
meta l4proto 6 tproxy to 192.0.2.1:50080;ok
meta l4proto 6 tproxy to [2001:db8::1]:50080;fail
tproxy to :50080;fail

