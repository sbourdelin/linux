/* Helper functions for Thinkpad LED control;
 * to be included from codec driver
 */

#if IS_ENABLED(CONFIG_THINKPAD_ACPI) || IS_ENABLED(CONFIG_HID_LENOVO)
#include <linux/acpi.h>
#include <linux/hid-lenovo.h>
#include <linux/thinkpad_acpi.h>

static int (*led_set_func_tpacpi)(int, bool);
static int (*led_set_func_hid_lenovo)(int, bool);
static void (*old_vmaster_hook)(void *, int);

static bool is_thinkpad(struct hda_codec *codec)
{
	return (codec->core.subsystem_id >> 16 == 0x17aa);
}

static bool is_thinkpad_acpi(struct hda_codec *codec)
{
	return (codec->core.subsystem_id >> 16 == 0x17aa) &&
	       (acpi_dev_found("LEN0068") || acpi_dev_found("IBM0068"));
}

static void update_thinkpad_mute_led(void *private_data, int enabled)
{
	if (old_vmaster_hook)
		old_vmaster_hook(private_data, enabled);

	if (led_set_func_tpacpi)
		led_set_func_tpacpi(TPACPI_LED_MUTE, !enabled);

	if (led_set_func_hid_lenovo)
		led_set_func_hid_lenovo(HID_LENOVO_LED_MUTE, !enabled);
}



static void update_thinkpad_micmute_led(struct hda_codec *codec,
				      struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	if (!ucontrol)
		return;
	if (strcmp("Capture Switch", ucontrol->id.name) == 0 && ucontrol->id.index == 0) {
		/* TODO: How do I verify if it's a mono or stereo here? */
		bool val = ucontrol->value.integer.value[0] || ucontrol->value.integer.value[1];
		if (led_set_func_tpacpi)
			led_set_func_tpacpi(TPACPI_LED_MICMUTE, !val);
		if (led_set_func_hid_lenovo)
			led_set_func_hid_lenovo(HID_LENOVO_LED_MICMUTE, !val);
	}
}

static int hda_fixup_thinkpad_acpi(struct hda_codec *codec)
{
	struct hda_gen_spec *spec = codec->spec;
	int ret = -ENXIO;

	if (!is_thinkpad(codec))
		return -ENODEV;
	if (!is_thinkpad_acpi(codec))
		return -ENODEV;
	if (!led_set_func_tpacpi)
		led_set_func_tpacpi = symbol_request(tpacpi_led_set);
	if (!led_set_func_tpacpi) {
		codec_warn(codec,
			   "Failed to find thinkpad-acpi symbol tpacpi_led_set\n");
		return -ENOENT;
	}

	if (led_set_func_tpacpi(TPACPI_LED_MUTE, false) >= 0) {
		old_vmaster_hook = spec->vmaster_mute.hook;
		spec->vmaster_mute.hook = update_thinkpad_mute_led;
		ret = 0;
	}

	if (led_set_func_tpacpi(TPACPI_LED_MICMUTE, false) >= 0) {
		if (spec->num_adc_nids > 1)
			codec_dbg(codec,
				  "Skipping micmute LED control due to several ADCs");
		else {
			spec->cap_sync_hook = update_thinkpad_micmute_led;
			ret = 0;
		}
	}

	return ret;
}

static int hda_fixup_thinkpad_hid(struct hda_codec *codec)
{
	struct hda_gen_spec *spec = codec->spec;
	int ret = 0;

	if (!is_thinkpad(codec))
		return -ENODEV;
	if (!led_set_func_hid_lenovo)
		led_set_func_hid_lenovo = symbol_request(hid_lenovo_led_set);
	if (!led_set_func_hid_lenovo) {
		codec_warn(codec,
			   "Failed to find hid-lenovo symbol hid_lenovo_led_set\n");
		return -ENOENT;
	}

	if (update_thinkpad_mute_led != spec->vmaster_mute.hook)
		old_vmaster_hook = spec->vmaster_mute.hook;

	// do not remove hook if setting delay does not work currently because
	// it is a usb hid devices which is not connected right now
	// maybe is will be connected later
	led_set_func_hid_lenovo(HID_LENOVO_LED_MUTE, false);
	spec->vmaster_mute.hook = update_thinkpad_mute_led;

	led_set_func_hid_lenovo(HID_LENOVO_LED_MICMUTE, false);
	spec->cap_sync_hook = update_thinkpad_micmute_led;

	return ret;
}

static void hda_fixup_thinkpad(struct hda_codec *codec,
				    const struct hda_fixup *fix, int action)
{
	int ret_fixup_acpi = 0;
	int ret_fixup_hid = 0;
	bool remove = 0;

	if (action == HDA_FIXUP_ACT_PROBE) {
		ret_fixup_acpi = hda_fixup_thinkpad_acpi(codec);
		ret_fixup_hid = hda_fixup_thinkpad_hid(codec);
	}

	if (led_set_func_tpacpi &&
		(action == HDA_FIXUP_ACT_FREE || ret_fixup_acpi)) {

		symbol_put(tpacpi_led_set);
		remove = true;
	}

	if (led_set_func_hid_lenovo &&
		(action == HDA_FIXUP_ACT_FREE || ret_fixup_hid)) {

		symbol_put(hid_lenovo_led_set);
		remove = true;
	}


	if (remove) {
		led_set_func_tpacpi = NULL;
		led_set_func_hid_lenovo = NULL;
		old_vmaster_hook = NULL;
	}
}

#else /* CONFIG_THINKPAD_ACPI */

static void hda_fixup_thinkpad(struct hda_codec *codec,
				    const struct hda_fixup *fix, int action)
{
}

#endif /* CONFIG_THINKPAD_ACPI */
