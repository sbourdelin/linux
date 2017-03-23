:input;type filter hook input priority 0
:ingress;type filter hook ingress device lo priority 0

*ip;test-inet;input

# can remove ip dependency -- its redundant in ip family
ip protocol tcp tcp dport 22;ok;tcp dport 22
