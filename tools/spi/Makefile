CC = $(CROSS_COMPILE)gcc

all: spidev_test spidev_fdx

%: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
clean:
	$(RM) spidev_test spidev_fdx
