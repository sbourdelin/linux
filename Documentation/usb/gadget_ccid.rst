.. SPDX-License-Identifier: GPL-2.0

============
CCID Gadget
============

:Author: Marcus Folkesson <marcus.folkesson@gmail.com>

Introduction
============

The CCID Gadget will present itself as a CCID device to the host system.
The device supports two endpoints for now; BULK IN and BULK OUT.
These endpoints are exposed to userspace via /dev/ccidg*.

All CCID commands are sent on the BULK-OUT endpoint. Each command sent to the CCID
has an associated ending response. Some commands can also have intermediate
responses. The response is sent on the BULK-IN endpoint.
See Figure 3-3 in the CCID Specification [1]_ for more details.

The CCID commands must be handled in userspace since the driver is only working
as a transport layer for the TPDUs.


CCID Commands
--------------

All CCID commands begins with a 10-byte header followed by an optional
data field depending on message type.

+--------+--------------+-------+----------------------------------+
| Offset | Field        | Size  | Description                      |
+========+==============+=======+==================================+
| 0      | bMessageType | 1     | Type of message                  |
+--------+--------------+-------+----------------------------------+
| 1      | dwLength     | 4     | Message specific data length     |
|        |              |       |                                  |
+--------+--------------+-------+----------------------------------+
| 5      | bSlot        | 1     | Identifies the slot number       |
|        |              |       | for this command                 |
+--------+--------------+-------+----------------------------------+
| 6      | bSeq         | 1     | Sequence number for command      |
+--------+--------------+-------+----------------------------------+
| 7      | ...          | 3     | Fields depends on message type   |
+--------+--------------+-------+----------------------------------+
| 10     | abData       | array | Message specific data (OPTIONAL) |
+--------+--------------+-------+----------------------------------+


Multiple CCID gadgets
----------------------

It is possible to create multiple instances of the CCID gadget, however,
a much more flexible way is to create one gadget and set the `nslots` attribute
to the number of desired CCID devices.

All CCID commands specify which slot is the receiver in the `bSlot` field
of the CCID header.

Usage
=====

Access from userspace
----------------------
All communication is by read(2) and write(2) to the corresponding /dev/ccidg* device.
Only one file descriptor is allowed to be open to the device at a time.

The buffer size provided to read(2) **must be at least** 522 (10 bytes header + 512 bytes payload)
bytes as we are working with whole commands.

The buffer size provided to write(2) **may not exceed** 522 (10 bytes header + 512 bytes payload)
bytes as we are working with whole commands.


Configuration with configfs
----------------------------

ConfigFS is used to create and configure the CCID gadget.
In order to get a device to work as intended, a few attributes must
be considered.

The attributes are described below followed by an example.

features
~~~~~~~~~

The `feature` attribute writes to the dwFeatures field in the class descriptor.
See Table 5.1-1 Smart Card Device Descriptors in the CCID Specification [1]_.

The value indicates what intelligent features the CCID has.
These values are available to user application as defined in ccid.h [2]_.
The default value is 0x00000000.

The value is a bitwise OR operation performed on the following values:

+------------+----------------------------------------------------------------+
| Value      | Description                                                    |
+============+================================================================+
| 0x00000000 | No special characteristics                                     |
+------------+----------------------------------------------------------------+
| 0x00000002 | Automatic parameter configuration based on ATR data            |
+------------+----------------------------------------------------------------+
| 0x00000004 | Automatic activation of ICC on inserting                       |
+------------+----------------------------------------------------------------+
| 0x00000008 | Automatic ICC voltage selection                                |
+------------+----------------------------------------------------------------+
| 0x00000010 | Automatic ICC clock frequency change according to active       |
|            | parameters provided by the Host or self determined             |
+------------+----------------------------------------------------------------+
| 0x00000020 | Automatic baud rate change according to active                 |
|            | parameters provided by the Host or self determined             |
+------------+----------------------------------------------------------------+
| 0x00000040 | Automatic parameters negotiation made by the CCID              |
+------------+----------------------------------------------------------------+
| 0x00000080 | Automatic PPS made by the CCID according to the                |
|            | active parameters                                              |
+------------+----------------------------------------------------------------+
| 0x00000100 | CCID can set ICC in clock stop mode                            |
+------------+----------------------------------------------------------------+
| 0x00000200 | NAD value other than 00 accepted (T=1 protocol in use)         |
+------------+----------------------------------------------------------------+
| 0x00000400 | Automatic IFSD exchange as first exchange                      |
+------------+----------------------------------------------------------------+


Only one of the following values may be present to select a level of exchange:

+------------+--------------------------------------------------+
| Value      | Description                                      |
+============+==================================================+
| 0x00010000 | TPDU level exchanges with CCID                   |
+------------+--------------------------------------------------+
| 0x00020000 | Short APDU level exchange with CCID              |
+------------+--------------------------------------------------+
| 0x00040000 | Short and Extended APDU level exchange with CCID |
+------------+--------------------------------------------------+

If none of those values is indicated the level of exchange is
character.


protocols
~~~~~~~~~~
The `protocols` attribute writes to the dwProtocols field in the class descriptor.
See Table 5.1-1 Smart Card Device Descriptors in the CCID Specification [1]_.

The value is a bitwise OR operation performed on the following values:

+--------+--------------+
| Value  | Description  |
+========+==============+
| 0x0001 | Protocol T=0 |
+--------+--------------+
| 0x0002 | Protocol T=1 |
+--------+--------------+

If no protocol is selected both T=0 and T=1 will be supported (`protocols` = 0x0003).

nslots
~~~~~~

The `nslots` attribute writes to the bMaxSlotIndex field in the class descriptor.
See Table 5.1-1 Smart Card Device Descriptors in the CCID Specification [1]_.

This is the index of the highest available slot on this device. All slots are consecutive starting at 00h.
i.e. 0Fh = 16 slots on this device numbered 00h to 0Fh.

The default value is 0, which means one slot.


pinsupport
~~~~~~~~~~~~

This value indicates what PIN support features the CCID has.

The `pinsupport` attribute writes to the dwPINSupport field in the class descriptor.
See Table 5.1-1 Smart Card Device Descriptors in the CCID Specification [1]_.


The value is a bitwise OR operation performed on the following values:

+--------+----------------------------+
| Value  | Description                |
+========+============================+
| 0x00   | No PIN support             |
+--------+----------------------------+
| 0x01   | PIN Verification supported |
+--------+----------------------------+
| 0x02   | PIN Modification supported |
+--------+----------------------------+

The default value is set to 0x00.


lcdlayout
~~~~~~~~~~

Number of lines and characters for the LCD display used to send messages for PIN entry.

The `lcdLayout` attribute writes to the wLcdLayout field in the class descriptor.
See Table 5.1-1 Smart Card Device Descriptors in the CCID Specification [1]_.


The value is set as follows:

+--------+------------------------------------+
| Value  | Description                        |
+========+====================================+
| 0x0000 | No LCD                             |
+--------+------------------------------------+
| 0xXXYY | XX: number of lines                |
|        | YY: number of characters per line. |
+--------+------------------------------------+

The default value is set to 0x0000.


Example
-------

Here is an example on how to setup a CCID gadget with configfs ::

    #!/bin/sh

    CONFIGDIR=/sys/kernel/config
    GADGET=$CONFIGDIR/usb_gadget/g0
    FUNCTION=$GADGET/functions/ccid.sc0

    VID=YOUR_VENDOR_ID_HERE
    PID=YOUR_PRODUCT_ID_HERE
    UDC=YOUR_UDC_HERE

    #Mount filesystem
    mount none -t configfs $CONFIGDIR

    #Populate ID:s
    echo $VID > $GADGET/idVendor
    echo $PID > $GADGET/idProduct

    #Create and configure the gadget
    mkdir $FUNCTION
    echo 0x000407B8 > $FUNCTION/features
    echo 0x02 > $FUNCTION/protocols

    #Create our english strings
    mkdir  $GADGET/strings/0x409
    echo 556677 > $GADGET/strings/0x409/serialnumber
    echo "Hungry Penguins" > $GADGET/strings/0x409/manufacturer
    echo "Harpoon With SmartCard"  > $GADGET/strings/0x409/product

    #Create configuration
    mkdir  $GADGET/configs/c.1
    mkdir  $GADGET/configs/c.1/strings/0x409
    echo Config1 > $GADGET/configs/c.1/strings/0x409/configuration

    #Use `Config1` for our CCID gadget
    ln -s $FUNCTION $GADGET/configs/c.1

    #Execute
    echo $UDC > $GADGET/UDC


References
==========

.. [1] http://www.usb.org/developers/docs/devclass_docs/DWG_Smart-Card_CCID_Rev110.pdf
.. [2] include/uapi/linux/usb/ccid.h
