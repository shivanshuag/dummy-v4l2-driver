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
#include <stdint.h>
#include <pthread.h>

#include "modeset.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
  void   *start;
  size_t  length;
};

#define LR 0
#define RL 1
#define TB 2
#define BT 3


static char            *dev_name;
struct buffer          *buffers;
struct buffer          *original_buffer;
static unsigned int     n_buffers;
static int              fd = -1;
static int              frame_count = 70;
static int height, width;
int display_mode = 3;
pthread_t thr_display;

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

static void precalculate_buffer() {
  uint32_t *line;
	int w, h;
  uint8_t r, g, b;
  line = malloc(width*4);
  // Supports just RGB32 format right now
	for (w = 0; w < width; w++) {
    // the color of the bar displayed
		int colorpos = (w/(width/8))%8;
    r = bars[bar_num].bar[colorpos][0];
    g = bars[bar_num].bar[colorpos][1];
    b = bars[bar_num].bar[colorpos][2];
    //printf("r is %x\n", (unsigned char)r);
    line[w] = (r << 16) | (g << 8) | b;
	}
  for (h=0;h<height;h++) {
    memcpy(original_buffer->start + h*width*4, line, width*4);
  }
}

static void process_image(const void *p, int size)
{
  modeset_draw((uint32_t *)original_buffer->start, (uint32_t *)p, display_mode);
  // fflush(stderr);
  // fprintf(stderr, ".");
  // fflush(stdout);
}

static void fill_buffer(uint32_t* buf) {
  memcpy(buf, original_buffer->start, height*width*4);
  //printf("\n\n\nFilled buffer\n");
  //process_image((void *)buf, width*height*4);
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
  //printf("\n\n\nReceived buffer\n");
  process_image((void *)buf.m.userptr, buf.bytesused);

  // fill the buffer
  fill_buffer((uint32_t *)buffers[i].start);
  // queue the buf again
  if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
    errno_exit("VIDIOC_QBUF");

  return 1;
}

void *display(void *arg) {
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
  pthread_exit(NULL);
}

static void take_input() {
  // take input
  // print ascii art
  printf("     .--.\n \
   |o_o |\n \
   |:_/ |\n \
  //   \\ \\\n \
 (|     | )\n \
/'\\_   _/`\\\n \
\\___)=(___/\n \
");
  printf("V4l2 Dummy Driver Application.\n");
  printf("---Display Mode---\n");
  printf("0 - ‘LR’  i.e. show input pattern on the left half, output pattern on the right half\n");
  printf("1 - ‘RL’  i.e. show input pattern on the right half, output pattern on the left half\n");
  printf("2 - ‘TB’  i.e. show input pattern on the top half, output pattern on the bottom half\n");
  printf("3 - ‘BT’  i.e. show input pattern on the bottom half, output pattern on the top half\n");
  printf("Please input the display mode: ");
  scanf("%d", &display_mode);
  if(display_mode < 0 || display_mode > 3) {
    fprintf(stderr,"Invalid display mode\n");
    exit(EXIT_FAILURE);
  }
  printf("Press Enter to start display. You can press enter during the display to stop the display\n");
  getchar();
}


static void stop_capturing(void)
{
  enum v4l2_buf_type type;
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
    errno_exit("VIDIOC_STREAMOFF");
}


static void start_capturing(void) {
  unsigned int i;
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

    fill_buffer((uint32_t *)buffers[i].start);
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

  req.count  = 2;
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

  buffers = calloc(2, sizeof(*buffers));

  if (!buffers) {
    fprintf(stderr, "Out of memory\n");
    exit(EXIT_FAILURE);
  }

  // allocate memory for the buffers
  for (n_buffers = 0; n_buffers < 2; ++n_buffers) {
    buffers[n_buffers].length = buffer_size;
    buffers[n_buffers].start = malloc(buffer_size);
    if (!buffers[n_buffers].start) {
      fprintf(stderr, "Out of memory\n");
      exit(EXIT_FAILURE);
    }
  }
}



static void init_device(void) {
  //printf("doing init");
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
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB32;
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;
  width = fmt.fmt.pix.width;
  height = fmt.fmt.pix.height;
  // set the image format of the driver
  if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
    errno_exit("VIDIOC_S_FMT");

  // set the size of a single frame.
  fmt.fmt.pix.sizeimage = fmt.fmt.pix.width*fmt.fmt.pix.height*4;

  //  initialize the user pointers for the buffers
  init_userp(fmt.fmt.pix.sizeimage);
  // calculate the color-bar pattern
  original_buffer = malloc(sizeof(struct buffer));
  original_buffer->start = malloc(fmt.fmt.pix.sizeimage);
  precalculate_buffer();

}

static void mainloop(void) {
  int drifd;
  for (;;) {
    take_input();

    //initilize the display
    drifd = init_modeset();
    if(modeset_list == NULL) {
      fprintf(stderr, "cannot find drm device");
      exit(EXIT_FAILURE);
    }
    height = modeset_list->height;
    width = modeset_list->width;
    // initialize the v4l2 device
    init_device();
    start_capturing();
    if (pthread_create(&thr_display, NULL, display, (void *)&thr_display)) {
      fprintf(stderr,"Can't create display thread\n");
      exit(EXIT_FAILURE);
    }
    // wait for enter to stop displaying
    getchar();
    pthread_cancel(thr_display);
    stop_capturing();
    uninit_device();
    modeset_cleanup(drifd);
  }
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
  mainloop();
  close_device();
  fprintf(stderr, "\n");
  return 0;
}
