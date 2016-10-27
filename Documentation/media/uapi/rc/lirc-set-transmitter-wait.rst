.. -*- coding: utf-8; mode: rst -*-

.. _lirc_set_transmitter_mask:

*******************************
ioctl LIRC_SET_TRANSMITTER_WAIT
*******************************

Name
====

LIRC_SET_TRANSMITTER_WAIT - Wait for IR to transmit

Synopsis
========

.. c:function:: int ioctl( int fd, LIRC_SET_TRANSMITTER_WAIT, __u32 *enable )
    :name: LIRC_SET_TRANSMITTER_WAIT

Arguments
=========

``fd``
    File descriptor returned by open().

``enable``
    enable = 1 means wait for IR to transmit before write() returns,
    enable = 0 means return as soon as the driver has sent the commmand
    to the hardware.


Description
===========

Early lirc drivers would only return from write() when the IR had been
transmitted and the lirc daemon relies on this for calculating when to
send the next IR signal. Some drivers (e.g. usb drivers) can return
earlier than that.


Return Value
============

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.
