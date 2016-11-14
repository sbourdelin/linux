#!/bin/sh
# Runs API tests for reservation_object using test-reservation kernel module

if /sbin/modprobe -q test-reservation; then
       /sbin/modprobe -q -r test-reservation
       echo "dma-buf/reservation: ok"
else
       echo "dma-buf/reservation: [FAIL]"
       exit 1
fi

