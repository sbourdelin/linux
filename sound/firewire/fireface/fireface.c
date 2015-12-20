/*
 * fireface.c - a part of driver for RME Fireface series
 *
 * Copyright (c) 2015-2016 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include "fireface.h"

#define OUI_RME	0x000a35

#define PROBE_DELAY_MS		(1 * MSEC_PER_SEC)

MODULE_DESCRIPTION("RME Fireface series Driver");
MODULE_AUTHOR("Takashi Sakamoto <o-takashi@sakamocchi.jp>");
MODULE_LICENSE("GPL v2");

struct snd_ff_spec spec_ff400 = {
	.name = "Fireface400",
};

static void name_card(struct snd_ff *ff)
{
	struct fw_device *fw_dev = fw_parent_device(ff->unit);

	strcpy(ff->card->driver, "Fireface");
	strcpy(ff->card->shortname, ff->spec->name);
	strcpy(ff->card->mixername, ff->spec->name);
	snprintf(ff->card->longname, sizeof(ff->card->longname),
		 "RME %s, GUID %08x%08x at %s, S%d", ff->spec->name,
		 fw_dev->config_rom[3], fw_dev->config_rom[4],
		 dev_name(&ff->unit->device), 100 << fw_dev->max_speed);
}

static void ff_card_free(struct snd_card *card)
{
	struct snd_ff *ff = card->private_data;

	/* The workqueue for registration uses the memory block. */
	cancel_work_sync(&ff->dwork.work);

	fw_unit_put(ff->unit);

	mutex_destroy(&ff->mutex);
}

static void do_probe(struct work_struct *work)
{
	struct snd_ff *ff = container_of(work, struct snd_ff, dwork.work);
	int err;

	mutex_lock(&ff->mutex);

	if (ff->card->shutdown || ff->probed)
		goto end;

	err = snd_card_register(ff->card);
	if (err < 0)
		goto end;

	ff->probed = true;
end:
	mutex_unlock(&ff->mutex);

	/*
	 * It's a difficult work to manage a race condition between workqueue,
	 * unit event handlers and processes. The memory block for this card
	 * is released as the same way that usual sound cards are going to be
	 * released.
	 */
}

static int snd_ff_probe(struct fw_unit *unit,
			   const struct ieee1394_device_id *entry)
{
	struct fw_card *fw_card = fw_parent_device(unit)->card;
	struct snd_card *card;
	struct snd_ff *ff;
	unsigned long delay;
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
	dev_set_drvdata(&unit->device, ff);

	ff->spec = (const struct snd_ff_spec *)entry->driver_data;
	name_card(ff);

	/* Register this sound card later. */
	INIT_DEFERRABLE_WORK(&ff->dwork, do_probe);
	delay = msecs_to_jiffies(PROBE_DELAY_MS) +
				fw_card->reset_jiffies - get_jiffies_64();
	schedule_delayed_work(&ff->dwork, delay);

	return 0;
}

static void snd_ff_update(struct fw_unit *unit)
{
	struct snd_ff *ff = dev_get_drvdata(&unit->device);
	struct fw_card *fw_card = fw_parent_device(unit)->card;
	unsigned long delay;

	/* Postpone a workqueue for deferred registration. */
	if (!ff->probed) {
		delay = msecs_to_jiffies(PROBE_DELAY_MS) -
				(get_jiffies_64() - fw_card->reset_jiffies);
		mod_delayed_work(ff->dwork.wq, &ff->dwork, delay);
	}
}

static void snd_ff_remove(struct fw_unit *unit)
{
	struct snd_ff *ff = dev_get_drvdata(&unit->device);

	/* For a race condition of struct snd_card.shutdown. */
	mutex_lock(&ff->mutex);

	/* No need to wait for releasing card object in this context. */
	snd_card_free_when_closed(ff->card);

	mutex_unlock(&ff->mutex);
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
		.driver_data	= (kernel_ulong_t)&spec_ff400,
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
