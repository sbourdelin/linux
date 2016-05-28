/*
 * Copyright (C) 2016 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, as published by
 * the Free Software Foundation.
 *
 * Expose a PWM controlled by the ChromeOS EC to the host processor.
 */

#include <linux/module.h>
#include <linux/mfd/cros_ec.h>
#include <linux/mfd/cros_ec_commands.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>

/**
 * struct cros_ec_pwm_device - Driver data for EC PWM
 *
 * @dev: Device node
 * @ec: Pointer to EC device
 * @chip: PWM controller chip
 */
struct cros_ec_pwm_device {
	struct device *dev;
	struct cros_ec_device *ec;
	struct pwm_chip chip;
};

static inline struct cros_ec_pwm_device *pwm_to_cros_ec_pwm(struct pwm_chip *c)
{
	return container_of(c, struct cros_ec_pwm_device, chip);
}

static int cros_ec_pwm_set_duty(struct cros_ec_pwm_device *ec_pwm,
				   struct pwm_device *pwm,
				   uint16_t duty)
{
	struct cros_ec_device *ec = ec_pwm->ec;
	struct ec_params_pwm_set_duty *params;
	struct cros_ec_command *msg;
	int ret;

	msg = kzalloc(sizeof(*msg) + sizeof(*params), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	params = (void *)&msg->data[0];

	msg->version = 0;
	msg->command = EC_CMD_PWM_SET_DUTY;
	msg->insize = 0;
	msg->outsize = sizeof(*params);

	params->duty = duty;
	params->pwm_type = EC_PWM_TYPE_GENERIC;
	params->index = pwm->hwpwm;

	ret = cros_ec_cmd_xfer_status(ec, msg);
	kfree(msg);
	return ret;
}

static int cros_ec_pwm_get_duty(struct cros_ec_pwm_device *ec_pwm,
				struct pwm_device *pwm)
{
	struct cros_ec_device *ec = ec_pwm->ec;
	struct ec_params_pwm_get_duty *params;
	struct ec_response_pwm_get_duty *resp;
	struct cros_ec_command *msg;
	int ret;

	msg = kzalloc(sizeof(*msg) + max(sizeof(*params), sizeof(*resp)),
			GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	params = (void *)&msg->data[0];
	resp = (void *)&msg->data[0];

	msg->version = 0;
	msg->command = EC_CMD_PWM_GET_DUTY;
	msg->insize = sizeof(*params);
	msg->outsize = sizeof(*resp);

	params->pwm_type = EC_PWM_TYPE_GENERIC;
	params->index = pwm->hwpwm;

	ret = cros_ec_cmd_xfer_status(ec, msg);
	if (ret < 0)
		goto out;

	ret = resp->duty;

out:
	kfree(msg);
	return ret;
}

static int cros_ec_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			     struct pwm_state *state)
{
	struct cros_ec_pwm_device *ec_pwm = pwm_to_cros_ec_pwm(chip);

	/* The EC won't let us change the period */
	if (state->period != EC_PWM_MAX_DUTY)
		return -EINVAL;

	return cros_ec_pwm_set_duty(ec_pwm, pwm, state->duty_cycle);
}

static void cros_ec_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				  struct pwm_state *state)
{
	struct cros_ec_pwm_device *ec_pwm = pwm_to_cros_ec_pwm(chip);
	int ret;

	ret = cros_ec_pwm_get_duty(ec_pwm, pwm);
	if (ret < 0) {
		dev_err(chip->dev, "error getting initial duty: %d\n", ret);
		return;
	}

	state->enabled = (ret > 0);
	state->period = EC_PWM_MAX_DUTY;
	state->duty_cycle = ret;
}

static struct pwm_device *
cros_ec_pwm_xlate(struct pwm_chip *pc, const struct of_phandle_args *args)
{
	struct pwm_device *pwm;

	if (args->args[0] >= pc->npwm)
		return ERR_PTR(-EINVAL);

	pwm = pwm_request_from_chip(pc, args->args[0], NULL);
	if (IS_ERR(pwm))
		return pwm;

	/* The EC won't let us change the period */
	pwm->args.period = EC_PWM_MAX_DUTY;

	return pwm;
}

static const struct pwm_ops cros_ec_pwm_ops = {
	.get_state	= cros_ec_pwm_get_state,
	.apply		= cros_ec_pwm_apply,
	.owner		= THIS_MODULE,
};

static int cros_ec_pwm_probe(struct platform_device *pdev)
{
	struct cros_ec_device *ec = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct cros_ec_pwm_device *ec_pwm;
	struct pwm_chip *chip;
	u32 val;
	int ret;

	if (!ec) {
		dev_err(dev, "no parent EC device\n");
		return -EINVAL;
	}

	ec_pwm = devm_kzalloc(dev, sizeof(*ec_pwm), GFP_KERNEL);
	if (!ec_pwm)
		return -ENOMEM;
	chip = &ec_pwm->chip;
	ec_pwm->ec = ec;

	/* PWM chip */
	chip->dev = dev;
	chip->ops = &cros_ec_pwm_ops;
	chip->of_xlate = cros_ec_pwm_xlate;
	chip->of_pwm_n_cells = 1;
	chip->base = -1;
	ret = of_property_read_u32(np, "google,max-pwms", &val);
	if (ret) {
		dev_err(dev, "Couldn't read max-pwms property: %d\n", ret);
		return ret;
	}
	/* The index field is only 8 bits */
	if (val > U8_MAX) {
		dev_err(dev, "Can't support %u PWMs\n", val);
		return -EINVAL;
	}
	chip->npwm = val;

	ret = pwmchip_add(chip);
	if (ret < 0) {
		dev_err(dev, "cannot register PWM: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, ec_pwm);

	return ret;
}

static int cros_ec_pwm_remove(struct platform_device *dev)
{
	struct cros_ec_pwm_device *ec_pwm = platform_get_drvdata(dev);
	struct pwm_chip *chip = &ec_pwm->chip;

	return pwmchip_remove(chip);
}

#ifdef CONFIG_OF
static const struct of_device_id cros_ec_pwm_of_match[] = {
	{ .compatible = "google,cros-ec-pwm" },
	{},
};
MODULE_DEVICE_TABLE(of, cros_ec_pwm_of_match);
#endif

static struct platform_driver cros_ec_pwm_driver = {
	.probe = cros_ec_pwm_probe,
	.remove = cros_ec_pwm_remove,
	.driver = {
		.name = "cros-ec-pwm",
		.of_match_table = of_match_ptr(cros_ec_pwm_of_match),
	},
};
module_platform_driver(cros_ec_pwm_driver);

MODULE_ALIAS("platform:cros-ec-pwm");
MODULE_DESCRIPTION("ChromeOS EC PWM driver");
MODULE_LICENSE("GPL v2");
