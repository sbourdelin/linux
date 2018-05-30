:sockchain;type filter hook prerouting priority -150

*ip;sockip4;sockchain
*ip6;sockip6;sockchain
*inet;sockin;sockchain

socket transparent 1;ok
