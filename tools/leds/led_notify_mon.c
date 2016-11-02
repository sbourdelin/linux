/*
 * led_notify_mon.c
 *
 * This program monitors LED brightness change notifications,
 * either having its origin in the hardware or in the software.
 * A timestamp and brightness value is printed each time the brightness changes.
 *
 * Usage: led_notify_mon <device-name>
 *
 * <device-name> is the name of the LED class device to be created. Pressing
 * CTRL+C will exit.
 */

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/uleds.h>

int main(int argc, char const *argv[])
{
	int fd, ret;
	char brightness_file_path[LEDS_MAX_NAME_SIZE + 11];
	struct pollfd pollfd;
	struct timespec ts;
	char buf[11];

	if (argc != 2) {
		fprintf(stderr, "Requires <device-name> argument\n");
		return 1;
	}

	snprintf(brightness_file_path, LEDS_MAX_NAME_SIZE,
		 "/sys/class/leds/%s/brightness", argv[1]);

	fd = open(brightness_file_path, O_RDONLY);
	if (fd == -1) {
		printf("Failed to open %s file\n", brightness_file_path);
		return 1;
	}

	ret = read(fd, buf, sizeof(buf));
	if (ret < 0) {
		printf("Failed to read %s file\n", brightness_file_path);
		goto err_read;
	}

	pollfd.fd = fd;
	pollfd.events = POLLPRI;

	while (1) {
		ret = poll(&pollfd, 1, -1);
		if (ret == -1) {
			printf("Failed to poll %s file (%d)\n",
				brightness_file_path, ret);
			ret = 1;
			break;
		}

		clock_gettime(CLOCK_MONOTONIC, &ts);

		ret = read(fd, buf, sizeof(buf));
		if (ret < 0)
			break;

		printf("[%ld.%09ld] %u\n", ts.tv_sec, ts.tv_nsec, atoi(buf));
	}

err_read:
	close(fd);

	return ret;
}
