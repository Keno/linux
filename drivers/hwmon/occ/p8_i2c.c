/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/unaligned.h>
#include "common.h"
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>

struct p8_i2c_occ {
	struct occ occ;
	struct i2c_client *client;
};

#define to_p8_i2c_occ(x)	container_of((x), struct p8_i2c_occ, occ)

static int p8_i2c_occ_getscom(struct i2c_client *client, u32 address, u8 *data)
{
	ssize_t rc;
	__be64 buf_be;
	u64 buf;
	struct i2c_msg msgs[2];

	address <<= 1;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags & I2C_M_TEN;
	msgs[0].len = sizeof(u32);
	msgs[0].buf = (char *)&address;


	msgs[1].addr = client->addr;
	msgs[1].flags = (client->flags & I2C_M_TEN) | I2C_M_RD;
	msgs[1].len = sizeof(u64);
	msgs[1].buf = (char *)&buf_be;

	rc = i2c_transfer(client->adapter, msgs, 2);
	if (rc < 0)
		return rc;

	buf = be64_to_cpu(buf_be);
	memcpy(data, &buf, sizeof(u64));

	return 0;
}

static int p8_i2c_occ_putscom(struct i2c_client *client, u32 address, u8 *data)
{
	u32 buf[3];
	ssize_t rc;

	address <<= 1;

	buf[0] = address;
	memcpy(&buf[1], &data[4], sizeof(u32));
	memcpy(&buf[2], data, sizeof(u32));

	rc = i2c_master_send(client, (const char *)buf, sizeof(buf));
	if (rc < 0)
		return rc;
	else if (rc != sizeof(buf))
		return -EIO;

	return 0;
}

static int p8_i2c_occ_putscom_u32(struct i2c_client *client, u32 address,
				  u32 data0, u32 data1)
{
	u8 buf[8];

	memcpy(buf, &data0, 4);
	memcpy(buf + 4, &data1, 4);

	return p8_i2c_occ_putscom(client, address, buf);
}

static int p8_i2c_occ_putscom_be(struct i2c_client *client, u32 address,
				 u8 *data)
{
	unsigned int i;
	u8 buf[8];

	for (i = 0; i < 4; ++i) {
		buf[i] = data[3 - i];
		buf[i + 4] = data[7 - i];
	}

	return p8_i2c_occ_putscom(client, address, buf);
}

static int p8_i2c_occ_send_cmd(struct occ *occ, u8 *cmd)
{
	int i, rc, error;
	unsigned long start;
	u16 data_length;
	struct p8_i2c_occ *p8_i2c_occ = to_p8_i2c_occ(occ);
	struct i2c_client *client = p8_i2c_occ->client;
	struct occ_response *resp = &occ->resp;

	start = jiffies;

	/* set sram address for command */
	rc = p8_i2c_occ_putscom_u32(client, 0x6B070, 0xFFFF6000, 0);
	if (rc)
		goto err;

	/* write command (already be), i2c expects le */
	rc = p8_i2c_occ_putscom_be(client, 0x6B075, cmd);
	if (rc)
		goto err;

	/* trigger OCC attention */
	rc = p8_i2c_occ_putscom_u32(client, 0x6B035, 0x20010000, 0);
	if (rc)
		goto err;

retry:
	/* set sram address for response */
	rc = p8_i2c_occ_putscom_u32(client, 0x6B070, 0xFFFF7000, 0);
	if (rc)
		goto err;

	rc = p8_i2c_occ_getscom(client, 0x6B075, (u8 *)resp);
	if (rc)
		goto err;

	/* check the occ response */
	switch (resp->return_status) {
	case RESP_RETURN_CMD_IN_PRG:
		if (time_after(jiffies,
			       start + msecs_to_jiffies(OCC_TIMEOUT_MS)))
			rc = -EALREADY;
		else {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(msecs_to_jiffies(OCC_CMD_IN_PRG_MS));

			goto retry;
		}
		break;
	case RESP_RETURN_SUCCESS:
		rc = 0;
		break;
	case RESP_RETURN_CMD_INVAL:
	case RESP_RETURN_CMD_LEN:
	case RESP_RETURN_DATA_INVAL:
	case RESP_RETURN_CHKSUM:
		rc = -EINVAL;
		break;
	case RESP_RETURN_OCC_ERR:
		rc = -EREMOTE;
		break;
	default:
		rc = -EFAULT;
	}

	if (rc < 0) {
		error = resp->return_status;
		dev_warn(&client->dev, "occ bad response:%d\n", error);
		goto done;
	}

	data_length = get_unaligned_be16(&resp->data_length_be);
	if (data_length > OCC_RESP_DATA_BYTES) {
		rc = -EMSGSIZE;
		dev_warn(&client->dev, "occ bad data length:%d\n",
			 data_length);
		goto assign;
	}

	for (i = 8; i < data_length + 7; i += 8) {
		rc = p8_i2c_occ_getscom(client, 0x6B075, ((u8 *)resp) + i);
		if (rc)
			goto err;
	}

	occ_reset_error(occ);
	return 0;

err:
	dev_err(&client->dev, "i2c scom op failed rc:%d\n", rc);
assign:
	error = rc;
done:
	occ_set_error(occ, error);
	return rc;
}

static int p8_i2c_occ_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	int rc;
	struct occ *occ;
	struct p8_i2c_occ *p8_i2c_occ = devm_kzalloc(&client->dev,
						     sizeof(*p8_i2c_occ),
						     GFP_KERNEL);
	if (!p8_i2c_occ)
		return -ENOMEM;

	p8_i2c_occ->client = client;

	occ = &p8_i2c_occ->occ;
	occ->bus_dev = &client->dev;
	occ->groups[0] = &occ->group;
	occ->poll_cmd_data = 0x10;
	occ->send_cmd = p8_i2c_occ_send_cmd;
	mutex_init(&occ->lock);

	dev_set_drvdata(&client->dev, occ);

	/* no need to lock yet */
	rc = occ_poll(occ);
	if (rc < 0) {
		dev_err(occ->bus_dev, "failed to get OCC poll response: %d\n",
			rc);
		return rc;
	}

	occ_parse_poll_response(occ);

	rc = occ_setup_sensor_attrs(occ);
	if (rc) {
		dev_err(occ->bus_dev, "failed to setup p8 attrs: %d\n", rc);
		return rc;
	}

	occ->hwmon = devm_hwmon_device_register_with_groups(occ->bus_dev,
							    "p8_occ", occ,
							    occ->groups);
	if (IS_ERR(occ->hwmon)) {
		rc = PTR_ERR(occ->hwmon);
		dev_err(occ->bus_dev, "failed to register hwmon device: %d\n",
			rc);
		return rc;
	}

	rc = occ_create_status_attrs(occ);
	if (rc) {
		dev_err(occ->bus_dev, "failed to setup p8 status attrs: %d\n",
			rc);
		return rc;
	}

	atomic_inc(&occ_num_occs);

	return 0;
}

static int p8_i2c_occ_remove(struct i2c_client *client)
{
	struct occ *occ = dev_get_drvdata(&client->dev);

	occ_remove_status_attrs(occ);

	atomic_dec(&occ_num_occs);

	return 0;
}

static const struct of_device_id p8_i2c_occ_of_match[] = {
	{ .compatible = "ibm,p8-occ-hwmon" },
	{}
};
MODULE_DEVICE_TABLE(of, p8_i2c_occ_of_match);

static const unsigned short p8_i2c_occ_addr[] = { 0x50, 0x51, I2C_CLIENT_END };

static struct i2c_driver p8_i2c_occ_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name = "occ-hwmon",
		.of_match_table = p8_i2c_occ_of_match,
	},
	.probe = p8_i2c_occ_probe,
	.remove = p8_i2c_occ_remove,
	.address_list = p8_i2c_occ_addr,
};

module_i2c_driver(p8_i2c_occ_driver);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("BMC P8 OCC hwmon driver");
MODULE_LICENSE("GPL");
