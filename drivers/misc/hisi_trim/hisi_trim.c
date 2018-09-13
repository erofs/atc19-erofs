#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>

struct hisi_trim_mbox {
	struct mutex lock;
	struct mbox_client cl;
	struct mbox_chan *mbox;
	u32 msg[8];
};

static struct hisi_trim_mbox hisi_trim_chan;

void hisi_trim_tx_done(struct mbox_client *cl, void *mssg, int r)
{
	memcpy(hisi_trim_chan.msg, mssg, sizeof(hisi_trim_chan.msg));
}

int parse_para_from_buf(const char *buf, const char *argv[], int argc)
{
	const char *para_cmd = buf;
	int para_id = 1;

	while ((para_cmd = strpbrk(para_cmd + 1, " :")))
		para_id++;

	if (para_id != argc)
		return -1;

	para_id = 0;
	para_cmd = buf;
	while (para_id < argc) {
		argv[para_id++] = para_cmd;

		para_cmd = strpbrk(para_cmd, " :");
		if (!para_cmd)
			break;

		para_cmd++;
	}

	return 0;
}

int trim_cmd_parse(const char *buf, u32 *msg)
{
	const char *argv[1] = {0};
	u32 result = 0;

	if (parse_para_from_buf(buf, argv, 1)) {
		pr_err("error, arg number not right\n");
		return -1;
	}

	result = simple_strtoul(argv[0], NULL, 0);
	if (result < 675 || result > 1200) {
		pr_err("para2 %u out of range\n", result);
		return -1;
	}
	msg[1] |= result;

	return 0;
}

#define hisi_trim_simple_func(name, msg_id, opp_id)			\
static ssize_t name##_state##opp_id##_store(struct device *dev,		\
			    struct device_attribute *attr,		\
			    const char *buf, size_t count)		\
{									\
	s32 ret = 0;							\
									\
	mutex_lock(&hisi_trim_chan.lock);				\
	memset(hisi_trim_chan.msg, 0, sizeof(hisi_trim_chan.msg));	\
									\
	hisi_trim_chan.msg[1] |= (11 << 24);				\
	hisi_trim_chan.msg[1] |= (msg_id << 20) | (opp_id << 16);	\
	hisi_trim_chan.msg[0] = 0x0008030F;				\
									\
	ret = trim_cmd_parse(buf, hisi_trim_chan.msg);			\
	if (ret)							\
		goto msg_err;						\
									\
	mbox_send_message(hisi_trim_chan.mbox, hisi_trim_chan.msg);	\
									\
	if (hisi_trim_chan.msg[1] & 0x8000)				\
		dev_info(dev, "set volt %dmV success\n",		\
			 hisi_trim_chan.msg[1] & 0x7FFF);		\
									\
msg_err:								\
	mutex_unlock(&hisi_trim_chan.lock);				\
	return count;							\
}									\
static DEVICE_ATTR_WO(name##_state##opp_id)

hisi_trim_simple_func(little, 0, 0);
hisi_trim_simple_func(little, 0, 1);
hisi_trim_simple_func(little, 0, 2);
hisi_trim_simple_func(little, 0, 3);
hisi_trim_simple_func(little, 0, 4);
hisi_trim_simple_func(big,    1, 0);
hisi_trim_simple_func(big,    1, 1);
hisi_trim_simple_func(big,    1, 2);
hisi_trim_simple_func(big,    1, 3);
hisi_trim_simple_func(big,    1, 4);
hisi_trim_simple_func(gpu,    2, 0);
hisi_trim_simple_func(gpu,    2, 1);
hisi_trim_simple_func(gpu,    2, 2);
hisi_trim_simple_func(gpu,    2, 3);
hisi_trim_simple_func(gpu,    2, 4);
hisi_trim_simple_func(gpu,    2, 5);

static struct attribute *hisi_trim_little_attrs[] = {
	&dev_attr_little_state0.attr,
	&dev_attr_little_state1.attr,
	&dev_attr_little_state2.attr,
	&dev_attr_little_state3.attr,
	&dev_attr_little_state4.attr,
	NULL,
};

static struct attribute *hisi_trim_big_attrs[] = {
	&dev_attr_big_state0.attr,
	&dev_attr_big_state1.attr,
	&dev_attr_big_state2.attr,
	&dev_attr_big_state3.attr,
	&dev_attr_big_state4.attr,
	NULL,
};

static struct attribute *hisi_trim_gpu_attrs[] = {
	&dev_attr_gpu_state0.attr,
	&dev_attr_gpu_state1.attr,
	&dev_attr_gpu_state2.attr,
	&dev_attr_gpu_state3.attr,
	&dev_attr_gpu_state4.attr,
	&dev_attr_gpu_state5.attr,
	NULL,
};

static const struct attribute_group hisi_trim_little_group = {
	.attrs = hisi_trim_little_attrs,
};

static const struct attribute_group hisi_trim_big_group = {
	.attrs = hisi_trim_big_attrs,
};

static const struct attribute_group hisi_trim_gpu_group = {
	.attrs = hisi_trim_gpu_attrs,
};

const struct attribute_group *hisi_trim_groups[] = {
	&hisi_trim_little_group,
	&hisi_trim_big_group,
	&hisi_trim_gpu_group,
	NULL,
};

static int hisi_trim_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;

	mutex_init(&hisi_trim_chan.lock);
	hisi_trim_chan.cl.dev = dev;
	hisi_trim_chan.cl.tx_done = hisi_trim_tx_done;
	hisi_trim_chan.cl.tx_block = true;
	hisi_trim_chan.cl.tx_tout = 500;
	hisi_trim_chan.cl.knows_txdone = false;

	hisi_trim_chan.mbox = mbox_request_channel(&hisi_trim_chan.cl, 0);
	if (IS_ERR(hisi_trim_chan.mbox)) {
		dev_err(dev, "request mailbox err\n");
		return -ENOMEM;
	}

	ret = sysfs_create_groups(&dev->kobj, hisi_trim_groups);
	if (ret)
		goto free_chan;

	return 0;

free_chan:
	mbox_free_channel(hisi_trim_chan.mbox);
	return ret;
}

static const struct of_device_id hisi_trim_of_match[] = {
	{ .compatible = "hisilicon,trim", },
	{}
};

static struct platform_driver hisi_trim_driver = {
	.driver = {
		.name = "hisi-trim",
		.of_match_table = hisi_trim_of_match,
	},
	.probe = hisi_trim_probe,
};

static int __init hisi_trim_init(void)
{
	return platform_driver_register(&hisi_trim_driver);
}
subsys_initcall(hisi_trim_init);

MODULE_AUTHOR("kevin.wangtao@hisilicon.com>");
MODULE_DESCRIPTION("HISILICON TRIM DRIVER");
MODULE_LICENSE("GPL");
