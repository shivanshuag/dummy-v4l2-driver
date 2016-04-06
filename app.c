#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
  void   *start;
  size_t  length;
};

static char            *dev_name;
struct buffer          *buffers;
static unsigned int     n_buffers;
static int              fd = -1;
static int              frame_count = 70;
static int height, width;
static char* line;

static void errno_exit(const char *s) {
  fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
  exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg) {
  int r;
  do {
    r = ioctl(fh, request, arg);
  } while (-1 == r && EINTR == errno);
  return r;
}

/*-----------------------------------------------------------------------------
   Generating colorbars
------------------------------------------------------------------------------*/
enum colors {
	WHITE,
	AMBER,
	CYAN,
	GREEN,
	MAGENTA,
	RED,
	BLUE,
	BLACK,
};

/* R   G   B */
#define COLOR_WHITE	{204, 204, 204}
#define COLOR_AMBER	{208, 208,   0}
#define COLOR_CYAN	{  0, 206, 206}
#define	COLOR_GREEN	{  0, 239,   0}
#define COLOR_MAGENTA	{239,   0, 239}
#define COLOR_RED	{205,   0,   0}
#define COLOR_BLUE	{  0,   0, 255}
#define COLOR_BLACK	{  0,   0,   0}


struct bar_std {
	char bar[9][3];
};

/* Maximum number of bars are 10 - otherwise, the input print code
   should be modified */
static struct bar_std bars[] = {
	{	/* Standard ITU-R color bar sequence */
		{ COLOR_WHITE, COLOR_AMBER, COLOR_CYAN, COLOR_GREEN,
		  COLOR_MAGENTA, COLOR_RED, COLOR_BLUE, COLOR_BLACK, COLOR_BLACK }
	}, {
		{ COLOR_WHITE, COLOR_AMBER, COLOR_BLACK, COLOR_WHITE,
		  COLOR_AMBER, COLOR_BLACK, COLOR_WHITE, COLOR_AMBER, COLOR_BLACK }
	}, {
		{ COLOR_WHITE, COLOR_CYAN, COLOR_BLACK, COLOR_WHITE,
		  COLOR_CYAN, COLOR_BLACK, COLOR_WHITE, COLOR_CYAN, COLOR_BLACK }
	}, {
		{ COLOR_WHITE, COLOR_GREEN, COLOR_BLACK, COLOR_WHITE,
		  COLOR_GREEN, COLOR_BLACK, COLOR_WHITE, COLOR_GREEN, COLOR_BLACK }
	},
};

// TODO: displaying the 0th bar format now, but this should be taken from user
int bar_num = 0;

static void precalculate_line(char *buf)
{
	int w;
  char a, r, g, b;
  // Supports just RGB32 format right now
	for (w = 0; w < width * 4; w += 4) {
    // the color of the bar displayed
		int colorpos = (w/4)/(width/8);
    a = 0;
    r = bars[bar_num].bar[colorpos][0];
    g = bars[bar_num].bar[colorpos][1];
    b = bars[bar_num].bar[colorpos][2];
    //printf("r is %x\n", (unsigned char)r);
    buf[w] = a;
    buf[w+1] = r;
    buf[w+2] = g;
    buf[w+3] = b;
	}
}

static void process_image(const void *p, int size)
{
  //if (out_buf)
  int h,w;
  char *frame = (char*)p;
  for(h=0;h<height;h++) {
    for(w=0;w<width*4;w+=4) {
      printf("%x %x %x %x    ", (unsigned char)frame[0],(unsigned char)frame[1],(unsigned char)frame[2],(unsigned char)frame[3]);
      frame+=4;
    }
    printf("\n");
  }
  //fwrite(p, size, 1, stdout);

  fflush(stderr);
  fprintf(stderr, ".");
  fflush(stdout);
}

fill_buffer(char* buf) {
  int i;
  for(i=0;i< height;i++) {
    memcpy(buf + i*width*4, line, width*4);
  }
  printf("\n\n\nFilled buffer\n");
  process_image((void *)buf, width*height*4);
}

static void uninit_device(void)
{
  unsigned int i;
  for (i = 0; i < n_buffers; ++i)
    free(buffers[i].start);
  free(buffers);
}


static void close_device(void)
{
  if (-1 == close(fd))
    errno_exit("close");
  fd = -1;
}



static int read_frame(void)
{
  struct v4l2_buffer buf;
  unsigned int i;
  CLEAR(buf);

  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_USERPTR;

  // get a filled buffer
  if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
    switch (errno) {
    case EAGAIN:
      return 0;
    case EIO:
      /* Could ignore EIO, see spec. */

      /* fall through */
    default:
      errno_exit("VIDIOC_DQBUF");
    }
  }

  for (i = 0; i < n_buffers; ++i)
    if (buf.m.userptr == (unsigned long)buffers[i].start
        && buf.length == buffers[i].length)
        break;

  assert(i < n_buffers);

  // display the buffer here
  printf("\n\n\nReceived buffer\n");
  process_image((void *)buf.m.userptr, buf.bytesused);

  // fill the buffer
  fill_buffer(buffers[i].start);
  // queue the buf again
  if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
    errno_exit("VIDIOC_QBUF");

  return 1;
}


static void mainloop(void) {
  unsigned int count;
  count = frame_count;

  while (count-- > 0) {
    for (;;) {
      fd_set fds;
      struct timeval tv;
      int r;

      FD_ZERO(&fds);
      FD_SET(fd, &fds);

      /* Timeout. */
      tv.tv_sec = 2;
      tv.tv_usec = 0;

      r = select(fd + 1, &fds, NULL, NULL, &tv);

      if (-1 == r) {
        if (EINTR == errno)
          continue;
        errno_exit("select");
      }
      if (0 == r) {
        fprintf(stderr, "select timeout\n");
        exit(EXIT_FAILURE);
      }

      if (read_frame())
        break;
      /* EAGAIN - continue select loop. */
    }
  }
}




static void start_capturing(void) {
  unsigned int i, j;
  enum v4l2_buf_type type;

  // QBUF ioctl for all the buffers
  for (i = 0; i < n_buffers; ++i) {
    struct v4l2_buffer buf;

    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_USERPTR;
    buf.index = i;
    buf.m.userptr = (unsigned long)buffers[i].start;
    buf.length = buffers[i].length;

    fill_buffer(buffers[i].start);
    if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
      errno_exit("VIDIOC_QBUF");
  }

  // streamon ioctl to start streaming
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
    errno_exit("VIDIOC_STREAMON");
}


static void init_userp(unsigned int buffer_size) {
  struct v4l2_requestbuffers req;
  CLEAR(req);

  req.count  = 4;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_USERPTR;

  // REQBUF ioctl
  if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s does not support user pointer i/o\n", dev_name);
      exit(EXIT_FAILURE);
    }
    else {
      errno_exit("VIDIOC_REQBUFS");
    }
  }

  buffers = calloc(4, sizeof(*buffers));

  if (!buffers) {
    fprintf(stderr, "Out of memory\n");
    exit(EXIT_FAILURE);
  }

  // allocate memory for the buffers
  for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
    buffers[n_buffers].length = buffer_size;
    buffers[n_buffers].start = malloc(buffer_size);
    if (!buffers[n_buffers].start) {
      fprintf(stderr, "Out of memory\n");
      exit(EXIT_FAILURE);
    }
  }
}



static void init_device(void) {
  struct v4l2_capability cap;
  struct v4l2_format fmt;

  // query capabilities of the driver
  if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s is no V4L2 device\n", dev_name);
      exit(EXIT_FAILURE);
    }
    else {
      errno_exit("VIDIOC_QUERYCAP");
    }
  }
  // should have capture capabilities
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "%s is no video capture device\n", dev_name);
    exit(EXIT_FAILURE);
  }

  // should have streaming capabilities
  if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
    fprintf(stderr, "%s does not support streaming i/o\n", dev_name);
    exit(EXIT_FAILURE);
  }

  CLEAR(fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  // TODO: get this from modeset
  fmt.fmt.pix.width       = 48;
  fmt.fmt.pix.height      = 32;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB32;
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;
  width = fmt.fmt.pix.width;
  height = fmt.fmt.pix.height;
  // set the image format of the driver
  // TODO: first get the formats supported by the driver, then set
  if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
    errno_exit("VIDIOC_S_FMT");

  // set the size of a single frame.
  fmt.fmt.pix.sizeimage = fmt.fmt.pix.width*fmt.fmt.pix.height*4;

  //  initialize the user pointers for the buffers
  init_userp(fmt.fmt.pix.sizeimage);
  // calculate line
  line = malloc(width*4);
  precalculate_line(line);

}


static void open_device(void)
{
  struct stat st;

  if (-1 == stat(dev_name, &st)) {
    fprintf(stderr, "Cannot identify '%s': %d, %s\n",
             dev_name, errno, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (!S_ISCHR(st.st_mode)) {
    fprintf(stderr, "%s is no device\n", dev_name);
    exit(EXIT_FAILURE);
  }

  fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

  if (-1 == fd) {
    fprintf(stderr, "Cannot open '%s': %d, %s\n",
             dev_name, errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

int main(int argc, char **argv) {
  dev_name = "/dev/video0";
  open_device();
  init_device();
  start_capturing();
  mainloop();
  uninit_device();
  close_device();
  fprintf(stderr, "\n");
  return 0;
}
