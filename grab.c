#include "grab.h"

#define _POSIX_C_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "utils.h"


void to_rgb(uint8_t* rgb, int y, int cb, int cr)
{
    rgb[0] = clamp_d(1.164*(y - 16) + 1.596*(cr - 128));
    rgb[1] = clamp_d(1.164*(y - 16) - 0.813*(cr - 128) - 0.392*(cb - 128));
    rgb[2] = clamp_d(1.164*(y - 16) + 2.017*(cb - 128));
}


int print_capabilities(int dev_fd)
{
    struct v4l2_capability c;
    int r = ioctl(dev_fd, VIDIOC_QUERYCAP, &c);
    if (r < 0) {
        fprintf(
            stderr, "Error: Couldn't check capabilities (%s)\n",
            strerror(errno));
        return -1;
    };

    fputs("Capabilities:\n", stdout);
    printf("  driver = %s\n", c.driver);
    printf("  card = %s\n", c.card);
    printf("  bus_info = %s\n", c.bus_info);
    printf("  version = %d\n", c.version);
    if (c.capabilities & V4L2_CAP_VIDEO_CAPTURE)
        fputs("  CAP_VIDEO_CAPTURE\n", stdout);
    if (c.capabilities & V4L2_CAP_VIDEO_M2M)
        fputs("  CAP_VIDEO_M2M\n", stdout);
    if (c.capabilities & V4L2_CAP_VBI_CAPTURE)
        fputs("  CAP_VBI_CAPTURE\n", stdout);
    if (c.capabilities & V4L2_CAP_READWRITE)
        fputs("  CAP_READWRITE\n", stdout);
    if (c.capabilities & V4L2_CAP_ASYNCIO)
        fputs("  CAP_ASYNCIO\n", stdout);
    if (c.capabilities & V4L2_CAP_STREAMING)
        fputs("  CAP_STREAMING\n", stdout);
    return 0;
}


int print_input_info(int dev_fd)
{
    struct v4l2_input in;

    for (in.index = 0; ; ++in.index) {
        int r = ioctl(dev_fd, VIDIOC_ENUMINPUT, &in);
        if (r < 0) {
            if (errno == EINVAL)
                return 0;
            fprintf(
                stderr, "Error: Couldn't enumerate inputs (%s)\n",
                strerror(errno));
            return -1;
        };

        printf("Device %d:\n", in.index);
        printf("  name = %s\n", in.name);
        printf("  type = %d\n", in.type);
        printf("  std = %u\n", (unsigned int)in.std);
    };
}


int set_input(int dev_fd, int input_index)
{
    int r = ioctl(dev_fd, VIDIOC_S_INPUT, &input_index);
    if (r < 0) {
        fprintf(
            stderr, "Error: Couldn't set input (%s)\n", strerror(errno));
        return -1;
    };

    return 0;
}


int print_format(int fd)
{
    struct v4l2_format f;
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int r = ioctl(fd, VIDIOC_G_FMT, &f);
    if (r < 0) {
        fprintf(stderr, "Error: Couldn't get format (%s)\n", strerror(errno));
        return -1;
    };

    fputs("Format:\n", stdout);
    printf("  type = %u\n", f.type);
    printf("  width = %u\n", f.fmt.pix.width);
    printf("  height = %u\n", f.fmt.pix.height);
    char pixelformat[5] = {
        f.fmt.pix.pixelformat & 0xFF,
        (f.fmt.pix.pixelformat & 0xFF00) >> 8,
        (f.fmt.pix.pixelformat & 0xFF0000) >> 16,
        (f.fmt.pix.pixelformat & 0xFF000000) >> 24,
        0
    };
    printf("  pixelformat = %u = %s\n", f.fmt.pix.pixelformat, pixelformat);
    printf("  sizeimage = %u\n", f.fmt.pix.sizeimage);
    return 0;
}


void print_info(const char* device, int input_index)
{
    int dev_fd = open(device, O_RDWR);
    if (dev_fd < 0) {
        fprintf(stderr, "Error: Couldn't open device (%s)\n", strerror(errno));
        return;
    };

    print_capabilities(dev_fd);
    print_input_info(dev_fd);
    set_input(dev_fd, input_index);
    print_format(dev_fd);

    close(dev_fd);
}


struct grabber* create_grabber(
        const char* device, int input_index, int width, int height)
{
    struct grabber* grabber = malloc(sizeof(struct grabber));
    int r;

    grabber->dev_fd = open(device, O_RDWR);
    if (grabber->dev_fd < 0) {
        fprintf(stderr, "Error: Couldn't open device (%s)\n", strerror(errno));
        free(grabber);
        return NULL;
    };

    r = set_input(grabber->dev_fd, input_index);
    if (r < 0) {
        free(grabber);
        return NULL;
    };

    struct v4l2_format f;
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    r = ioctl(grabber->dev_fd, VIDIOC_G_FMT, &f);
    if (r < 0) {
        fprintf(stderr, "Error: Couldn't get format (%s)\n", strerror(errno));
        free(grabber);
        return NULL;
    };

    f.fmt.pix.width = width;
    f.fmt.pix.height = height;

    r = ioctl(grabber->dev_fd, VIDIOC_S_FMT, &f);
    if (r < 0) {
        fprintf(stderr, "Error: Couldn't set format (%s)\n", strerror(errno));
        free(grabber);
        return NULL;
    };
    grabber->format_type = f.type;
    grabber->width = f.fmt.pix.width;
    grabber->height = f.fmt.pix.height;
    grabber->pixelformat = f.fmt.pix.pixelformat;

    grabber->frame = malloc(grabber->width * grabber->height * 3);

    struct v4l2_requestbuffers rb;
    rb.count = 1;
    rb.type = f.type;
    rb.memory = V4L2_MEMORY_MMAP;
    rb.reserved[0] = 0;
    rb.reserved[1] = 0;
    r = ioctl(grabber->dev_fd, VIDIOC_REQBUFS, &rb);
    if (r < 0) {
        fprintf(
            stderr, "Buffer request was denied (%s)\n", strerror(errno));
        free(grabber);
        return NULL;
    };

    grabber->buffer.type = f.type;
    grabber->buffer.index = 0;
    grabber->buffer.reserved = 0;
    grabber->buffer.reserved2 = 0;
    r = ioctl(grabber->dev_fd, VIDIOC_QUERYBUF, &grabber->buffer);
    if (r < 0) {
        fprintf(
            stderr, "Error: Couldn't query buffer address (%s)\n",
            strerror(errno));
        free(grabber);
        return NULL;
    };

    grabber->raw_frame = mmap(
        NULL, grabber->buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
        grabber->dev_fd, grabber->buffer.m.offset);
    if (grabber->raw_frame == NULL) {
        fputs("Error: Couldn't mmap frame\n", stderr);
        free(grabber);
        return NULL;
    };

    r = ioctl(grabber->dev_fd, VIDIOC_STREAMON, &grabber->format_type);
    if (r < 0) {
        fprintf(
            stderr, "Error: Couldn't start streaming (%s)\n", strerror(errno));
        munmap(grabber->raw_frame, grabber->buffer.length);
        free(grabber);
        return NULL;
    };

    return grabber;
}


int grab(struct grabber* grabber)
{
    int r;

    r = ioctl(grabber->dev_fd, VIDIOC_QBUF, &grabber->buffer);
    if (r < 0) {
        fprintf(
            stderr, "Error: Couldn't enqueue buffer (%s)\n",
            strerror(errno));
        return -1;
    };

    r = ioctl(grabber->dev_fd, VIDIOC_DQBUF, &grabber->buffer);
    if (r < 0) {
        fprintf(
            stderr, "Error: Couldn't dequeue buffer (%s)\n",
            strerror(errno));
        return -1;
    };

    return 0;
}


int process(struct grabber* grabber)
{
    uint8_t* p = grabber->frame;
    if (grabber->pixelformat == 1448695129) {  // YUYV
        for (int i = 0; i <= grabber->buffer.length - 4; i += 4) {
            uint8_t y0 = grabber->raw_frame[i];
            uint8_t cb = grabber->raw_frame[i + 1];
            uint8_t y1 = grabber->raw_frame[i + 2];
            uint8_t cr = grabber->raw_frame[i + 3];
            to_rgb(p, y0, cb, cr);
            to_rgb(p + 3, y1, cb, cr);
            p += 6;
        };
    } else if (grabber->pixelformat == 1498831189) {  // UYVY
        for (int i = 0; i <= grabber->buffer.length - 4; i += 4) {
            uint8_t cb = grabber->raw_frame[i];
            uint8_t y0 = grabber->raw_frame[i + 1];
            uint8_t cr = grabber->raw_frame[i + 2];
            uint8_t y1 = grabber->raw_frame[i + 3];
            to_rgb(p, y0, cb, cr);
            to_rgb(p + 3, y1, cb, cr);
            p += 6;
        };
    } else {
        fprintf(stderr, "Unrecognized pixelformat %d\n",
            grabber->pixelformat);
        return -1;
    };

    return 0;
}


void delete_grabber(struct grabber* grabber)
{
    ioctl(grabber->dev_fd, VIDIOC_STREAMOFF, &grabber->format_type);
    munmap(grabber->raw_frame, grabber->buffer.length);
    close(grabber->dev_fd);
    free(grabber->frame);
    free(grabber);
}
