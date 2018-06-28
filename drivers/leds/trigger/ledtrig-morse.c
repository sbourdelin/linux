// SPDX-License-Identifier: GPL-2.0
/*
 * ledtrig-morse: LED Morse Trigger
 *
 * send a string as morse code out through LEDs
 *
 * can be used to send error codes or messages
 *
 * string to be send is written into morse_string
 * supported are letters and digits
 *
 * Author: Andreas Klinger <ak@it-klinger.de>
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/leds.h>


#define MORSE_DOT_UNIT_DEFAULT	500
#define MORSE_TELEGRAM_SIZE	100

struct morse_data {
	unsigned int		dot_unit;
	struct led_classdev	*led_cdev;
	struct work_struct	work;
	char			telegram[MORSE_TELEGRAM_SIZE];
	unsigned int		telegram_size;
	struct mutex		lock;
};

struct morse_char {
	char	c;
	char	*z;
};

static struct morse_char morse_table[] = {
	{'a', ".-S"},
	{'b', "-...S"},
	{'c', "-.-.S"},
	{'d', "-..S"},
	{'e', ".S"},
	{'f', "..-.S"},
	{'g', "--.S"},
	{'h', "....S"},
	{'i', "..S"},
	{'j', ".---S"},
	{'k', "-.-S"},
	{'l', ".-..S"},
	{'m', "--S"},
	{'n', "-.S"},
	{'o', "---S"},
	{'p', ".--.S"},
	{'q', "--.-S"},
	{'r', ".-.S"},
	{'s', "...S"},
	{'t', "-S"},
	{'u', "..-S"},
	{'v', "...-S"},
	{'w', ".--S"},
	{'x', "-..-S"},
	{'y', "-.--S"},
	{'z', "--..S"},
	{'1', ".----S"},
	{'2', "..---S"},
	{'3', "...--S"},
	{'4', "....-S"},
	{'5', ".....S"},
	{'6', "-....S"},
	{'7', "--...S"},
	{'8', "---..S"},
	{'9', "----.S"},
	{'0', "-----S"},
	{0, NULL},
};

static void morse_long(struct led_classdev *led_cdev)
{
	struct morse_data *data = led_cdev->trigger_data;

	led_set_brightness(led_cdev, LED_ON);
	msleep(3 * data->dot_unit);
	led_set_brightness(led_cdev, LED_OFF);
	msleep(data->dot_unit);
}

static void morse_short(struct led_classdev *led_cdev)
{
	struct morse_data *data = led_cdev->trigger_data;

	led_set_brightness(led_cdev, LED_ON);
	msleep(data->dot_unit);
	led_set_brightness(led_cdev, LED_OFF);
	msleep(data->dot_unit);
}

static void morse_letter_space(struct led_classdev *led_cdev)
{
	struct morse_data *data = led_cdev->trigger_data;
	/*
	 * Pause: 3 dot spaces
	 * 1 dot space already there from morse character
	 */
	msleep(2 * data->dot_unit);
}

static void morse_word_space(struct led_classdev *led_cdev)
{
	struct morse_data *data = led_cdev->trigger_data;
	/*
	 * Pause: 7 dot spaces
	 * 1 dot space already there from morse character
	 * 2 dot spaces already there from letter space
	 */
	msleep(4 * data->dot_unit);
}

static void morse_send_char(struct led_classdev *led_cdev, char ch)
{
	int i = 0;

	while ((morse_table[i].c) && (morse_table[i].c != tolower(ch)))
		i++;

	if (morse_table[i].c) {
		int j = 0;

		while (morse_table[i].z[j] != 'S') {
			switch (morse_table[i].z[j]) {
			case '.':
				morse_short(led_cdev);
				break;
			case '-':
				morse_long(led_cdev);
				break;
			}
			j++;
		}
		morse_letter_space(led_cdev);
	} else {
		/*
		 * keep it simple:
		 * whenever there is an unrecognized character make a word
		 * space
		 */
		morse_word_space(led_cdev);
	}
}

static void morse_work(struct work_struct *work)
{
	struct morse_data *data = container_of(work, struct morse_data, work);
	int i;

	mutex_lock(&data->lock);

	for (i = 0; i < data->telegram_size; i++)
		morse_send_char(data->led_cdev, data->telegram[i]);

	mutex_unlock(&data->lock);
}

static ssize_t morse_string_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct morse_data *data = led_cdev->trigger_data;

	if (size >= sizeof(data->telegram))
		return -E2BIG;

	mutex_lock(&data->lock);

	memcpy(data->telegram, buf, size);
	data->telegram_size = size;

	mutex_unlock(&data->lock);

	schedule_work(&data->work);

	return size;
}

static DEVICE_ATTR_WO(morse_string);

static ssize_t dot_unit_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct morse_data *data = led_cdev->trigger_data;

	return sprintf(buf, "%u\n", data->dot_unit);
}

static ssize_t dot_unit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct morse_data *data = led_cdev->trigger_data;
	unsigned long dot_unit;
	ssize_t ret = -EINVAL;

	ret = kstrtoul(buf, 10, &dot_unit);
	if (ret)
		return ret;

	data->dot_unit = dot_unit;

	return size;
}

static DEVICE_ATTR_RW(dot_unit);

static void morse_trig_activate(struct led_classdev *led_cdev)
{
	int rc;
	struct morse_data *data;

	data = kzalloc(sizeof(struct morse_data), GFP_KERNEL);
	if (!data) {
		dev_err(led_cdev->dev, "unable to allocate morse trigger\n");
		return;
	}

	led_cdev->trigger_data = data;
	data->led_cdev = led_cdev;
	data->dot_unit = MORSE_DOT_UNIT_DEFAULT;

	rc = device_create_file(led_cdev->dev, &dev_attr_morse_string);
	if (rc)
		goto err_out_data;

	rc = device_create_file(led_cdev->dev, &dev_attr_dot_unit);
	if (rc)
		goto err_out_morse_string;

	INIT_WORK(&data->work, morse_work);

	mutex_init(&data->lock);

	led_set_brightness(led_cdev, LED_OFF);
	led_cdev->activated = true;

	return;

err_out_data:
	kfree(data);
err_out_morse_string:
	device_remove_file(led_cdev->dev, &dev_attr_morse_string);
}

static void morse_trig_deactivate(struct led_classdev *led_cdev)
{
	struct morse_data *data = led_cdev->trigger_data;

	if (led_cdev->activated) {

		cancel_work_sync(&data->work);

		device_remove_file(led_cdev->dev, &dev_attr_morse_string);
		device_remove_file(led_cdev->dev, &dev_attr_dot_unit);

		kfree(data);

		led_cdev->trigger_data = NULL;
		led_cdev->activated = false;
	}
}

static struct led_trigger morse_led_trigger = {
	.name     = "morse",
	.activate = morse_trig_activate,
	.deactivate = morse_trig_deactivate,
};

static int __init morse_trig_init(void)
{
	return led_trigger_register(&morse_led_trigger);
}

static void __exit morse_trig_exit(void)
{
	led_trigger_unregister(&morse_led_trigger);
}

module_init(morse_trig_init);
module_exit(morse_trig_exit);

MODULE_AUTHOR("Andreas Klinger <ak@it-klinger.de>");
MODULE_DESCRIPTION("Morse code LED trigger");
MODULE_LICENSE("GPL");
