/*
 * Intel CHT Whiskey Cove Fuel Gauge driver
 * Copyright (C) 2017 Hans de Goede <hdegoede@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __CHT_WC_FUEL_GAUGE_H
#define __CHT_WC_FUEL_GAUGE_H

int cht_wc_fg_get_property(enum power_supply_property prop,
			   union power_supply_propval *val);

#endif
