/*
 * Dummy V4L2 driver - This code emulates a real video device with v4l2 api
 * It takes a frame and inverts it by 180 degrees.
 *
 * This code is based on the VIVI driver present in the kernel source
 * Parts of it have been taken as it is from the VIVI driver
 * Some parts have been modified according to the use case.
 */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/font.h>
#include <linux/mutex.h>
#include <linux/videodev2.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <media/videobuf2-vmalloc.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/v4l2-common.h>

#define DD_MODULE_NAME "dd"

/* Wake up at about 30 fps */
#define WAKE_NUMERATOR 30
#define WAKE_DENOMINATOR 1001
#define BUFFER_TIMEOUT     msecs_to_jiffies(500)  /* 0.5 seconds */

// Support Quad HD
#define MAX_WIDTH 2560
#define MAX_HEIGHT 1440

#define DD_VERSION "1.0.0"

MODULE_DESCRIPTION("A Dummy V4l2 driver which inverts frames by 180 degrees");
MODULE_AUTHOR("Shivanshu Agrawal");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DD_VERSION);

static unsigned debug=1;

// Video memory limit - 64MB
static unsigned int vid_limit = 64;


#define dprintk(dev, level, fmt, arg...) \
	v4l2_dbg(level, debug, &dev->v4l2_dev, fmt, ## arg)

/* ------------------------------------------------------------------
	Basic structures
   ------------------------------------------------------------------*/

// Video formats supported
struct dd_fmt {
	char  *name;
	u32   fourcc;          /* v4l2 format id */
	int   depth;
};

// Currently, only RGB32 (4 byte packed RGB format) is supported
static struct dd_fmt formats[] = {
  {
  	.name     = "RGB32 (LE)",
  	.fourcc   = V4L2_PIX_FMT_RGB32,
  	.depth    = 32,
  },
};


static struct dd_fmt *get_format(struct v4l2_format *f)
{
	struct dd_fmt *fmt;
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(formats); k++) {
		fmt = &formats[k];
		if (fmt->fourcc == f->fmt.pix.pixelformat)
			break;
	}

	if (k == ARRAY_SIZE(formats))
		return NULL;

	return &formats[k];
}

/* buffer for one video frame */
struct dd_buffer {
	/* common v4l buffer stuff -- must be first */
	struct vb2_buffer	vb;
	struct list_head	list;
	struct dd_fmt        *fmt;
};

struct dd_dmaqueue {
	struct list_head       active;

	/* thread for generating video stream*/
	struct task_struct         *kthread;
	wait_queue_head_t          wq;
};


struct dd_dev {
	struct v4l2_device 	   v4l2_dev;
	spinlock_t                 slock;
	struct mutex		   mutex;

	/* various device info */
	struct video_device        *vfd;

	struct dd_dmaqueue       vidq;

	/* Several counters */
	unsigned long              jiffies;

	/* video capture */
	struct dd_fmt            *fmt;
	unsigned int               width, height;
	struct vb2_queue	   vb_vidq;
	enum v4l2_field		   field;
	unsigned int		   field_count;

  u8         tmpbuf[MAX_WIDTH * 4];
};

struct dd_dev *dev;

/* ------------------------------------------------------------------
	DMA and thread functions
   ------------------------------------------------------------------*/

static void invert_line(char *basep, char* origp, int width) {
    int w;
    char *lastpix = origp + (width-1)*4; /* 32 bpp */
    for(w=0; w<width; w++) {
        basep[0] = lastpix[0];
        basep[1] = lastpix[1];
        basep[2] = lastpix[2];
        basep[3] = lastpix[3];
        lastpix = lastpix - 4;
        basep = basep + 4;
    }
}

static void dd_fillbuff(struct dd_dev *dev, struct dd_buffer *buf)
{
	int wmax = dev->width;
	int hmax = dev->height;
	struct timeval ts;
  int pos = 0;
	void *vbuf = vb2_plane_vaddr(&buf->vb, 0);
	int h;

	if (!vbuf)
		return;

	for (h = 0; h < hmax; h++) {
		invert_line(dev->tmpbuf, vbuf + pos, wmax);
    memcpy(vbuf + pos, dev->tmpbuf, wmax*4);
    pos += wmax*4;
  }


	/* Updates stream time */

	dev->jiffies = jiffies;

	buf->vb.v4l2_buf.field = dev->field;
	dev->field_count++;
	buf->vb.v4l2_buf.sequence = dev->field_count;
	do_gettimeofday(&ts);
	buf->vb.v4l2_buf.timestamp = ts;
}

static void dd_thread_tick(struct dd_dev *dev)
{
	struct dd_dmaqueue *dma_q = &dev->vidq;
	struct dd_buffer *buf;
	unsigned long flags = 0;

	dprintk(dev, 1, "Thread tick\n");

	spin_lock_irqsave(&dev->slock, flags);
	if (list_empty(&dma_q->active)) {
		dprintk(dev, 1, "No active queue to serve\n");
		spin_unlock_irqrestore(&dev->slock, flags);
		return;
	}

	buf = list_entry(dma_q->active.next, struct dd_buffer, list);
	list_del(&buf->list);
	spin_unlock_irqrestore(&dev->slock, flags);

	do_gettimeofday(&buf->vb.v4l2_buf.timestamp);

	/* Fill buffer */
	dd_fillbuff(dev, buf);
	dprintk(dev, 1, "filled buffer %p\n", buf);

	vb2_buffer_done(&buf->vb, VB2_BUF_STATE_DONE);
	dprintk(dev, 2, "[%p/%d] done\n", buf, buf->vb.v4l2_buf.index);
}

#define frames_to_ms(frames)					\
	((frames * WAKE_NUMERATOR * 1000) / WAKE_DENOMINATOR)

static void dd_sleep(struct dd_dev *dev)
{
	struct dd_dmaqueue *dma_q = &dev->vidq;
	int timeout;
	DECLARE_WAITQUEUE(wait, current);

	dprintk(dev, 1, "%s dma_q=0x%08lx\n", __func__,
		(unsigned long)dma_q);

	add_wait_queue(&dma_q->wq, &wait);
	if (kthread_should_stop())
		goto stop_task;

	/* Calculate time to wake up */
	timeout = msecs_to_jiffies(frames_to_ms(1));

	dd_thread_tick(dev);

	schedule_timeout_interruptible(timeout);

stop_task:
	remove_wait_queue(&dma_q->wq, &wait);
	try_to_freeze();
}

static int dd_thread(void *data)
{
	struct dd_dev *dev = data;

	dprintk(dev, 1, "thread started\n");

	set_freezable();

	for (;;) {
		dd_sleep(dev);

		if (kthread_should_stop())
			break;
	}
	dprintk(dev, 1, "thread: exit\n");
	return 0;
}

static int dd_start_generating(struct dd_dev *dev)
{
	struct dd_dmaqueue *dma_q = &dev->vidq;

	dprintk(dev, 1, "%s\n", __func__);

	/* Resets frame counters */
	dev->jiffies = jiffies;

	dma_q->kthread = kthread_run(dd_thread, dev, dev->v4l2_dev.name);

	if (IS_ERR(dma_q->kthread)) {
		v4l2_err(&dev->v4l2_dev, "kernel_thread() failed\n");
		return PTR_ERR(dma_q->kthread);
	}
	/* Wakes thread */
	wake_up_interruptible(&dma_q->wq);

	dprintk(dev, 1, "returning from %s\n", __func__);
	return 0;
}

static void dd_stop_generating(struct dd_dev *dev)
{
	struct dd_dmaqueue *dma_q = &dev->vidq;

	dprintk(dev, 1, "%s\n", __func__);

	/* shutdown control thread */
	if (dma_q->kthread) {
		kthread_stop(dma_q->kthread);
		dma_q->kthread = NULL;
	}

	/*
	 * Typical driver might need to wait here until dma engine stops.
	 * In this case we can abort imiedetly, so it's just a noop.
	 */

	/* Release all active buffers */
	while (!list_empty(&dma_q->active)) {
		struct dd_buffer *buf;
		buf = list_entry(dma_q->active.next, struct dd_buffer, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
		dprintk(dev, 2, "[%p/%d] done\n", buf, buf->vb.v4l2_buf.index);
	}
}
/* ------------------------------------------------------------------
	Videobuf operations
   ------------------------------------------------------------------*/
static int queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
				unsigned int *nbuffers, unsigned int *nplanes,
				unsigned int sizes[], void *alloc_ctxs[])
{
	struct dd_dev *dev = vb2_get_drv_priv(vq);
	unsigned long size;

	size = dev->width * dev->height * 4;

	if (0 == *nbuffers)
		*nbuffers = 32;

	while (size * *nbuffers > vid_limit * 1024 * 1024)
		(*nbuffers)--;

	*nplanes = 1;

	sizes[0] = size;

	/*
	 * videobuf2-vmalloc allocator is context-less so no need to set
	 * alloc_ctxs array.
	 */

	dprintk(dev, 1, "%s, count=%d, size=%ld\n", __func__,
		*nbuffers, size);

	return 0;
}

static int buffer_init(struct vb2_buffer *vb)
{
	struct dd_dev *dev = vb2_get_drv_priv(vb->vb2_queue);

	BUG_ON(NULL == dev->fmt);

	/*
	 * This callback is called once per buffer, after its allocation.
	 *
	 * Vivi does not allow changing format during streaming, but it is
	 * possible to do so when streaming is paused (i.e. in streamoff state).
	 * Buffers however are not freed when going into streamoff and so
	 * buffer size verification has to be done in buffer_prepare, on each
	 * qbuf.
	 * It would be best to move verification code here to buf_init and
	 * s_fmt though.
	 */

	return 0;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct dd_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct dd_buffer *buf = container_of(vb, struct dd_buffer, vb);
	unsigned long size;

	dprintk(dev, 1, "%s, field=%d\n", __func__, vb->v4l2_buf.field);

	BUG_ON(NULL == dev->fmt);

	/*
	 * Theses properties only change when queue is idle, see s_fmt.
	 * The below checks should not be performed here, on each
	 * buffer_prepare (i.e. on each qbuf). Most of the code in this function
	 * should thus be moved to buffer_init and s_fmt.
	 */
	if (dev->width  < 48 || dev->width  > MAX_WIDTH ||
	    dev->height < 32 || dev->height > MAX_HEIGHT)
		return -EINVAL;

  // TODO: changed the size here. should be width*height*4
	size = dev->width * dev->height * 4;
	if (vb2_plane_size(vb, 0) < size) {
		dprintk(dev, 1, "%s data will not fit into plane (%lu < %lu)\n",
				__func__, vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(&buf->vb, 0, size);

	buf->fmt = dev->fmt;

	return 0;
}

void buffer_finish(struct vb2_buffer *vb)
{
	struct dd_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	dprintk(dev, 1, "%s\n", __func__);
	//return 0;
}

static void buffer_cleanup(struct vb2_buffer *vb)
{
	struct dd_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	dprintk(dev, 1, "%s\n", __func__);

}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct dd_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct dd_buffer *buf = container_of(vb, struct dd_buffer, vb);
	struct dd_dmaqueue *vidq = &dev->vidq;
	unsigned long flags = 0;

	dprintk(dev, 1, "%s\n", __func__);

	spin_lock_irqsave(&dev->slock, flags);
	list_add_tail(&buf->list, &vidq->active);
	spin_unlock_irqrestore(&dev->slock, flags);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct dd_dev *dev = vb2_get_drv_priv(vq);
	dprintk(dev, 1, "%s\n", __func__);
	return dd_start_generating(dev);
}

/* abort streaming and wait for last buffer */
void stop_streaming(struct vb2_queue *vq)
{
	struct dd_dev *dev = vb2_get_drv_priv(vq);
	dprintk(dev, 1, "%s\n", __func__);
	dd_stop_generating(dev);
	//return 0;
}

static void dd_lock(struct vb2_queue *vq)
{
	struct dd_dev *dev = vb2_get_drv_priv(vq);
	mutex_lock(&dev->mutex);
}

static void dd_unlock(struct vb2_queue *vq)
{
	struct dd_dev *dev = vb2_get_drv_priv(vq);
	mutex_unlock(&dev->mutex);
}


static struct vb2_ops dd_video_qops = {
	.queue_setup		= queue_setup,
	.buf_init		= buffer_init,
	.buf_prepare		= buffer_prepare,
	.buf_finish		= buffer_finish,
	.buf_cleanup		= buffer_cleanup,
	.buf_queue		= buffer_queue,
	.start_streaming	= start_streaming,
	.stop_streaming		= stop_streaming,
	.wait_prepare		= dd_unlock,
	.wait_finish		= dd_lock,
};

/* ------------------------------------------------------------------
	IOCTL vidioc handling
   ------------------------------------------------------------------*/
static int vidioc_querycap(struct file *file, void  *priv,
					struct v4l2_capability *cap)
{
	struct dd_dev *dev = video_drvdata(file);

	strcpy(cap->driver, "dd");
	strcpy(cap->card, "dd");
	strlcpy(cap->bus_info, dev->v4l2_dev.name, sizeof(cap->bus_info));
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | \
			    V4L2_CAP_READWRITE;
	return 0;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct dd_dev *dev = video_drvdata(file);

	f->fmt.pix.width        = dev->width;
	f->fmt.pix.height       = dev->height;
	f->fmt.pix.field        = dev->field;
	f->fmt.pix.pixelformat  = dev->fmt->fourcc;
	f->fmt.pix.bytesperline =
		(f->fmt.pix.width * dev->fmt->depth) >> 3;
	f->fmt.pix.sizeimage =
		f->fmt.pix.height * f->fmt.pix.bytesperline;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
			struct v4l2_format *f)
{
	struct dd_dev *dev = video_drvdata(file);
	struct dd_fmt *fmt;
	enum v4l2_field field;

	fmt = get_format(f);
	if (!fmt) {
		dprintk(dev, 1, "Fourcc format (0x%08x) invalid.\n",
			f->fmt.pix.pixelformat);
		return -EINVAL;
	}

	field = f->fmt.pix.field;

	if (field == V4L2_FIELD_ANY) {
		field = V4L2_FIELD_NONE;
	} else if (V4L2_FIELD_NONE != field) {
		dprintk(dev, 1, "Field type invalid.\n");
		return -EINVAL;
	}

	f->fmt.pix.field = field;
	v4l_bound_align_image(&f->fmt.pix.width, 48, MAX_WIDTH, 2,
			      &f->fmt.pix.height, 32, MAX_HEIGHT, 0, 0);
	f->fmt.pix.bytesperline =
		(f->fmt.pix.width * fmt->depth) >> 3;
	f->fmt.pix.sizeimage =
		f->fmt.pix.height * f->fmt.pix.bytesperline;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct dd_dev *dev = video_drvdata(file);
	struct vb2_queue *q = &dev->vb_vidq;

	int ret = vidioc_try_fmt_vid_cap(file, priv, f);
	if (ret < 0)
		return ret;

	if (vb2_is_streaming(q)) {
		dprintk(dev, 1, "%s device busy\n", __func__);
		return -EBUSY;
	}

	dev->fmt = get_format(f);
	dev->width = f->fmt.pix.width;
	dev->height = f->fmt.pix.height;
	dev->field = f->fmt.pix.field;

	return 0;
}

static int vidioc_reqbufs(struct file *file, void *priv,
			  struct v4l2_requestbuffers *p)
{
	struct dd_dev *dev = video_drvdata(file);
	return vb2_reqbufs(&dev->vb_vidq, p);
}

static int vidioc_querybuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct dd_dev *dev = video_drvdata(file);
	return vb2_querybuf(&dev->vb_vidq, p);
}

static int vidioc_qbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct dd_dev *dev = video_drvdata(file);
	return vb2_qbuf(&dev->vb_vidq, p);
}

static int vidioc_dqbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct dd_dev *dev = video_drvdata(file);
	return vb2_dqbuf(&dev->vb_vidq, p, file->f_flags & O_NONBLOCK);
}

static int vidioc_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
	struct dd_dev *dev = video_drvdata(file);
	return vb2_streamon(&dev->vb_vidq, i);
}

static int vidioc_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
	struct dd_dev *dev = video_drvdata(file);
	return vb2_streamoff(&dev->vb_vidq, i);
}


/* ------------------------------------------------------------------
	File operations for the device
   ------------------------------------------------------------------*/

static ssize_t
dd_read(struct file *file, char __user *data, size_t count, loff_t *ppos)
{
	struct dd_dev *dev = video_drvdata(file);

	dprintk(dev, 1, "read called\n");
	return vb2_read(&dev->vb_vidq, data, count, ppos,
		       file->f_flags & O_NONBLOCK);
}

static unsigned int
dd_poll(struct file *file, struct poll_table_struct *wait)
{
	struct dd_dev *dev = video_drvdata(file);
	struct v4l2_fh *fh = file->private_data;
	struct vb2_queue *q = &dev->vb_vidq;
	unsigned int res;

	dprintk(dev, 1, "%s\n", __func__);
	res = vb2_poll(q, file, wait);
	if (v4l2_event_pending(fh))
		res |= POLLPRI;
	else
		poll_wait(file, &fh->wait, wait);
	return res;
}

static int dd_close(struct file *file)
{
	struct video_device  *vdev = video_devdata(file);
	struct dd_dev *dev = video_drvdata(file);

	dprintk(dev, 1, "close called (dev=%s), file %p\n",
		video_device_node_name(vdev), file);

	if (v4l2_fh_is_singular_file(file))
		vb2_queue_release(&dev->vb_vidq);
	return v4l2_fh_release(file);
}

static int dd_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct dd_dev *dev = video_drvdata(file);
	int ret;

	dprintk(dev, 1, "mmap called, vma=0x%08lx\n", (unsigned long)vma);

	ret = vb2_mmap(&dev->vb_vidq, vma);
	dprintk(dev, 1, "vma start=0x%08lx, size=%ld, ret=%d\n",
		(unsigned long)vma->vm_start,
		(unsigned long)vma->vm_end - (unsigned long)vma->vm_start,
		ret);
	return ret;
}


#define DD_CID_CUSTOM_BASE	(V4L2_CID_USER_BASE | 0xf000)


static const struct v4l2_file_operations dd_fops = {
	.owner		= THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = dd_close,
	.read           = dd_read,
	.poll		= dd_poll,
	.unlocked_ioctl = video_ioctl2, /* V4L2 ioctl handler */
	.mmap           = dd_mmap,
};

static const struct v4l2_ioctl_ops dd_ioctl_ops = {
	.vidioc_querycap      = vidioc_querycap,
	.vidioc_g_fmt_vid_cap     = vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap   = vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap     = vidioc_s_fmt_vid_cap,
	.vidioc_reqbufs       = vidioc_reqbufs,
	.vidioc_querybuf      = vidioc_querybuf,
	.vidioc_qbuf          = vidioc_qbuf,
	.vidioc_dqbuf         = vidioc_dqbuf,
	.vidioc_streamon      = vidioc_streamon,
	.vidioc_streamoff     = vidioc_streamoff,
};

static struct video_device dd_template = {
	.name		= "dd",
	.fops           = &dd_fops,
	.ioctl_ops 	= &dd_ioctl_ops,
	.release	= video_device_release,

	.tvnorms              = V4L2_STD_525_60,
};

/* -----------------------------------------------------------------
	Initialization and module stuff
   ------------------------------------------------------------------*/

static int dd_release(void)
{
	v4l2_info(&dev->v4l2_dev, "unregistering %s\n",
		video_device_node_name(dev->vfd));
	video_unregister_device(dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);
	kfree(dev);
	return 0;
}

static int __init dd_create_instance(void)
{

	struct video_device *vfd;
	struct vb2_queue *q;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	snprintf(dev->v4l2_dev.name, sizeof(dev->v4l2_dev.name),
			"%s", DD_MODULE_NAME);
	ret = v4l2_device_register(NULL, &dev->v4l2_dev);
	if (ret)
		goto free_dev;
	dev->fmt = &formats[0];
	dev->width = 640;
	dev->height = 480;

	/* initialize locks */
	spin_lock_init(&dev->slock);

	/* initialize queue */
	q = &dev->vb_vidq;
	memset(q, 0, sizeof(dev->vb_vidq));
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct dd_buffer);
	q->ops = &dd_video_qops;
	q->mem_ops = &vb2_vmalloc_memops;

	vb2_queue_init(q);

	mutex_init(&dev->mutex);

	/* init video dma queues */
	INIT_LIST_HEAD(&dev->vidq.active);
	init_waitqueue_head(&dev->vidq.wq);

	ret = -ENOMEM;
	vfd = video_device_alloc();
	if (!vfd)
		goto unreg_dev;

	*vfd = dd_template;
	vfd->v4l2_dev = &dev->v4l2_dev;

	/*
	 * Provide a mutex to v4l2 core. It will be used to protect
	 * all fops and v4l2 ioctls.
	 */
	vfd->lock = &dev->mutex;

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, -1);
	if (ret < 0)
		goto rel_vdev;

	video_set_drvdata(vfd, dev);


	dev->vfd = vfd;
	v4l2_info(&dev->v4l2_dev, "V4L2 device registered as %s\n",
		  video_device_node_name(vfd));
	return 0;

rel_vdev:
	video_device_release(vfd);
unreg_dev:
	v4l2_device_unregister(&dev->v4l2_dev);
free_dev:
	kfree(dev);
	return ret;
}

static int __init dd_init(void)
{
	int ret = 0;

	ret = dd_create_instance();

	if (ret < 0) {
		printk(KERN_ERR "dd: error %d while loading driver\n", ret);
		return ret;
	}

	printk(KERN_INFO "Dummy V4L2 driver ver %s successfully loaded.\n",	DD_VERSION);

	return ret;
}

static void __exit dd_exit(void)
{
	dd_release();
}

module_init(dd_init);
module_exit(dd_exit);
