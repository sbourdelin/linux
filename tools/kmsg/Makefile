# SPDX-License-Identifier: GPL-2.0
CC := $(CROSS_COMPILE)gcc

CFLAGS := -O2 -Wall $$(pkg-config --libs openssl)

PROGS := dmesg-decipher

%: %.c
	$(CC) $(CFLAGS) -o $@ $^

all: $(PROGS)

clean:
	rm -fr $(PROGS)
