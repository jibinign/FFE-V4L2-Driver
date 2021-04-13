// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * V4L2 driver with Frame Feed Emulator
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/freezer.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-vmalloc.h>


#define VERSION				"0.1.0"

#define MAX_WIDTH			1920
#define MAX_HEIGHT			1080
#define MAX_FPS				1000

MODULE_DESCRIPTION("V4L2 Driver with FFE");
MODULE_LICENSE("GPL");
MODULE_VERSION(VERSION);

static void p_release(struct device *dev)
{
	dev_info(dev, "%s", __func__);
}

static struct platform_device p_device = {
	.name		= KBUILD_MODNAME,
	.id		= PLATFORM_DEVID_NONE,
	.dev		= {
		.release		= p_release,
	},
};

static const struct v4l2_fract
	tpf_min = {.numerator = 1, .denominator = MAX_FPS},
	tpf_max = {.numerator = MAX_FPS, .denominator = 1},
	tpf_default = {.numerator = 1, .denominator = 30};			/* 30 frames per second */

struct ffe_fmt {
	const char			*name;
	u32				fourcc;
	u8				depth;
	bool				is_yuv;
};

static struct ffe_fmt formats[] = {
	{
		.name			= "4:2:2, packed, YUYV",
		.fourcc			= V4L2_PIX_FMT_YUYV,
		.depth			= 16,
		.is_yuv			= true,
	},
	{
		.name			= "4:2:2, packed, UYVY",
		.fourcc			= V4L2_PIX_FMT_UYVY,
		.depth			= 16,
		.is_yuv			= true,
	},
	{
		.name			= "4:2:2, packed, YVYU",
		.fourcc			= V4L2_PIX_FMT_YVYU,
		.depth			= 16,
		.is_yuv			= true,
	},
	{
		.name			= "4:2:2, packed, VYUY",
		.fourcc			= V4L2_PIX_FMT_VYUY,
		.depth			= 16,
		.is_yuv			= true,
	},
	{
		.name			= "RGB565 (LE)",
		.fourcc			= V4L2_PIX_FMT_RGB565,
		.depth			= 16,
	},
	{
		.name			= "RGB565 (BE)",
		.fourcc			= V4L2_PIX_FMT_RGB565X,
		.depth			= 16,
	},
	{
		.name			= "RGB555 (LE)",
		.fourcc			= V4L2_PIX_FMT_RGB555,
		.depth			= 16,
	},
	{
		.name			= "RGB555 (BE)",
		.fourcc			= V4L2_PIX_FMT_RGB555X,
		.depth			= 16,
	},
	{
		.name			= "RGB24 (LE)",
		.fourcc			= V4L2_PIX_FMT_RGB24,
		.depth			= 24,
	},
	{
		.name			= "RGB24 (BE)",
		.fourcc			= V4L2_PIX_FMT_BGR24,
		.depth			= 24,
	},
	{
		.name			= "RGB32 (LE)",
		.fourcc			= V4L2_PIX_FMT_RGB32,
		.depth			= 32,
	},
	{
		.name			= "RGB32 (BE)",
		.fourcc			= V4L2_PIX_FMT_BGR32,
		.depth			= 32,
	},
};

static struct ffe_fmt *get_format(u32 pixelformat)
{
	const struct ffe_fmt *fmt;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		fmt = &formats[i];
		if (fmt->fourcc == pixelformat)
			break;
	}

	if (i == ARRAY_SIZE(formats))
		return NULL;

	return &formats[i];
}

struct ffe_buffer {
	struct vb2_buffer		vb;
	struct list_head		list;
	struct v4l2_buffer		v4l2_buf;
};

struct ffe_dmaq {
	struct list_head		active;
	struct task_struct		*kthread;
	wait_queue_head_t		wq;
	int				frame;
	int				jiffies;
};

struct dev_data {
	struct platform_device		*pdev;
	struct v4l2_device		v4l2_dev;
	struct video_device		vdev;
	struct mutex			mutex;
	struct vb2_queue		queue;
	struct ffe_dmaq			vidq;
	struct ffe_fmt			*fmt;
	struct v4l2_fract		time_per_frame;
	spinlock_t			s_lock;
	unsigned long			jiffies;
	int				mv_count, input;
	unsigned int			f_count;
	unsigned int			width, height, pixelsize;
	u8				bars[8][3], alpha;
	u8				line[MAX_WIDTH * 8];
};

/* ------------------------------------ {    R,    G,    B} */

#define COLOR_WHITE			{ 0xFF, 0xFF, 0xFF}
#define COLOR_YELLOW			{ 0xFF, 0xFF, 0x00}
#define COLOR_CYAN			{ 0x00, 0xFF, 0xFF}
#define COLOR_GREEN			{ 0x00, 0xFF, 0x00}
#define COLOR_MAGENTA			{ 0xFF, 0x00, 0xFF}
#define COLOR_RED			{ 0xFF, 0x00, 0x00}
#define COLOR_BLUE			{ 0x00, 0x00, 0xFF}
#define COLOR_BLACK			{ 0x00, 0x00, 0x00}

/* ----------standard color bar---------- */
static const u8 bar[8][3] = {
	COLOR_WHITE, COLOR_YELLOW, COLOR_CYAN, COLOR_GREEN, COLOR_MAGENTA, COLOR_RED, COLOR_BLUE, COLOR_BLACK
};

static void generate_color_pix(struct dev_data *dev, u8 *buf, int colorpos, bool odd)
{
	u8 r_y, g_u, b_v, alpha;
	u8 *p;
	int color;

	v4l2_info(&dev->v4l2_dev, "%s\t", __func__);
	alpha = dev->alpha;
	r_y = dev->bars[colorpos][0];			/* R or Y component */
	g_u = dev->bars[colorpos][1];			/* G or U component */
	b_v = dev->bars[colorpos][2];			/* B or V component */

	for (color = 0; color < dev->pixelsize; color++) {
		p = buf + color;

		switch (dev->fmt->fourcc) {
		case V4L2_PIX_FMT_YUYV:
			switch (color) {
			case 0:
				*p = r_y;
				break;
			case 1:
				*p = odd ? b_v : g_u;
				break;
			}
			break;
		case V4L2_PIX_FMT_UYVY:
			switch (color) {
			case 0:
				*p = odd ? b_v : g_u;
				break;
			case 1:
				*p = r_y;
				break;
			}
			break;
		case V4L2_PIX_FMT_YVYU:
			switch (color) {
			case 0:
				*p = r_y;
				break;
			case 1:
				*p = odd ? g_u : b_v;
				break;
			}
			break;
		case V4L2_PIX_FMT_VYUY:
			switch (color) {
			case 0:
				*p = odd ? g_u : b_v;
				break;
			case 1:
				*p = r_y;
				break;
			}
			break;
		case V4L2_PIX_FMT_RGB565:
			switch (color) {
			case 0:
				*p = (g_u << 5) | b_v;
				break;
			case 1:
				*p = (r_y << 3) | (g_u >> 3);
				break;
			}
			break;
		case V4L2_PIX_FMT_RGB565X:
			switch (color) {
			case 0:
				*p = (r_y << 3) | (g_u >> 3);
				break;
			case 1:
				*p = (g_u << 5) | b_v;
				break;
			}
			break;
		case V4L2_PIX_FMT_RGB555:
			switch (color) {
			case 0:
				*p = (g_u << 5) | b_v;
				break;
			case 1:
				*p = (alpha & 0x80) | (r_y << 2) | (g_u >> 3);
				break;
			}
			break;
		case V4L2_PIX_FMT_RGB555X:
			switch (color) {
			case 0:
				*p = (alpha & 0x80) | (r_y << 2) | (g_u >> 3);
				break;
			case 1:
				*p = (g_u << 5) | b_v;
				break;
			}
			break;
		case V4L2_PIX_FMT_RGB24:
			switch (color) {
			case 0:
				*p = r_y;
				break;
			case 1:
				*p = g_u;
				break;
			case 2:
				*p = b_v;
				break;
			}
			break;
		case V4L2_PIX_FMT_BGR24:
			switch (color) {
			case 0:
				*p = b_v;
				break;
			case 1:
				*p = g_u;
				break;
			case 2:
				*p = r_y;
				break;
			}
			break;
		case V4L2_PIX_FMT_RGB32:
			switch (color) {
			case 0:
				*p = alpha;
				break;
			case 1:
				*p = r_y;
				break;
			case 2:
				*p = g_u;
				break;
			case 3:
				*p = b_v;
				break;
			}
			break;
		case V4L2_PIX_FMT_BGR32:
			switch (color) {
			case 0:
				*p = b_v;
				break;
			case 1:
				*p = g_u;
				break;
			case 2:
				*p = r_y;
				break;
			case 3:
				*p = alpha;
				break;
			}
			break;
		}
	}
}

static void generate_colorbar(struct dev_data *dev)
{
	u8 r, g, b;
	int i;
	bool is_yuv;
	unsigned int pixelsize, pixelsize2;
	int colorpos;
	u8 *pos;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	for (i = 0; i < 8; i++) {
		r = bar[i][0];
		g = bar[i][1];
		b = bar[i][2];
		is_yuv = dev->fmt->is_yuv;

		switch (dev->fmt->fourcc) {
		case V4L2_PIX_FMT_RGB565:
		case V4L2_PIX_FMT_RGB565X:
			r >>= 3;
			g >>= 2;
			b >>= 3;
			break;
		case V4L2_PIX_FMT_RGB555:
		case V4L2_PIX_FMT_RGB555X:
			r >>= 3;
			g >>= 3;
			b >>= 3;
			break;
		default:
			break;
		}

		if (is_yuv) {
			dev->bars[i][0] = (((16829 * r + 33039 * g + 6416 * b  + 32768) >> 16) + 16);		/* Luma Y */
			dev->bars[i][1] = (((-9714 * r - 19070 * g + 28784 * b + 32768) >> 16) + 128);		/* Chrominanace Cb or U */
			dev->bars[i][2] = (((28784 * r - 24103 * g - 4681 * b  + 32768) >> 16) + 128);		/* Chrominance Cr or V */
		} else {
			dev->bars[i][0] = r;
			dev->bars[i][1] = g;
			dev->bars[i][2] = b;
		}
	}

	pixelsize = dev->pixelsize;
	pixelsize2 = 2 * pixelsize;

	for (colorpos = 0; colorpos < 16; colorpos++) {
		u8 pix[8];
		int wstart = colorpos * dev->width / 8;
		int wend = (colorpos+1) * dev->width / 8;
		int w = wstart / 2 * 2;

		pos = dev->line + w * pixelsize;
		generate_color_pix(dev, &pix[0], colorpos % 8, 0);
		generate_color_pix(dev, &pix[pixelsize], colorpos % 8, 1);

		while (w < wend) {
			memcpy(pos, pix, pixelsize2);
			pos += pixelsize2;
			w += 2;
		}
	}
}

static void ffe_fillbuff(struct dev_data *dev, struct ffe_buffer *buf)
{
	void *vbuf = vb2_plane_vaddr(&buf->vb, 0);
	int size, height, i;
	u8 *start;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (!vbuf) {
		v4l2_err(&dev->v4l2_dev, "%s: buffer error..\n", __func__);
		return;
	}

	size = dev->width * dev->pixelsize;
	height = dev->height;
	start = dev->line + (dev->mv_count % dev->width) * dev->pixelsize;

	for (i = 0; i < height; i++)
		memcpy(vbuf + i * size, start, size);

	dev->mv_count += 2;
	buf->v4l2_buf.field = V4L2_FIELD_INTERLACED;
	buf->v4l2_buf.sequence = dev->f_count++;
}

static void ffe_thread_tick(struct dev_data *dev)
{
	struct ffe_dmaq *q;
	struct ffe_buffer *buf;
	unsigned long flags = 0;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	q = &dev->vidq;
	spin_lock_irqsave(&dev->s_lock, flags);

	if (list_empty(&q->active)) {
		v4l2_err(&dev->v4l2_dev, "%s: No active queue\n", __func__);
		spin_unlock_irqrestore(&dev->s_lock, flags);
		return;
	}

	buf = list_entry(q->active.next, struct ffe_buffer, list);
	list_del(&buf->list);
	spin_unlock_irqrestore(&dev->s_lock, flags);
	ffe_fillbuff(dev, buf);
	vb2_buffer_done(&buf->vb, VB2_BUF_STATE_DONE);
}

static void ffe_sleep(struct dev_data *dev)
{
	struct ffe_dmaq *q = &dev->vidq;
	int timeout;
	DECLARE_WAITQUEUE(wait, current);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	add_wait_queue(&q->wq, &wait);

	if (kthread_should_stop()) {
		v4l2_err(&dev->v4l2_dev, "%s: kthread should stop\n", __func__);
		remove_wait_queue(&q->wq, &wait);
		try_to_freeze();
		return;
	}

	timeout = msecs_to_jiffies((dev->time_per_frame.numerator * 1000) / dev->time_per_frame.denominator);
	ffe_thread_tick(dev);
	schedule_timeout_interruptible(timeout);
	remove_wait_queue(&q->wq, &wait);
	try_to_freeze();
}

static int ffe_thread(void *data)
{
	struct dev_data *dev = data;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	set_freezable();

	while (1) {
		ffe_sleep(dev);
		if (kthread_should_stop())
			break;
	}
	v4l2_info(&dev->v4l2_dev, "%s: exit\n", __func__);
	return 0;
}

static int ffe_start_generating(struct dev_data *dev)
{
	struct ffe_dmaq *q = &dev->vidq;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	dev->mv_count = 0;
	dev->jiffies = jiffies;
	q->frame = 0;
	q->jiffies = jiffies;
	q->kthread = kthread_run(ffe_thread, dev, "%s", dev->v4l2_dev.name);

	if (IS_ERR(q->kthread)) {
		v4l2_err(&dev->v4l2_dev, "%s: kernel_thread() failed..\n", __func__);
		return PTR_ERR(q->kthread);
	}

	wake_up_interruptible(&q->wq);
	return 0;
}

static void ffe_stop_generating(struct dev_data *dev)
{
	struct ffe_dmaq *q = &dev->vidq;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (q->kthread) {
		kthread_stop(q->kthread);
		q->kthread = NULL;
	}

	while (!list_empty(&q->active)) {
		struct ffe_buffer *buf;

		buf = list_entry(q->active.next, struct ffe_buffer, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
	}
}

static int queue_setup(struct vb2_queue *vq, unsigned int *nbuffers, unsigned int *nplanes, unsigned int sizes[], struct device *alloc_ctxs[])
{
	struct dev_data *dev;
	unsigned long size;

	dev = vb2_get_drv_priv(vq);
	size = dev->width * dev->height * dev->pixelsize;
	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);

	*nplanes = 1;
	sizes[0] = size;

	v4l2_info(&dev->v4l2_dev, "%s: count = %d, size = %ld\n", __func__, *nbuffers, size);
	return 0;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct dev_data *dev;
	struct ffe_buffer *buf;
	unsigned long size;

	dev = vb2_get_drv_priv(vb->vb2_queue);
	buf = container_of(vb, struct ffe_buffer, vb);
	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);

	if (dev->width < 48 || dev->width > MAX_WIDTH || dev->height < 32 || dev->height > MAX_HEIGHT) {
		v4l2_err(&dev->v4l2_dev, "%s: width or/and height is/are not in expected range..\n", __func__);
		return -EINVAL;
	}

	size = dev->width * dev->height * dev->pixelsize;
	if (vb2_plane_size(vb, 0) < size) {
		v4l2_err(&dev->v4l2_dev, "%s: data will not fit into the plane (%lu < %lu)..\n", __func__, vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(&buf->vb, 0, size);
	generate_colorbar(dev);
	return 0;
}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct dev_data *dev;
	struct ffe_buffer *buf;
	struct ffe_dmaq *vidq;
	unsigned long flags = 0;

	dev = vb2_get_drv_priv(vb->vb2_queue);
	buf = container_of(vb, struct ffe_buffer, vb);
	vidq = &dev->vidq;
	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);

	spin_lock_irqsave(&dev->s_lock, flags);
	list_add_tail(&buf->list, &vidq->active);
	spin_unlock_irqrestore(&dev->s_lock, flags);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct dev_data *dev;
	int ret;

	dev = vb2_get_drv_priv(vq);
	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	dev->f_count = 0;

	ret = ffe_start_generating(dev);
	if (ret) {
		struct ffe_buffer *buf, *tmp;

		list_for_each_entry_safe(buf, tmp, &dev->vidq.active, list) {
			list_del(&buf->list);
			vb2_buffer_done(&buf->vb, VB2_BUF_STATE_QUEUED);
		}
	}
	return ret;
}

static void stop_streaming(struct vb2_queue *vq)
{
	struct dev_data *dev;

	dev = vb2_get_drv_priv(vq);
	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	ffe_stop_generating(dev);
}

static void ffe_lock(struct vb2_queue *vq)
{
	struct dev_data *dev = vb2_get_drv_priv(vq);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	mutex_lock(&dev->mutex);
}

static void ffe_unlock(struct vb2_queue *vq)
{
	struct dev_data *dev = vb2_get_drv_priv(vq);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
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

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	strcpy(cap->driver, KBUILD_MODNAME);
	strcpy(cap->card, KBUILD_MODNAME);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", dev->v4l2_dev.name);
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	const struct ffe_fmt *fmt;
	struct dev_data *dev = video_drvdata(file);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (f->index >= ARRAY_SIZE(formats))
		return -EINVAL;

	fmt = &formats[f->index];

	strlcpy(f->description, fmt->name, sizeof(f->description));
	f->pixelformat = fmt->fourcc;
	return 0;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct dev_data *dev = video_drvdata(file);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	f->fmt.pix.width = dev->width;
	f->fmt.pix.height = dev->height;
	f->fmt.pix.field = V4L2_FIELD_INTERLACED;
	f->fmt.pix.pixelformat = dev->fmt->fourcc;
	f->fmt.pix.bytesperline = (f->fmt.pix.width * dev->fmt->depth) >> 3;
	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;
	if (dev->fmt->is_yuv)
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
	else
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct dev_data *dev = video_drvdata(file);
	const struct ffe_fmt *fmt;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	fmt = get_format(f->fmt.pix.pixelformat);
	if (!fmt) {
		v4l2_err(&dev->v4l2_dev, "%s: Unknown format..\n", __func__);
		f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
		fmt = get_format(f->fmt.pix.pixelformat);
	}

	f->fmt.pix.field = V4L2_FIELD_INTERLACED;
	f->fmt.pix.bytesperline = (f->fmt.pix.width * fmt->depth) >> 3;
	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;
	if (fmt->is_yuv)
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
	else
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct dev_data *dev = video_drvdata(file);
	struct vb2_queue *q = &dev->queue;
	int ret;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	ret = vidioc_try_fmt_vid_cap(file, priv, f);
	if (ret < 0)
		return ret;

	if (vb2_is_busy(q)) {
		v4l2_err(&dev->v4l2_dev, "%s device busy..\n", __func__);
		return -EBUSY;
	}

	dev->fmt = get_format(f->fmt.pix.pixelformat);
	dev->pixelsize = dev->fmt->depth / 8;
	dev->width = f->fmt.pix.width;
	dev->height = f->fmt.pix.height;
	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fsize)
{
	struct dev_data *dev = video_drvdata(file);
	static const struct v4l2_frmsize_stepwise sizes = {
		48, MAX_WIDTH, 4, 32, MAX_HEIGHT, 1
	};
	int i;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (fsize->index)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(formats); i++)
		if (formats[i].fourcc == fsize->pixel_format)
			break;
	if (i == ARRAY_SIZE(formats))
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = sizes;
	return 0;
}

static int vidioc_enum_input(struct file *file, void *priv, struct v4l2_input *inp)
{
	struct dev_data *dev = video_drvdata(file);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (inp->index >= 1)
		return -EINVAL;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	sprintf(inp->name, "Camera %u", inp->index);
	return 0;
}

static int vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct dev_data *dev = video_drvdata(file);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	*i = dev->input;
	return 0;
}

static int vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	struct dev_data *dev = video_drvdata(file);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (i >= 1)
		return -EINVAL;

	if (i == dev->input)
		return 0;

	dev->input = i;
	generate_colorbar(dev);
	return 0;
}

static int vidioc_enum_frameintervals(struct file *file, void *priv, struct v4l2_frmivalenum *fival)
{
	const struct ffe_fmt *fmt;
	struct dev_data *dev = video_drvdata(file);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (fival->index)
		return -EINVAL;

	fmt = get_format(fival->pixel_format);
	if (!fmt)
		return -EINVAL;

	if (fival->width < 48 || fival->width > MAX_WIDTH || (fival->width & 3))
		return -EINVAL;

	if (fival->height < 32 || fival->height > MAX_HEIGHT)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_CONTINUOUS;
	fival->stepwise.min = tpf_min;
	fival->stepwise.max = tpf_max;
	fival->stepwise.step = (struct v4l2_fract) {1, 1};
	return 0;
}

static int vidioc_g_parm(struct file *file, void *priv, struct v4l2_streamparm *parm)
{
	struct dev_data *dev = video_drvdata(file);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	parm->parm.capture.capability   = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.timeperframe = dev->time_per_frame;
	parm->parm.capture.readbuffers  = 1;
	return 0;
}

static int vidioc_s_parm(struct file *file, void *priv, struct v4l2_streamparm *parm)
{
	struct v4l2_fract tpf;
	struct dev_data *dev = video_drvdata(file);

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);
	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	tpf = parm->parm.capture.timeperframe;
	tpf = tpf.denominator ? tpf : tpf_default;
	tpf = (u64)tpf.numerator * tpf_min.denominator < (u64)tpf_min.numerator * tpf.denominator ? tpf_min : tpf;
	tpf = (u64)tpf.numerator * tpf_max.denominator > (u64)tpf_max.numerator * tpf.denominator ? tpf_max : tpf;

	dev->time_per_frame = tpf;
	parm->parm.capture.timeperframe = tpf;
	parm->parm.capture.readbuffers = 1;
	return 0;
}

static const struct v4l2_file_operations ffe_fops = {
	.owner				= THIS_MODULE,
	.open				= v4l2_fh_open,
	.release			= vb2_fop_release,
	.read				= vb2_fop_read,
	.poll				= vb2_fop_poll,
	.unlocked_ioctl			= video_ioctl2,
	.mmap				= vb2_fop_mmap,
};

static const struct v4l2_ioctl_ops ffe_ioctl_ops = {
	.vidioc_querycap		= vidioc_querycap,
	.vidioc_enum_fmt_vid_cap	= vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= vidioc_s_fmt_vid_cap,
	.vidioc_enum_framesizes		= vidioc_enum_framesizes,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_enum_input		= vidioc_enum_input,
	.vidioc_g_input			= vidioc_g_input,
	.vidioc_s_input			= vidioc_s_input,
	.vidioc_enum_frameintervals	= vidioc_enum_frameintervals,
	.vidioc_g_parm			= vidioc_g_parm,
	.vidioc_s_parm			= vidioc_s_parm,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
	.vidioc_log_status		= v4l2_ctrl_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
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

	dev->fmt = &formats[0];
	dev->time_per_frame = tpf_default;
	dev->width = 640;
	dev->height = 360;
	dev->pixelsize = dev->fmt->depth/2;

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
	INIT_LIST_HEAD(&dev->vidq.active);
	init_waitqueue_head(&dev->vidq.wq);

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
	v4l2_info(&dev->v4l2_dev, "%s: unregistering %s\n", __func__, video_device_node_name(&dev->vdev));
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

	ret = platform_driver_register(&p_driver);
	if (ret) {
		pr_err("%s: platform driver, %s registration failed..\n", __func__, p_driver.driver.name);
		platform_device_unregister(&p_device);
	}
	pr_info("FFE-V4L2-Driver version %s loaded successfully..\n", VERSION);
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
