// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * V4L2 driver with Frame Feed Emulator
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <linux/errno.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/videobuf2-vmalloc.h>

MODULE_DESCRIPTION("V4L2 Driver with FFE");
MODULE_LICENSE("GPL");

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

struct ffe_buffer {
	struct vb2_v4l2_buffer		vb;
	struct list_head		list;
};

struct dev_data {
	struct platform_device		*pdev;
	struct v4l2_device		v4l2_dev;
	struct video_device		vdev;
	struct list_head		devlist;
	struct mutex			mutex;
	struct vb2_queue		queue;
	spinlock_t			s_lock;
};

static int queue_setup(struct vb2_queue *vq, unsigned int *nbuffers, unsigned int *nplanes, unsigned int sizes[], struct device *alloc_ctxs[])
{
	pr_info("%s\n", __func__);
	return 0;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	pr_info("%s\n", __func__);
	return 0;
}

static void buffer_queue(struct vb2_buffer *vb)
{
	pr_info("%s\n", __func__);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	pr_info("%s\n", __func__);
	return 0;
}

static void stop_streaming(struct vb2_queue *vq)
{
	pr_info("%s\n", __func__);
}

static void ffe_lock(struct vb2_queue *vq)
{
	struct dev_data *dev = vb2_get_drv_priv(vq);

	pr_info("%s\n", __func__);
	mutex_lock(&dev->mutex);
}

static void ffe_unlock(struct vb2_queue *vq)
{
	struct dev_data *dev = vb2_get_drv_priv(vq);

	pr_info("%s\n", __func__);
	mutex_unlock(&dev->mutex);
}

static const struct vb2_ops ffe_qops = {
	.queue_setup			= queue_setup,
	.buf_prepare			= buffer_prepare,
	.buf_queue			= buffer_queue,
	.start_streaming		= start_streaming,
	.stop_streaming			= stop_streaming,
	.wait_prepare			= ffe_unlock,
	.wait_finish			= ffe_lock,
};

static int vidioc_querycap(struct file *file, void  *priv, struct v4l2_capability *cap)
{
	struct dev_data *dev = video_drvdata(file);

	pr_info("%s\n", __func__);
	strcpy(cap->driver, KBUILD_MODNAME);
	strcpy(cap->card, KBUILD_MODNAME);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", dev->v4l2_dev.name);
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static const struct v4l2_ioctl_ops ffe_ioctl_ops = {
	.vidioc_querycap      = vidioc_querycap,
};

static const struct v4l2_file_operations ffe_fops = {
	.owner				= THIS_MODULE,
	.open				= v4l2_fh_open,
	.release			= vb2_fop_release,
	.read				= vb2_fop_read,
	.poll				= vb2_fop_poll,
	.unlocked_ioctl			= video_ioctl2,
	.mmap				= vb2_fop_mmap,
};

static int p_probe(struct platform_device *pdev)
{
	struct dev_data *dev;
	struct video_device *vdev;
	struct vb2_queue *q;
	int ret;

	dev_info(&pdev->dev, "%s\n", __func__);
	dev = devm_kzalloc(&pdev->dev, sizeof(struct dev_data), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->pdev = pdev;
	dev_set_drvdata(&pdev->dev, dev);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "%s: v4l2 device registration failed..\n", __func__);
		return ret;
	}

	spin_lock_init(&dev->s_lock);

	q = &dev->queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct ffe_buffer);
	q->ops = &ffe_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(&pdev->dev, "%s: vb2 queue init failed..\n", __func__);
		v4l2_device_unregister(&dev->v4l2_dev);
		return ret;
	}

	mutex_init(&dev->mutex);

	vdev = &dev->vdev;
	strlcpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->release = video_device_release_empty;
	vdev->fops = &ffe_fops;
	vdev->ioctl_ops = &ffe_ioctl_ops;
	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->queue = q;
	vdev->lock = &dev->mutex;
	video_set_drvdata(vdev, dev);

	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: video device registration failed..\n", __func__);
		v4l2_device_unregister(&dev->v4l2_dev);
		video_device_release(&dev->vdev);
		return ret;
	}
	v4l2_info(&dev->v4l2_dev, "%s: V4L2 device registered as %s\n", __func__, video_device_node_name(vdev));
	return 0;
}

static int p_remove(struct platform_device *pdev)
{
	struct dev_data *dev;

	dev_info(&pdev->dev, "%s\n", __func__);
	dev = platform_get_drvdata(pdev);
	video_unregister_device(&dev->vdev);
	v4l2_device_unregister(&dev->v4l2_dev);
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

	ret = platform_device_register(&p_device);
	if (ret) {
		pr_err("%s: platform device, %s registration failed..\n", __func__, p_device.name);
		return ret;
	}

	ret = platform_driver_probe(&p_driver, p_probe);
	if (ret) {
		pr_err("%s: platform driver, %s registration failed..\n", __func__, p_driver.driver.name);
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
