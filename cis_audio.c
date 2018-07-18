#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/cis_core.h>
#include <linux/cis_dev.h>
#include "../uevent/cis_uio.h"

static int g_debug_mask = DBG_ERR;
module_param_named(debug, g_debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);
#define dbg_prt(level, fmt, args...) dbg_info("cis_audio", g_debug_mask, level, fmt, ##args)

#define CIS_AUDIO_MAGIC					'a'

/* Command ID for HAL */
#define CIS_AUDIO_CMD_ID_GET_PA_STATE	_IO(CIS_AUDIO_MAGIC, 0x01)
#define CIS_AUDIO_CMD_ID_SET_PA_STATE	_IO(CIS_AUDIO_MAGIC, 0x02)

#define CIS_AUDIO_CMD_ID_GET_CLIP_OVERRUN	_IO(CIS_AUDIO_MAGIC, 0x03)
#define CIS_AUDIO_CMD_ID_SET_CLIP_OVERRUN	_IO(CIS_AUDIO_MAGIC, 0x04)

struct cis_audio_data {
	unsigned long flag;
	struct cis_audio_info info;

	struct delayed_work work;
};

static struct cis_audio_data g_cis_audio_data;
static struct device_attribute cis_audio_dev_attrs[];
static struct mutex audio_mutex;

extern void set_amp_mute_state(int state);
extern void mxc_amp_gpio_enable(int enable);



static int cis_audio_get_info(enum cis_audio_id id)
{
	int err;
	struct cis_audio_data *data = &g_cis_audio_data;
	struct cis_audio_info *info = &(data->info);

	if (info->debug != MSG_DBG_NULL)
		return 0;
	mutex_lock(&audio_mutex);
	set_bit(id, &(data->flag));
	err = sip_audio_get_info(SIP_AUDIO_TYPE_USUAL, id);
	if (err < 0) {
		dbg_prt(ERR, "(%d)get info failed(%d)!", id, err);
		clear_bit(id, &(data->flag));
		mutex_unlock(&audio_mutex);
		return err;
	}

	err = wait_on_bit(&(data->flag), id, cis_get_info_sched_wait, TASK_INTERRUPTIBLE);
	if (err) {
		if(!test_bit(id, &(data->flag))) {
			dbg_prt(CRIT, "audio: (%d) Arleady report data in receive thread\n", id);
			mutex_unlock(&audio_mutex);
			return 0;
		}
		dbg_prt(ERR, "(%d)wait_on_bit failed(%d)!", id, err);
		clear_bit(id, &(data->flag));
		mutex_unlock(&audio_mutex);
		return err;
	}
	mutex_unlock(&audio_mutex);
	return err;
}

static int cis_audio_set_info(enum cis_audio_id id, unsigned char *value, int len)
{
	return sip_audio_set_info(SIP_AUDIO_TYPE_USUAL, id, value, len);
}

int cis_audio_get_pa_state(enum cis_audio_pa_state *state)
{
	int err;
	struct cis_audio_info *info = &(g_cis_audio_data.info);

	err = cis_audio_get_info(CIS_AUDIO_ID_PA_STATE);
	if (!err && state)
		*state = info->pa_state;

	return err;
}
EXPORT_SYMBOL(cis_audio_get_pa_state);

int cis_audio_set_pa_state(enum cis_audio_pa_state state)
{
	unsigned char value = state;

	return cis_audio_set_info(CIS_AUDIO_ID_PA_STATE, &value, 1);
}
EXPORT_SYMBOL(cis_audio_set_pa_state);

static void cis_audio_pa_state_worker(struct work_struct *work)
{
	int err;
	static int s_retry;
	struct cis_audio_data *data = container_of(work,
		struct cis_audio_data, work.work);

	if (!s_retry) {
		mxc_amp_gpio_enable(1);
		err = cis_audio_set_pa_state(CIS_AUDIO_PA_ST_UNMUTE);
		if (!err){
			s_retry = 1;
			set_amp_mute_state(0);
		}else {
			dbg_prt(ERR, "cis_audio_set_pa_state failed(%d)", err);
			schedule_delayed_work(&(data->work), msecs_to_jiffies(1000));
		}
	}
}

void cis_audio_update_info(enum cis_audio_id id, struct cis_audio_info* audio)
{
	struct cis_audio_data *data = &g_cis_audio_data;
	struct cis_audio_info *info = &(data->info);

	if (info->debug == MSG_DBG_SIMU) {
		if (audio->debug == MSG_DBG_NULL) {
			dbg_prt(INFO, "under simulate mode, not receive real msg");
			return;
		}
	}

	dbg_prt(INFO, "id:%d", id);
	switch (id) {
	case CIS_AUDIO_ID_PA_STATE:
		{
			dbg_prt(UPD, "PA State:%d", audio->pa_state);
			info->pa_state = audio->pa_state;
		}
		break;
	case CIS_AUDIO_ID_CHIME:
		{
			dbg_prt(UPD, "Chime Tone:%d, Period:%d, Cycle:%d, Location:%d",
				audio->chime.tone,
				audio->chime.period,
				audio->chime.cycle,
				audio->chime.location);
			memcpy(&(info->chime), &(audio->chime), sizeof(audio->chime));

			cis_chime_play((info->chime));

			return;
		}
		break;
	case CIS_AUDIO_ID_CLIP_OVERRUN:
		{
			dbg_prt(UPD, "CLIP OVERRUN STATUS:%d", audio->pa_ad_clip_status);
			info->pa_ad_clip_status = audio->pa_ad_clip_status;
		}
		break;
	default:
		dbg_prt(ERR, "invalid(id:0x%x) info", id);
		return;;
	}

	if (test_bit(id, &(data->flag))) {
		clear_bit(id, &(data->flag));
		wake_up_bit(&(data->flag), id);
	}
}
EXPORT_SYMBOL(cis_audio_update_info);

static ssize_t cis_audio_show_property(struct device *dev,
		struct device_attribute *dev_attr, char *buf)
{
	int ret;
	int len;
	ptrdiff_t off = dev_attr - cis_audio_dev_attrs;
	struct cis_audio_info *info = &(g_cis_audio_data.info);

	dbg_prt(INFO, "off:%d", off);
	switch (off) {
	case CIS_AUDIO_ID_DEBUG:
		{
			len = sprintf(buf, "debug mode:%d\n", info->debug);
		}
		break;
	case CIS_AUDIO_ID_PA_STATE:
		{
			ret = cis_audio_get_info(off);
			if (ret)
				len = sprintf(buf, "failed\n");
			else
				len = sprintf(buf, "PA State:0x%x\n", info->pa_state);
		}
		break;
	default:
		len = sprintf(buf, "invalid attr\n");
		break;
	}

	return len;
}

static ssize_t cis_audio_store_property(struct device *dev,
	struct device_attribute *dev_attr, const char *buf, size_t count)
{
	int err;
	int num;
	int value[8];
	unsigned char temp[8];
	struct cis_audio_info audio = {0};
	struct cis_audio_info *info = &(g_cis_audio_data.info);
	ptrdiff_t off = dev_attr - cis_audio_dev_attrs;

	audio.debug = MSG_DBG_SIMU;

	dbg_prt(INFO, "off:%d", off);
	switch (off) {
	case CIS_AUDIO_ID_DEBUG:
		{
			num = sscanf(buf, "%d", value);
			if (num < 1) {
				dbg_prt(ERR, "invalid input param");
				break;
			}
			dbg_prt(PROP, "debug mode:%d", value[0]);
			info->debug = value[0];
		}
		break;
	case CIS_AUDIO_ID_PA_STATE:
		{
			num = sscanf(buf, "%d", value);
			if (num < 1) {
				dbg_prt(ERR, "invalid input param");
				break;
			}
			dbg_prt(PROP, "PA State:%d", value[0]);

			if (info->debug == MSG_DBG_SET) {
				temp[0] = (unsigned char)value[0];
				err = cis_audio_set_info(CIS_AUDIO_ID_PA_STATE, temp, 1);
				if (err) {
					dbg_prt(ERR, "set info failed(%d)", err);
					return -EIO;
				}
			} else {
				audio.pa_state = !!value[0];
				cis_audio_update_info(off, &audio);
			}
		}
		break;
	default:
		break;
	}

	return count;
}

#define CIS_AUDIO_ATTR(_name) \
{.attr = { .name = #_name, .mode = 0664,}, \
	.show = cis_audio_show_property, \
	.store = cis_audio_store_property, \
}

static struct device_attribute cis_audio_dev_attrs[] = {
	CIS_AUDIO_ATTR(debug),
	CIS_AUDIO_ATTR(pa_state),
	CIS_AUDIO_ATTR(chime),
};

static struct attribute *cis_audio_attrs[] = {
	&(cis_audio_dev_attrs[CIS_AUDIO_ID_DEBUG].attr),
	&(cis_audio_dev_attrs[CIS_AUDIO_ID_PA_STATE].attr),
	&(cis_audio_dev_attrs[CIS_AUDIO_ID_CHIME].attr),
	NULL
};

static struct attribute_group cis_audio_group = {
	.attrs = cis_audio_attrs,
};

static int cis_audio_open(struct inode *inode, struct file *file)
{
	file->private_data = &(g_cis_audio_data.info);

	return 0;
}

static int cis_audio_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;

	return 0;
}

static long cis_audio_ioctl(struct file *file, unsigned int cmd_id, unsigned long arg)
{
	int err = 0;
	int temp[8] = {0};
	char value[8] = {0};
	struct cis_audio_info audio = {0};
	unsigned int cmd = _IOC_NR(cmd_id);
	void __user *argp = (void __user *)arg;
	struct cis_audio_info *info = (struct cis_audio_info *)file->private_data;

	dbg_prt(INFO, "cmd:0x%x", cmd);
	switch (cmd_id) {
	case CIS_AUDIO_CMD_ID_GET_PA_STATE:
		{
			err = cis_audio_get_info(CIS_AUDIO_ID_PA_STATE);
			if (err) {
				dbg_prt(ERR, "(0x%x) get info failed(%d)", cmd, err);
				return -EIO;
			}
			err = copy_to_user(argp, &(info->pa_state), sizeof(info->pa_state));
			if (err) {
				dbg_prt(ERR, "Cannot copy data to user space(%d)", cmd);
				return -EFAULT;
			}
			dbg_prt(PROP, "PA State:0x%x", info->pa_state);

			return 0;
		}
		break;
	case CIS_AUDIO_CMD_ID_SET_PA_STATE:
		{
			err = copy_from_user(temp, argp, 1*sizeof(int));
			if (err) {
				dbg_prt(ERR, "copy_from_user failed");
				return -EFAULT;
			}
			dbg_prt(PROP, "PA State:0x%x", temp[0]);

			if (info->debug == MSG_DBG_TEST) {
				audio.pa_state = temp[0];
				cis_audio_update_info(CIS_AUDIO_ID_PA_STATE, &audio);

				return 0;
			}

			value[0] = (unsigned char)temp[0];
			err = cis_audio_set_info(CIS_AUDIO_ID_PA_STATE, value, 1);
			if (err) {
				dbg_prt(ERR, "(0x%x) set info failed(%d)",
					cmd, err);
				return -EIO;
			}

			return 0;
		}
		break;
	case CIS_AUDIO_CMD_ID_GET_CLIP_OVERRUN:
		{
			err = cis_audio_get_info(CIS_AUDIO_ID_CLIP_OVERRUN);
			if (err) {
				dbg_prt(ERR, "(0x%x) get info failed(%d)", cmd, err);
				return -EIO;
			}
			err = copy_to_user(argp, &(info->pa_ad_clip_status), sizeof(info->pa_ad_clip_status));
			if (err) {
				dbg_prt(ERR, "Cannot copy data to user space(%d)", cmd);
				return -EFAULT;
			}
			dbg_prt(PROP, "PA clip overrun:0x%x", info->pa_ad_clip_status);

			return 0;
		}
		break;
	default:
		dbg_prt(ERR, "ctrl_cmd(%d) is not supported!", cmd);
		break;
	}

	return -ENOIOCTLCMD;
}

static const struct file_operations cis_audio_fops = {
	.owner	= THIS_MODULE,
	.open       	= cis_audio_open,
	.release   = cis_audio_release,
	.unlocked_ioctl = cis_audio_ioctl,
};

static struct miscdevice cis_audio_dev = {
	.minor =	MISC_DYNAMIC_MINOR,
	.name = "cis_audio",
	.fops = &cis_audio_fops
};

static void cis_audio_info_init(struct cis_audio_info *info)
{
	info->debug = MSG_DBG_NULL;

	info->pa_state = CIS_ITEM_INT_VALUE_INVALID;
}

static int __init cis_audio_init(void)
{
	int err;
	struct cis_audio_data *data = &g_cis_audio_data;
	struct cis_audio_info *info = &(data->info);

	dbg_prt(INFO, "enter");
	mutex_init(&audio_mutex);
	err = misc_register(&cis_audio_dev);
	if (err) {
		dbg_prt(ERR, "misc_register failed(%d)", err);
		return err;
	}

	err = sysfs_create_group(&(cis_audio_dev.this_device->kobj),
		&cis_audio_group);
	if (err) {
		dbg_prt(ERR, "sysfs_create_group failed(%d)", err);
		goto ERR_SYSFILE_CREATE;
	}

	data->flag = 0;
	cis_audio_info_init(info);

	INIT_DELAYED_WORK(&(data->work), cis_audio_pa_state_worker);
	schedule_delayed_work(&(data->work), msecs_to_jiffies(500));

	return 0;
ERR_SYSFILE_CREATE:
	misc_deregister(&cis_audio_dev);
	return err;
}

static void __exit cis_audio_exit(void)
{
	misc_deregister(&cis_audio_dev);
	sysfs_remove_group(&(cis_audio_dev.this_device->kobj),
		&cis_audio_group);
}
late_initcall(cis_audio_init);
module_exit(cis_audio_exit);

MODULE_AUTHOR("null@ecarx.com.cn");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Get the audio infomation");
