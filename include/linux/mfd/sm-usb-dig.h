/*
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether expressed or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.
 */

#ifndef __LINUX_MFD_SM_USB_DIG_H
#define __LINUX_MFD_SM_USB_DIG_H

#define SMUSBDIG_PACKET_SIZE    32
/* (packet size - packet header */
#define SMUSBDIG_DATA_SIZE (SMUSBDIG_PACKET_SIZE - 7)

enum smusbdig_function {
	SMUSBDIG_SPI            = 0x01,
	SMUSBDIG_I2C            = 0x02,
	SMUSBDIG_1W             = 0x03,
	SMUSBDIG_COMMAND        = 0x04,
	SMUSBDIG_VERSION        = 0x07,
};

enum smusbdig_sub_command {
	SMUSBDIG_COMMAND_DUTPOWERON	= 0x01,
	SMUSBDIG_COMMAND_DUTPOWEROFF	= 0x02,
};

struct smusbdig_packet {
	u8 function;
	u8 channel;
	u8 edge_polarity;
	u8 num_commands;
	u8 is_command_mask[3];
	u8 data[SMUSBDIG_DATA_SIZE];
} __packed;

static void smusbdig_packet_add_command(struct smusbdig_packet *packet,
					int command)
{
	int num_commands = packet->num_commands;
	int mask_number = num_commands / 8;
	int mask_bit = num_commands % 8;

	if (num_commands >= SMUSBDIG_DATA_SIZE)
		return;

	packet->is_command_mask[mask_number] |= BIT(7 - mask_bit);
	packet->data[num_commands] = command;
	packet->num_commands++;
}

static void smusbdig_packet_add_data(struct smusbdig_packet *packet, u8 data)
{
	int num_commands = packet->num_commands;

	if (num_commands >= SMUSBDIG_DATA_SIZE)
		return;

	packet->data[num_commands] = data;
	packet->num_commands++;
}

struct smusbdig_device;
int smusbdig_xfer(struct smusbdig_device *smusbdig, u8 *buffer, int size);

#endif /* __LINUX_MFD_SM_USB_DIG_H */
