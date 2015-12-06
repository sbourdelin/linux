/*
 * fireface.c - a part of driver for RMW Fireface series
 *
 * Copyright (c) 2015-2016 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include "fireface.h"

#define OUI_RME	0x000a35

MODULE_DESCRIPTION("RME Fireface series Driver");
MODULE_AUTHOR("Takashi Sakamoto <o-takashi@sakamocchi.jp>");
MODULE_LICENSE("GPL v2");

static int identify_model(struct snd_ff *ff,
			  const struct ieee1394_device_id *entry)
{
	struct fw_device *fw_dev = fw_parent_device(ff->unit);
	const char *model;

	/* TODO: how to detect all of models? */
	model = "Fireface 400";

	strcpy(ff->card->driver, "Fireface");
	strcpy(ff->card->shortname, model);
	strcpy(ff->card->mixername, model);
	snprintf(ff->card->longname, sizeof(ff->card->longname),
		 "RME %s, GUID %08x%08x at %s, S%d", model,
		 fw_dev->config_rom[3], fw_dev->config_rom[4],
		 dev_name(&ff->unit->device), 100 << fw_dev->max_speed);

	return 0;
}

static void ff_card_free(struct snd_card *card)
{
	struct snd_ff *ff = card->private_data;

	snd_ff_transaction_unregister(ff);

	fw_unit_put(ff->unit);

	mutex_destroy(&ff->mutex);
}

static int snd_ff_probe(struct fw_unit *unit,
			   const struct ieee1394_device_id *entry)
{
	struct snd_card *card;
	struct snd_ff *ff;
	int err;

	err = snd_card_new(&unit->device, -1, NULL, THIS_MODULE,
			   sizeof(struct snd_ff), &card);
	if (err < 0)
		return err;
	card->private_free = ff_card_free;

	/* initialize myself */
	ff = card->private_data;
	ff->card = card;
	ff->unit = fw_unit_get(unit);

	mutex_init(&ff->mutex);
	spin_lock_init(&ff->lock);
	dev_set_drvdata(&unit->device, ff);

	err = identify_model(ff, entry);
	if (err < 0)
		goto error;

	/*
	 * TODO: the rest of work should be done in workqueue because of some
	 * bus resets.
	 */

	err = snd_ff_transaction_register(ff);
	if (err < 0)
		goto error;

	err = snd_card_register(card);
	if (err < 0)
		goto error;

	return err;
error:
	snd_card_free(card);
	return err;
}

static void snd_ff_update(struct fw_unit *unit)
{
	struct snd_ff *ff = dev_get_drvdata(&unit->device);

	snd_ff_transaction_reregister(ff);
}

static void snd_ff_remove(struct fw_unit *unit)
{
	struct snd_ff *ff = dev_get_drvdata(&unit->device);

	/* No need to wait for releasing card object in this context. */
	snd_card_free_when_closed(ff->card);
}

static const struct ieee1394_device_id snd_ff_id_table[] = {
	/* Fireface 400 */
	{
		.match_flags	= IEEE1394_MATCH_VENDOR_ID |
				  IEEE1394_MATCH_SPECIFIER_ID |
				  IEEE1394_MATCH_VERSION |
				  IEEE1394_MATCH_MODEL_ID,
		.vendor_id	= OUI_RME,
		.specifier_id	= 0x000a35,
		.version	= 0x000002,
		.model_id	= 0x101800,
	},
	{}
};
MODULE_DEVICE_TABLE(ieee1394, snd_ff_id_table);

static struct fw_driver ff_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "snd-fireface",
		.bus	= &fw_bus_type,
	},
	.probe    = snd_ff_probe,
	.update   = snd_ff_update,
	.remove   = snd_ff_remove,
	.id_table = snd_ff_id_table,
};

static int __init snd_ff_init(void)
{
	return driver_register(&ff_driver.driver);
}

static void __exit snd_ff_exit(void)
{
	driver_unregister(&ff_driver.driver);
}

module_init(snd_ff_init);
module_exit(snd_ff_exit);
