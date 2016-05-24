CC = $(CROSS_COMPILE)gcc
CFLAGS += -Wall -g -D_GNU_SOURCE

all: iio_event_monitor lsiio iio_generic_buffer

iio_event_monitor: iio_event_monitor.o iio_utils.o

lsiio: lsiio.o iio_utils.o

iio_generic_buffer: iio_generic_buffer.o iio_utils.o

%.o: %.c iio_utils.h

.PHONY: clean
clean:
	rm -f *.o iio_event_monitor lsiio iio_generic_buffer
