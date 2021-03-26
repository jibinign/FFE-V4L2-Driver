// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * V4L2 driver with Frame Feed Emulator
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/platform_device.h>

static void p_release(struct device *dev)
{
	dev_info(dev, "%s", __func__);
}

static struct platform_device p_device = {
	.name = KBUILD_MODNAME,
	.id = PLATFORM_DEVID_NONE,
	.dev = {
		.release = p_release,
	},
};

static int p_probe(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "%s\n", __func__);
	return 0;
}

static int p_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "%s\n", __func__);
	return 0;
}

static struct platform_driver p_driver = {
	.probe = p_probe,
	.remove = p_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
	},
};

static int __init ffe_v4l2_init(void)
{
	int ret;

	pr_info("%s\n", __func__);
	platform_device_register(&p_device);
	ret = platform_driver_probe(&p_driver, p_probe);
	if (ret) {
		pr_err("%s: platform driver, %s registration error..\n", __func__, p_driver.driver.name);
		platform_device_unregister(&p_device);
	}
	return ret;
}

static void __exit ffe_v4l2_exit(void)
{
	pr_info("%s\n", __func__);
	platform_driver_unregister(&p_driver);
	platform_device_unregister(&p_device);
}

module_init(ffe_v4l2_init);
module_exit(ffe_v4l2_exit);

MODULE_DESCRIPTION("V4L2 Driver with FFE");
MODULE_LICENSE("GPL");
