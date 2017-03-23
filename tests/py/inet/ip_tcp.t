:input;type filter hook input priority 0
:ingress;type filter hook ingress device lo priority 0

*inet;test-inet;input
*bridge;test-bridge;input
*netdev;test-netdev;ingress

# must not remove ip dependency -- ONLY ipv4 packets should be matched
ip protocol tcp tcp dport 22;ok;ip protocol 6 tcp dport 22
