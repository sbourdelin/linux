DESTDIR ?=

x86_energy_perf_policy : x86_energy_perf_policy.c ../../utils/utils.c

clean :
	rm -f x86_energy_perf_policy ../../utils/utils.o

install :
	install x86_energy_perf_policy ${DESTDIR}/usr/bin/
	install x86_energy_perf_policy.8 ${DESTDIR}/usr/share/man/man8/
