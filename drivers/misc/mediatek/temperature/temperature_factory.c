/*
* Copyright (C) 2018 MediaTek Inc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See http://www.gnu.org/licenses/gpl-2.0.html for more details.
*/

#include "inc/temperature_factory.h"

static int temp_factory_open(struct inode *inode, struct file *file)
{
	file->private_data = temp_context_obj;

	if (file->private_data == NULL) {
		TEMP_ERR("null pointer!!\n");
		return -EINVAL;
	}
	return nonseekable_open(inode, file);
}

static int temp_factory_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static long temp_factory_unlocked_ioctl(struct file *file,
	unsigned int cmd, unsigned long arg)
{
	long err = 0;
	struct temp_context *cxt = temp_context_obj;
	void __user *ptr = (void __user *)arg;
	int dat;


	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE,
			(void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ,
			(void __user *)arg, _IOC_SIZE(cmd));

	if (err) {
		TEMP_ERR("access error: %08X, (%2d, %2d)\n",
			cmd, _IOC_DIR(cmd), _IOC_SIZE(cmd));
		return -EFAULT;
	}

	switch (cmd) {
	case TEMPERATURE_IOCTL_INIT:
		TEMP_LOG("TEMPERATURE_IOCTL_INIT enable\n");
		if (cxt->temp_ctl.enable_nodata != NULL) {
			err = cxt->temp_ctl.enable_nodata(1);
			if (err < 0) {
				TEMP_ERR("TEMPERATURE_IOCTL_INIT fail!\n");
				break;
			}
		}
		break;
	case TEMPERATURE_GET_DATA:
		if (cxt->temp_data.get_raw_data != NULL) {
			err = cxt->temp_data.get_raw_data(&dat);
			if (err < 0) {
				TEMP_ERR("TEMPERATURE_GET_DATA fail!\n");
				break;
			}
		}

		if (copy_to_user(ptr, &dat, sizeof(dat))) {
			err = -EFAULT;
			break;
		}
		break;

	default:
		TEMP_ERR("unknown IOCTL: 0x%08x\n", cmd);
		err = -ENOIOCTLCMD;
		break;

	}
	return err;
}

static const struct file_operations temp_factory_fops = {
	.open = temp_factory_open,
	.release = temp_factory_release,
	.unlocked_ioctl = temp_factory_unlocked_ioctl,
};

static struct miscdevice temp_factory_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "temperature",
	.fops = &temp_factory_fops,
};

int temp_factory_device_init(void)
{
	int error = 0;
	struct temp_context *cxt = temp_context_obj;

	if (!cxt->temp_ctl.is_use_common_factory) {
		TEMP_LOG("Node of '/dev/temperature' has already existed!\n");
		return -1;
	}
	error = misc_register(&temp_factory_device);
	if (error) {
		TEMP_ERR("temp_factory_device register failed\n");
		error = -1;
	}
	return error;
}
