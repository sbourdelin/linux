#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int get_pref_hint(unsigned int cpu)
{
	char fname[64];
	int val = -1;
	int fd;

	sprintf(fname, "/sys/devices/system/cpu/cpu%d/energy_policy_pref_hint", cpu);

	fd = open(fname, O_RDONLY);
	if (fd < 0)
		return -1;

	if (read(fd, &val, sizeof(val)) != sizeof(val))
		val = -1;

	close(fd);
	return val;
}

int set_pref_hint(unsigned int cpu, unsigned int val)
{
	char fname[64];
	int ret = 0, fd;

	sprintf(fname, "/sys/devices/system/cpu/cpu%d/energy_policy_pref_hint", cpu);

	fd = open(fname, O_WRONLY);
	if (fd < 0)
		return -1;

	if (write(fd, &val, sizeof(val)) != sizeof(val))
		ret = -1;

	close(fd);
	return ret;
}


