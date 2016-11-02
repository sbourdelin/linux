# Makefile for LEDs tools

CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -g -I../../include/uapi

all: uledmon led_notify_mon
%: %.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) uledmon led_notify_mon

.PHONY: all clean
