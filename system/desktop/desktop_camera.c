/****************************************************************************
 * apps/system/desktop/desktop_camera.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * V4L2 camera preview for the ESP32-P4 desktop.  Capture and scaling run in
 * a worker thread; only the timer callback touches LVGL objects.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <nuttx/cache.h>
#include <nuttx/video/v4l2_cap.h>
#include <nuttx/video/video.h>
#include <lvgl/lvgl.h>

#include "desktop_camera.h"

#define CAMERA_DEVICE          "/dev/video0"
#define CAMERA_WIDTH           1280
#define CAMERA_HEIGHT          720
#define CAMERA_FRAME_BYTES     (CAMERA_WIDTH * CAMERA_HEIGHT * 2)
#define CAMERA_CAPTURE_BUFS    3
#define CAMERA_PREVIEW_WIDTH   640
#define CAMERA_PREVIEW_HEIGHT  360
#define CAMERA_PREVIEW_BYTES   (CAMERA_PREVIEW_WIDTH * \
                                CAMERA_PREVIEW_HEIGHT * 2)
#define CAMERA_THREAD_STACK    8192
#define CAMERA_POLL_MS         10
#define CAMERA_UI_PERIOD_MS    33
#define CAMERA_START_TIMEOUT_MS 4000

enum camera_state_e
{
  CAMERA_STATE_IDLE = 0,
  CAMERA_STATE_STARTING,
  CAMERA_STATE_RUNNING,
  CAMERA_STATE_FAILED
};

struct camera_preview_s
{
  pthread_mutex_t lock;
  pthread_t thread;
  bool joinable;
  bool stop;
  enum camera_state_e state;
  int result;
  int front;
  int ready;
  int writing;
  uint32_t sequence;
  uint32_t shown_sequence;
  FAR uint16_t *pixels[2];
  lv_image_dsc_t image_dsc[2];
  lv_obj_t *image;
  lv_obj_t *status;
  lv_timer_t *timer;
};

static struct camera_preview_s g_camera =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .state = CAMERA_STATE_IDLE,
  .front = -1,
  .ready = -1,
  .writing = -1,
};

static int camera_errno(void)
{
  return errno > 0 ? -errno : -EIO;
}

static bool camera_should_stop(void)
{
  bool stop;

  pthread_mutex_lock(&g_camera.lock);
  stop = g_camera.stop;
  pthread_mutex_unlock(&g_camera.lock);
  return stop;
}

static void camera_set_state(enum camera_state_e state, int result)
{
  pthread_mutex_lock(&g_camera.lock);
  g_camera.state = state;
  g_camera.result = result;
  pthread_mutex_unlock(&g_camera.lock);
}

static void camera_scale_frame(FAR const uint16_t *source)
{
  FAR uint16_t *dest;
  int target;
  int x;
  int y;

  pthread_mutex_lock(&g_camera.lock);
  target = g_camera.front == 0 ? 1 : 0;
  g_camera.ready = -1;
  g_camera.writing = target;
  dest = g_camera.pixels[target];
  pthread_mutex_unlock(&g_camera.lock);

  /* 2x integer downsample: no per-pixel division, sequential writes. */

  for (y = 0; y < CAMERA_PREVIEW_HEIGHT; y++)
    {
      FAR const uint16_t *srcrow = source + (y * 2) * CAMERA_WIDTH;
      FAR uint16_t *dstrow = dest + y * CAMERA_PREVIEW_WIDTH;

      for (x = 0; x < CAMERA_PREVIEW_WIDTH; x++)
        {
          dstrow[x] = srcrow[x * 2];
        }
    }

  pthread_mutex_lock(&g_camera.lock);
  g_camera.writing = -1;
  if (!g_camera.stop)
    {
      g_camera.ready = target;
      g_camera.sequence++;
    }
  pthread_mutex_unlock(&g_camera.lock);
}

static FAR void *camera_worker(FAR void *arg)
{
  FAR uint8_t *buffers[CAMERA_CAPTURE_BUFS] = { NULL };
  size_t lengths[CAMERA_CAPTURE_BUFS] = { 0 };
  struct v4l2_capability capability;
  struct v4l2_requestbuffers request;
  struct v4l2_streamparm parameter;
  struct v4l2_format format;
  struct v4l2_buffer buffer;
  struct pollfd pollfd;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  bool streaming = false;
  bool got_frame = false;
  unsigned int count = 0;
  int waited_ms = 0;
  int result = 0;
  int fd = -1;
  int i;

  (void)arg;
  fd = open(CAMERA_DEVICE, O_RDWR);
  if (fd < 0)
    {
      result = camera_errno();
      printf("desktop: camera open failed: %d\n", result);
      goto out;
    }

  memset(&capability, 0, sizeof(capability));
  if (ioctl(fd, VIDIOC_QUERYCAP, (uintptr_t)&capability) < 0)
    {
      result = camera_errno();
      printf("desktop: camera QUERYCAP failed: %d\n", result);
      goto out;
    }

  memset(&format, 0, sizeof(format));
  format.type = type;
  format.fmt.pix.width = CAMERA_WIDTH;
  format.fmt.pix.height = CAMERA_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  if (ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      result = camera_errno();
      printf("desktop: camera S_FMT failed: %d\n", result);
      goto out;
    }

  memset(&parameter, 0, sizeof(parameter));
  parameter.type = type;
  parameter.parm.capture.timeperframe.numerator = 1;
  parameter.parm.capture.timeperframe.denominator = 30;
  if (ioctl(fd, VIDIOC_S_PARM, (uintptr_t)&parameter) < 0)
    {
      printf("desktop: camera VIDIOC_S_PARM ignored: %d\n", camera_errno());
    }

  memset(&request, 0, sizeof(request));
  request.type = type;
  request.memory = V4L2_MEMORY_MMAP;
  request.count = CAMERA_CAPTURE_BUFS;
  request.mode = V4L2_BUF_MODE_RING;
  if (ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      result = camera_errno();
      printf("desktop: camera REQBUFS failed: %d\n", result);
      goto out;
    }

  if (request.count < CAMERA_CAPTURE_BUFS)
    {
      result = -ENOMEM;
      goto out;
    }

  count = request.count > CAMERA_CAPTURE_BUFS ?
          CAMERA_CAPTURE_BUFS : request.count;
  for (i = 0; i < (int)count; i++)
    {
      memset(&buffer, 0, sizeof(buffer));
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = i;
      if (ioctl(fd, VIDIOC_QUERYBUF, (uintptr_t)&buffer) < 0)
        {
          result = camera_errno();
          printf("desktop: camera QUERYBUF %d failed: %d\n", i, result);
          goto out;
        }

      buffers[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, buffer.m.offset);
      if (buffers[i] == MAP_FAILED)
        {
          buffers[i] = NULL;
          result = camera_errno();
          printf("desktop: camera mmap %d failed: %d\n", i, result);
          goto out;
        }

      lengths[i] = buffer.length;
      if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          result = camera_errno();
          printf("desktop: camera QBUF %d failed: %d\n", i, result);
          goto out;
        }
    }

  if (ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      result = camera_errno();
      printf("desktop: camera STREAMON failed: %d\n", result);
      goto out;
    }

  streaming = true;
  camera_set_state(CAMERA_STATE_RUNNING, 0);
  pollfd.fd = fd;
  pollfd.events = POLLIN;

  while (!camera_should_stop())
    {
      int ret;

      pollfd.revents = 0;
      ret = poll(&pollfd, 1, CAMERA_POLL_MS);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          result = camera_errno();
          break;
        }

      if (ret == 0 || (pollfd.revents & POLLIN) == 0)
        {
          if (!got_frame)
            {
              waited_ms += CAMERA_POLL_MS;
              if (waited_ms >= CAMERA_START_TIMEOUT_MS)
                {
                  result = -ETIMEDOUT;
                  printf("desktop: camera first frame timeout\n");
                  break;
                }
            }

          continue;
        }

      memset(&buffer, 0, sizeof(buffer));
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_MMAP;
      if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0)
        {
          if (errno == EINTR || errno == EAGAIN)
            {
              continue;
            }

          result = camera_errno();
          break;
        }

      /* Keep the newest frame; return older queued frames immediately
       * so CSI does not stall with only the backup buffer.
       */

      while (!camera_should_stop())
        {
          struct v4l2_buffer drop;

          pollfd.revents = 0;
          ret = poll(&pollfd, 1, 0);
          if (ret <= 0 || (pollfd.revents & POLLIN) == 0)
            {
              break;
            }

          memset(&drop, 0, sizeof(drop));
          drop.type = type;
          drop.memory = V4L2_MEMORY_MMAP;
          if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&drop) < 0)
            {
              break;
            }

          (void)ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buffer);
          buffer = drop;
        }

      if (buffer.index >= count || buffers[buffer.index] == NULL ||
          buffer.bytesused < CAMERA_FRAME_BYTES)
        {
          result = -EIO;
        }
      else
        {
          up_invalidate_dcache((uintptr_t)buffers[buffer.index],
                               (uintptr_t)buffers[buffer.index] +
                               buffer.bytesused);
          camera_scale_frame((FAR const uint16_t *)buffers[buffer.index]);
          got_frame = true;
        }

      if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          result = camera_errno();
          break;
        }

      if (result < 0)
        {
          break;
        }
    }

out:
  if (streaming)
    {
      (void)ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
    }

  for (i = 0; i < (int)count; i++)
    {
      if (buffers[i] != NULL)
        {
          munmap(buffers[i], lengths[i]);
        }
    }

  if (fd >= 0)
    {
      close(fd);
    }

  if (result < 0 && !camera_should_stop())
    {
      camera_set_state(CAMERA_STATE_FAILED, result);
      printf("desktop: camera preview failed: %d\n", result);
    }
  else
    {
      camera_set_state(CAMERA_STATE_IDLE, 0);
    }

  return NULL;
}

static void camera_ui_timer(lv_timer_t *timer)
{
  enum camera_state_e state;
  lv_image_dsc_t *source = NULL;
  uint32_t sequence;
  int result;
  int front;
  char text[80];

  (void)timer;
  pthread_mutex_lock(&g_camera.lock);
  if (g_camera.ready >= 0 && g_camera.writing != g_camera.ready)
    {
      g_camera.front = g_camera.ready;
      g_camera.ready = -1;
    }

  front = g_camera.front;
  sequence = g_camera.sequence;
  state = g_camera.state;
  result = g_camera.result;
  if (front >= 0 && sequence != g_camera.shown_sequence)
    {
      source = &g_camera.image_dsc[front];
      g_camera.shown_sequence = sequence;
    }
  pthread_mutex_unlock(&g_camera.lock);

  if (source != NULL && g_camera.image != NULL)
    {
      lv_image_set_src(g_camera.image, source);
      lv_obj_remove_flag(g_camera.image, LV_OBJ_FLAG_HIDDEN);
      lv_obj_invalidate(g_camera.image);
    }

  if (g_camera.status == NULL)
    {
      return;
    }

  if (state == CAMERA_STATE_RUNNING && front >= 0)
    {
      lv_label_set_text(g_camera.status, "1280 x 720  实时预览");
      lv_obj_set_style_text_color(g_camera.status,
                                  lv_color_hex(0xffffff), 0);
    }
  else if (state == CAMERA_STATE_FAILED)
    {
      if (result == -ETIMEDOUT)
        {
          snprintf(text, sizeof(text), "没有画面（超时）");
        }
      else
        {
          snprintf(text, sizeof(text), "摄像头不可用（%d）", result);
        }

      lv_label_set_text(g_camera.status, text);
      lv_obj_set_style_text_color(g_camera.status,
                                  lv_color_hex(0xff8e8e), 0);
    }
  else if (state == CAMERA_STATE_RUNNING)
    {
      lv_label_set_text(g_camera.status, "正在等待画面...");
      lv_obj_set_style_text_color(g_camera.status,
                                  lv_color_hex(0xd8deea), 0);
    }
  else
    {
      lv_label_set_text(g_camera.status, "正在启动摄像头...");
      lv_obj_set_style_text_color(g_camera.status,
                                  lv_color_hex(0xd8deea), 0);
    }
}

void desktop_camera_stop(void)
{
  FAR uint16_t *pixels[2];
  pthread_t thread;
  bool joinable;
  int i;

  pthread_mutex_lock(&g_camera.lock);
  g_camera.stop = true;
  joinable = g_camera.joinable;
  thread = g_camera.thread;
  pthread_mutex_unlock(&g_camera.lock);

  if (joinable)
    {
      pthread_join(thread, NULL);
    }

  if (g_camera.timer != NULL)
    {
      lv_timer_delete(g_camera.timer);
      g_camera.timer = NULL;
    }

  if (g_camera.image != NULL)
    {
      lv_image_set_src(g_camera.image, NULL);
      lv_obj_add_flag(g_camera.image, LV_OBJ_FLAG_HIDDEN);
    }

  pthread_mutex_lock(&g_camera.lock);
  g_camera.joinable = false;
  g_camera.state = CAMERA_STATE_IDLE;
  g_camera.result = 0;
  g_camera.front = -1;
  g_camera.ready = -1;
  g_camera.writing = -1;
  g_camera.sequence = 0;
  g_camera.shown_sequence = 0;
  g_camera.image = NULL;
  g_camera.status = NULL;
  for (i = 0; i < 2; i++)
    {
      pixels[i] = g_camera.pixels[i];
      g_camera.pixels[i] = NULL;
      memset(&g_camera.image_dsc[i], 0, sizeof(g_camera.image_dsc[i]));
    }
  pthread_mutex_unlock(&g_camera.lock);

  for (i = 0; i < 2; i++)
    {
      free(pixels[i]);
    }
}

int desktop_camera_start(lv_obj_t *parent, const lv_font_t *font)
{
  pthread_attr_t attr;
  lv_obj_t *viewport;
  int ret;
  int i;

  if (parent == NULL || font == NULL)
    {
      return -EINVAL;
    }

  desktop_camera_stop();
  for (i = 0; i < 2; i++)
    {
      g_camera.pixels[i] = memalign(64, CAMERA_PREVIEW_BYTES);
      if (g_camera.pixels[i] == NULL)
        {
          desktop_camera_stop();
          return -ENOMEM;
        }

      memset(g_camera.pixels[i], 0, CAMERA_PREVIEW_BYTES);
      g_camera.image_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
      g_camera.image_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
      g_camera.image_dsc[i].header.w = CAMERA_PREVIEW_WIDTH;
      g_camera.image_dsc[i].header.h = CAMERA_PREVIEW_HEIGHT;
      g_camera.image_dsc[i].header.stride = CAMERA_PREVIEW_WIDTH * 2;
      g_camera.image_dsc[i].data_size = CAMERA_PREVIEW_BYTES;
      g_camera.image_dsc[i].data =
        (FAR const uint8_t *)g_camera.pixels[i];
    }

  viewport = lv_obj_create(parent);
  lv_obj_set_size(viewport, CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT);
  lv_obj_set_pos(viewport, 12, 58);
  lv_obj_set_style_bg_color(viewport, lv_color_hex(0x05070b), 0);
  lv_obj_set_style_border_width(viewport, 0, 0);
  lv_obj_set_style_radius(viewport, 8, 0);
  lv_obj_set_style_pad_all(viewport, 0, 0);
  lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);

  g_camera.image = lv_image_create(viewport);
  lv_obj_set_size(g_camera.image, CAMERA_PREVIEW_WIDTH,
                  CAMERA_PREVIEW_HEIGHT);
  lv_obj_set_pos(g_camera.image, 0, 0);
  lv_obj_add_flag(g_camera.image, LV_OBJ_FLAG_HIDDEN);

  g_camera.status = lv_label_create(viewport);
  lv_label_set_text(g_camera.status, "正在启动摄像头...");
  lv_obj_set_style_text_font(g_camera.status, font, 0);
  lv_obj_set_style_text_color(g_camera.status, lv_color_hex(0xd8deea), 0);
  lv_obj_set_style_bg_color(g_camera.status, lv_color_hex(0x10141d), 0);
  lv_obj_set_style_bg_opa(g_camera.status, LV_OPA_80, 0);
  lv_obj_set_style_radius(g_camera.status, 8, 0);
  lv_obj_set_style_pad_hor(g_camera.status, 14, 0);
  lv_obj_set_style_pad_ver(g_camera.status, 8, 0);
  lv_obj_align(g_camera.status, LV_ALIGN_BOTTOM_MID, 0, -12);

  pthread_mutex_lock(&g_camera.lock);
  g_camera.stop = false;
  g_camera.state = CAMERA_STATE_STARTING;
  g_camera.result = 0;
  g_camera.front = -1;
  g_camera.ready = -1;
  g_camera.writing = -1;
  g_camera.sequence = 0;
  g_camera.shown_sequence = 0;
  pthread_mutex_unlock(&g_camera.lock);

  g_camera.timer = lv_timer_create(camera_ui_timer,
                                   CAMERA_UI_PERIOD_MS, NULL);
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CAMERA_THREAD_STACK);
  ret = pthread_create(&g_camera.thread, &attr, camera_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      camera_set_state(CAMERA_STATE_FAILED, -ret);
      return -ret;
    }

  pthread_mutex_lock(&g_camera.lock);
  g_camera.joinable = true;
  pthread_mutex_unlock(&g_camera.lock);
  pthread_setname_np(g_camera.thread, "camera_preview");
  return 0;
}
