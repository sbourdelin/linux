.. SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB

Linux* Base Driver for Mellanox Core VPI Network Connection
===========================================================

Copyright (c) 2018, Mellanox Technologies inc.

Contents
========

- Command Line Parameters

Command Line Parameters
=======================

Devlink tool
------------
The driver utilizes the devlink device configuration tool for setting driver
configuration, as well as displaying device attributes.

:Devlink supported parameters:

- DEVLINK_PARAM_GENERIC_ID_REGION_SNAPSHOT:

      - This parameter enables capturing region snapshot of the crspace during critical errors.
      - The default value of this parameter is disabled.

      - Example:
         devlink region show
            List available address regions and snapshot.

         devlink region del pci/0000:00:05.0/cr-space snapshot 1
            Delete snapshot id 1 from cr-space address region from device pci/0000:00:05.0.

         devlink region dump pci/0000:00:05.0/cr-space snapshot 1
            Dump the snapshot taken from cr-space address region with ID 1

         devlink region read pci/0000:00:05.0/cr-space snapshot 1 address 0x10 legth 16
            Read from address 0x10, 16 Bytes of snapshot ID 1 taken from cr-space address region
