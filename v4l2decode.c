#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <linux/videodev2.h>

#include "vdpau_private.h"

#include "v4l2.h"
#include "v4l2decode.h"

static int openDevices(v4l2_decoder_t *ctx);
static void cleanup(v4l2_decoder_t *ctx);
static void *pumpFIMC(void *arg);
static void *pumpMFC(void *arg);

static __u32 get_codec(VdpDecoderProfile profile)
{
    switch (profile)
    {
    case VDP_DECODER_PROFILE_MPEG1:
        return V4L2_PIX_FMT_MPEG1;

    case VDP_DECODER_PROFILE_MPEG2_SIMPLE:
    case VDP_DECODER_PROFILE_MPEG2_MAIN:
        return V4L2_PIX_FMT_MPEG2;

    case VDP_DECODER_PROFILE_H264_BASELINE:
    case VDP_DECODER_PROFILE_H264_MAIN:
    case VDP_DECODER_PROFILE_H264_HIGH:
        return V4L2_PIX_FMT_H264;

    case VDP_DECODER_PROFILE_MPEG4_PART2_SP:
    case VDP_DECODER_PROFILE_MPEG4_PART2_ASP:
        return V4L2_PIX_FMT_MPEG4;
    }
//    VDP_DECODER_PROFILE_VC1_SIMPLE
//    VDP_DECODER_PROFILE_VC1_MAIN
//    VDP_DECODER_PROFILE_VC1_ADVANCED
//    VDP_DECODER_PROFILE_MPEG4_PART2_SP
//    VDP_DECODER_PROFILE_MPEG4_PART2_ASP
//    VDP_DECODER_PROFILE_DIVX4_QMOBILE
//    VDP_DECODER_PROFILE_DIVX4_MOBILE
//    VDP_DECODER_PROFILE_DIVX4_HOME_THEATER
//    VDP_DECODER_PROFILE_DIVX4_HD_1080P
//    VDP_DECODER_PROFILE_DIVX5_QMOBILE
//    VDP_DECODER_PROFILE_DIVX5_MOBILE
//    VDP_DECODER_PROFILE_DIVX5_HOME_THEATER
//    VDP_DECODER_PROFILE_DIVX5_HD_1080P

    //            return V4L2_PIX_FMT_H263;
    //            return V4L2_PIX_FMT_XVID;
    return V4L2_PIX_FMT_H264;
}

void *decoder_open(VdpDecoderProfile profile, uint32_t width, uint32_t height)
{
    v4l2_decoder_t *ctx = calloc(1, sizeof(v4l2_decoder_t));
    struct v4l2_format fmt;

    ctx->width = width;
    ctx->height = height;

    ctx->decoderHandle = -1;
    ctx->converterHandle = -1;

    ctx->outputBuffersCount = -1;
    ctx->captureBuffersCount = -1;
    ctx->converterBuffersCount = -1;

    ctx->codec = get_codec(profile);
    if(openDevices(ctx)) {
        cleanup(ctx);
        return NULL;
    }

    // Setup mfc output
    // Set mfc output format
    memzero(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = ctx->codec;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = STREAM_BUFFER_SIZE;
    if (ioctl(ctx->decoderHandle, VIDIOC_S_FMT, &fmt)) {
        VDPAU_ERR("Failed to setup for MFC decoding");
        cleanup(ctx);
        return NULL;
    }

    // Setup MFC CAPTURE format if we don't need FIMC conversion
    if (!ctx->needConvert) {
        memzero(fmt);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
        if (ioctl(ctx->decoderHandle, VIDIOC_S_FMT, &fmt)) {
            VDPAU_ERR("Set MFC Capture Format failed");
            cleanup(ctx);
            return NULL;
        }
    }

    // Request mfc output buffers
    ctx->outputBuffersCount = RequestBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP, STREAM_BUFFER_CNT);
    if (ctx->outputBuffersCount == V4L2_ERROR) {
        VDPAU_ERR("REQBUFS failed on queue of MFC");
        cleanup(ctx);
        return NULL;
    }
    VDPAU_DBG("REQBUFS Number of MFC buffers is %d (requested %d)", ctx->outputBuffersCount, STREAM_BUFFER_CNT);

    // Memory Map mfc output buffers
    ctx->outputBuffers = (v4l2_buffer_t *)calloc(ctx->outputBuffersCount, sizeof(v4l2_buffer_t));
    if(!ctx->outputBuffers) {
        VDPAU_ERR("cannot allocate buffers\n");
        cleanup(ctx);
        return NULL;
    }
    if(!MmapBuffers(ctx->decoderHandle, ctx->outputBuffersCount, ctx->outputBuffers, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP, FALSE)) {
        VDPAU_ERR("cannot mmap output buffers\n");
        cleanup(ctx);
        return NULL;
    }
    VDPAU_DBG("Succesfully mmapped %d buffers", ctx->outputBuffersCount);

    return ctx;
}

void decoder_close(void *private)
{
    v4l2_decoder_t *ctx = (v4l2_decoder_t*)private;
    cleanup(ctx);

    free(ctx);
}

static int process_header(v4l2_decoder_t *ctx, uint32_t buffer_count,
                    VdpBitstreamBuffer const *buffers);

static VdpStatus process_frames(v4l2_decoder_t *ctx, uint32_t buffer_count,
                    VdpBitstreamBuffer const *buffers, VdpVideoSurface output);

VdpStatus decoder_decode(void *private, uint32_t buffer_count,
                    VdpBitstreamBuffer const *buffers, VdpVideoSurface output)
{
    v4l2_decoder_t *ctx = (v4l2_decoder_t*)private;

    if (!ctx->headerProcessed) {
        int ret = process_header(ctx, buffer_count, buffers);
        if(ret)
            return ret < 0 ? VDP_STATUS_ERROR : VDP_STATUS_OK;
        if (ctx->codec != V4L2_PIX_FMT_H263)
            return VDP_STATUS_OK;
    }

    return process_frames(ctx, buffer_count, buffers, output);
}




static int openDevices(v4l2_decoder_t *ctx)
{
    DIR *dir;
    struct dirent *ent;

    if ((dir = opendir ("/sys/class/video4linux/")) != NULL) {
        while ((ent = readdir (dir)) != NULL) {
            if (strncmp(ent->d_name, "video", 5) == 0) {
                char *p;
                char name[64];
                char devname[64];
                char sysname[64];
                char drivername[32];
                char target[1024];
                int ret;

                snprintf(sysname, 64, "/sys/class/video4linux/%s", ent->d_name);
                snprintf(name, 64, "/sys/class/video4linux/%s/name", ent->d_name);

                FILE* fp = fopen(name, "r");
                if (fgets(drivername, 32, fp) != NULL) {
                    p = strchr(drivername, '\n');
                    if (p != NULL)
                        *p = '\0';
                } else {
                    fclose(fp);
                    continue;
                }
                fclose(fp);

                ret = readlink(sysname, target, sizeof(target));
                if (ret < 0)
                    continue;
                target[ret] = '\0';
                p = strrchr(target, '/');
                if (p == NULL)
                    continue;

                sprintf(devname, "/dev/%s", ++p);

                if (ctx->decoderHandle < 0 && strstr(drivername, "s5p-mfc-dec") != NULL) {
                    struct v4l2_capability cap;
                    int fd = open(devname, O_RDWR | O_NONBLOCK, 0);
                    if (fd > 0) {
                        memzero(cap);
                        if (!ioctl(fd, VIDIOC_QUERYCAP, &cap))
                            if ((cap.capabilities & V4L2_CAP_STREAMING) &&
                                    ((cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) ||
                                    (cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE)))) {
                                ctx->decoderHandle = fd;
                                VDPAU_DBG("Found %s %s", drivername, devname);

                                // Only need FIMC if we cannot set this capture pixel format to NV12M
                                struct v4l2_format fmt;
                                memzero(fmt);
                                fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                                fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
                                if(ioctl(ctx->decoderHandle, VIDIOC_TRY_FMT, &fmt)) {
                                    ctx->needConvert = 1;
                                    VDPAU_DBG("Direct decoding to untiled picture is NOT supported, FIMC conversion needed");
                                } else {
                                    ctx->needConvert = 0;
                                    VDPAU_DBG("Direct decoding to untiled picture is supported, no conversion needed");
                                    if (ctx->converterHandle >= 0)
                                        close(ctx->converterHandle);
                                }

                            }
                  }
                  if (ctx->decoderHandle < 0)
                      close(fd);
                }
                if (ctx->needConvert && ctx->converterHandle < 0 && strstr(drivername, "fimc") != NULL && strstr(drivername, "m2m") != NULL) {
                    struct v4l2_capability cap;
                    int fd = open(devname, O_RDWR | O_NONBLOCK, 0);
                    if (fd > 0) {
                        memzero(cap);
                        if (!ioctl(fd, VIDIOC_QUERYCAP, &cap))
                            if ((cap.capabilities & V4L2_CAP_STREAMING) &&
                                    ((cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) ||
                                    (cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE)))) {
                                ctx->converterHandle = fd;
                                VDPAU_DBG("Found %s %s", drivername, devname);
                            }
                    }
                    if (ctx->converterHandle < 0)
                        close(fd);
                }
            }
        }
        closedir (dir);
    }
    return 0;
}

static void cleanup(v4l2_decoder_t *ctx)
{
    if (ctx->decoderHandle >= 0) {
        if (ctx->outputBuffers)
            ctx->outputBuffers = FreeBuffers(ctx->outputBuffersCount, ctx->outputBuffers);
        if (ctx->captureBuffers)
            ctx->captureBuffers = FreeBuffers(ctx->captureBuffersCount, ctx->captureBuffers);
        if (StreamOn(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, VIDIOC_STREAMOFF))
            VDPAU_ERR("Stream OFF");
        if (StreamOn(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, VIDIOC_STREAMOFF))
            VDPAU_ERR("Stream OFF");
        close(ctx->decoderHandle);
    }
    if (ctx->converterHandle >= 0) {
        if (ctx->converterBuffers)
            ctx->converterBuffers = FreeBuffers(ctx->converterBuffersCount, ctx->converterBuffers);
        if (StreamOn(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, VIDIOC_STREAMOFF))
            VDPAU_ERR("Stream OFF");
        if (StreamOn(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, VIDIOC_STREAMOFF))
            VDPAU_ERR("Stream OFF");
        close(ctx->converterHandle);
    }
}

static int process_header(v4l2_decoder_t *ctx, uint32_t buffer_count,
                    VdpBitstreamBuffer const *buffers)
{
    int size = 0, ret, i;
    struct v4l2_format fmt;
    struct v4l2_control ctrl;
    struct v4l2_crop crop;

    int capturePlane1Size;
    int capturePlane2Size;
    int capturePlane3Size;

    // Prepare header frame
    for(i=0 ; i<buffer_count ; i++) {
        memcpy(ctx->outputBuffers[0].cPlane[0] + size, buffers[i].bitstream, buffers[i].bitstream_bytes);
        size += buffers[i].bitstream_bytes;
    }
    ctx->outputBuffers[0].iBytesUsed[0] = size;

    // Queue header to mfc output
    ret = QueueBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP, &ctx->outputBuffers[0]);
    if (ret == V4L2_ERROR) {
        VDPAU_ERR("queue input buffer");
        return -1;
    }

    // STREAMON on mfc OUTPUT
    if (!StreamOn(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, VIDIOC_STREAMON)) {
        VDPAU_ERR("Failed to Stream ON");
        return -1;
    }
    VDPAU_DBG("Stream ON");

    // Get mfc capture picture format
    memzero(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(ctx->decoderHandle, VIDIOC_G_FMT, &fmt)) {
        VDPAU_ERR("Failed to get format from");
        return -1;
    }
    capturePlane1Size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    capturePlane2Size = fmt.fmt.pix_mp.plane_fmt[1].sizeimage;
    capturePlane3Size = fmt.fmt.pix_mp.plane_fmt[2].sizeimage;
    VDPAU_DBG("G_FMT: fmt (%dx%d), %c%c%c%c plane[0]=%d plane[1]=%d plane[2]=%d", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
                        fmt.fmt.pix_mp.pixelformat & 0xFF, (fmt.fmt.pix_mp.pixelformat >> 8) & 0xFF,
                        (fmt.fmt.pix_mp.pixelformat >> 16) & 0xFF, (fmt.fmt.pix_mp.pixelformat >> 24) & 0xFF,
                        capturePlane1Size, capturePlane2Size, capturePlane3Size);

    // Setup FIMC OUTPUT fmt with data from MFC CAPTURE if required
    if(ctx->needConvert) {
        fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (ioctl(ctx->converterHandle, VIDIOC_S_FMT, &fmt)) {
            VDPAU_ERR("Failed to SFMT on OUTPUT of FIMC");
            return -1;
        }
        VDPAU_DBG("S_FMT %dx%d", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
    }

    // Get mfc needed number of buffers
    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    if (ioctl(ctx->decoderHandle, VIDIOC_G_CTRL, &ctrl)) {
        VDPAU_ERR("Failed to get the number of buffers required");
        return -1;
    }
    ctx->captureBuffersCount = (int)(ctrl.value * 1.5);

    // Get mfc capture crop
    memzero(crop);
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(ctx->decoderHandle, VIDIOC_G_CROP, &crop)) {
        VDPAU_ERR("Failed to get crop information");
        return -1;
    }
    VDPAU_DBG("G_CROP %dx%d", crop.c.width, crop.c.height);
    ctx->captureWidth = crop.c.width;
    ctx->captureHeight = crop.c.height;

    if(ctx->needConvert) {
        //setup FIMC OUTPUT crop with data from MFC CAPTURE
        crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (ioctl(ctx->converterHandle, VIDIOC_S_CROP, &crop)) {
            VDPAU_ERR("Failed to set CROP on OUTPUT");
            return -1;
        }
        VDPAU_DBG("S_CROP %dx%d", crop.c.width, crop.c.height);
    }

    // Request mfc capture buffers
    ctx->captureBuffersCount = RequestBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, ctx->captureBuffersCount);
    if (ctx->captureBuffersCount == V4L2_ERROR) {
        VDPAU_ERR("REQBUFS failed");
        return -1;
    }
    VDPAU_DBG("REQBUFS Number of buffers is %d", ctx->captureBuffersCount);

    // Memory Map and queue mfc capture buffers
    ctx->captureBuffers = (v4l2_buffer_t *)calloc(ctx->captureBuffersCount, sizeof(v4l2_buffer_t));
    if(!ctx->captureBuffers) {
        VDPAU_ERR("cannot allocate buffers");
        return -1;
    }
    if(!MmapBuffers(ctx->decoderHandle, ctx->captureBuffersCount, ctx->captureBuffers, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, TRUE)) {
        VDPAU_DBG("cannot mmap capture buffers");
        return -1;
    }
    VDPAU_DBG("Succesfully mmapped and queued %d buffers", ctx->captureBuffersCount);

    // STREAMON on mfc CAPTURE
    if (!StreamOn(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, VIDIOC_STREAMON)) {
        VDPAU_ERR("Failed to Stream ON");
        return -1;
    }
    VDPAU_DBG("Stream ON");

    if(ctx->needConvert) {
        // Request fimc capture buffers
        ret = RequestBuffer(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_USERPTR, ctx->captureBuffersCount);
        if (ret == V4L2_ERROR) {
            VDPAU_ERR("REQBUFS failed");
            return -1;
        }
        VDPAU_DBG("REQBUFS Number of buffers is %d", ret);

        // Setup fimc capture
        memzero(fmt);
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width = ctx->width;
        fmt.fmt.pix_mp.height = ctx->height;
        fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
        if (ioctl(ctx->converterHandle, VIDIOC_S_FMT, &fmt)) {
            VDPAU_ERR("Failed SFMT");
            return -1;
        }
        VDPAU_DBG("S_FMT %dx%d", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);

        // Setup FIMC CAPTURE crop
        memzero(crop);
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        crop.c.left = 0;
        crop.c.top = 0;
        crop.c.width = ctx->width;
        crop.c.height = ctx->height;
        if (ioctl(ctx->converterHandle, VIDIOC_S_CROP, &crop)) {
            VDPAU_ERR("Failed to set CROP on OUTPUT");
            return -1;
        }
        VDPAU_DBG("S_CROP %dx%d", crop.c.width, crop.c.height);
        memzero(fmt);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (ioctl(ctx->converterHandle, VIDIOC_G_FMT, &fmt)) {
            VDPAU_ERR("Failed to get format from");
            return -1;
        }
        capturePlane1Size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        capturePlane2Size = fmt.fmt.pix_mp.plane_fmt[1].sizeimage;
        capturePlane3Size = fmt.fmt.pix_mp.plane_fmt[2].sizeimage;

        // Request fimc capture buffers
        ctx->converterBuffersCount = RequestBuffer(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, CONVERTER_VIDEO_BUFFERS_CNT);
        if (ctx->converterBuffersCount == V4L2_ERROR) {
            VDPAU_ERR("REQBUFS failed");
            return -1;
        }
        VDPAU_DBG("REQBUFS Number of buffers is %d", ctx->converterBuffersCount);
        VDPAU_DBG("buffer parameters: plane[0]=%d plane[1]=%d plane[2]=%d", capturePlane1Size, capturePlane2Size, capturePlane3Size);

        // Memory Map and queue mfc capture buffers
        ctx->converterBuffers = (v4l2_buffer_t *)calloc(ctx->converterBuffersCount, sizeof(v4l2_buffer_t));
        if(!ctx->converterBuffers) {
            VDPAU_ERR("cannot allocate buffers");
            return -1;
        }
        if(!MmapBuffers(ctx->converterHandle, ctx->converterBuffersCount, ctx->converterBuffers, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, TRUE)) {
            VDPAU_ERR("cannot mmap capture buffers\n");
            return -1;
        }
        VDPAU_DBG("Succesfully mmapped and queued %d buffers", ctx->converterBuffersCount);

        if (StreamOn(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, VIDIOC_STREAMON))
            VDPAU_DBG("Stream ON");
        else
            VDPAU_ERR("Failed to Stream ON");
        if (StreamOn(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, VIDIOC_STREAMON))
            VDPAU_DBG("Stream ON");
        else
            VDPAU_ERR("Failed to Stream ON");

        // create FIMC pumping threads
        pthread_create(&ctx->fimc_thread, NULL, &pumpFIMC, ctx);
        pthread_create(&ctx->mfc_thread, NULL, &pumpMFC, ctx);
    }

    // Dequeue header on input queue
    ret = DequeueBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP);
    if (ret < 0) {
        VDPAU_ERR("error dequeue output buffer, got number %d, errno %d", ret, errno);
        return -1;
    }
    ctx->outputBuffers[ret].bQueue = FALSE;

    ctx->headerProcessed = TRUE;
    return 0;
}

static VdpStatus process_frames(v4l2_decoder_t *ctx, uint32_t buffer_count,
                    VdpBitstreamBuffer const *buffers, VdpVideoSurface output)
{
    int index = 0;
    int ret, i;

    while (index < ctx->outputBuffersCount && ctx->outputBuffers[index].bQueue)
        index++;

    if (index >= ctx->outputBuffersCount) { //all input buffers are busy, dequeue needed
        ret = PollOutput(ctx->decoderHandle, 1000); // POLLIN - Poll Capture, POLLOUT - Poll Output
        if (ret == V4L2_ERROR) {
            VDPAU_ERR("PollInput Error");
            return VDP_STATUS_ERROR;
        } else if (ret == V4L2_BUSY) {
            VDPAU_ERR("PollOutput busy after timeout");
            return VDP_STATUS_ERROR;
        } else if (ret != V4L2_READY) {
            VDPAU_ERR("PollOutput unexpected error, what the? %d", ret);
            return VDP_STATUS_ERROR;
        }
        index = DequeueBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP);
        if (index < 0) {
            VDPAU_ERR("error dequeue output buffer, got number %d, errno %d", index, errno);
            return VDP_STATUS_ERROR;
        }
    }

    // Parse frame, copy it to buffer
    int frameSize = 0;
    for(i=0 ; i<buffer_count ; i++) {
        memcpy(ctx->outputBuffers[index].cPlane[0] + frameSize, buffers[i].bitstream, buffers[i].bitstream_bytes);
        frameSize += buffers[i].bitstream_bytes;
    }
    ctx->outputBuffers[index].iBytesUsed[0] = frameSize;

    // Queue buffer into input queue
    ctx->outputBuffers[index].iBytesUsed[0] = frameSize;
    ret = QueueBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP, &ctx->outputBuffers[index]);
    if (ret == V4L2_ERROR) {
        VDPAU_ERR("Failed to queue buffer with index %d, errno %d", index, errno);
        return VDP_STATUS_ERROR;
    }

    return VDP_STATUS_OK;
}

static void *pumpFIMC(void *arg)
{
    int ret, index;
    v4l2_decoder_t *ctx = (v4l2_decoder_t *)arg;

    while(1){
        // Dequeue frame from fimc output and pass it back to mfc capture
        ret = PollOutput(ctx->converterHandle, 1000);
        if (ret == V4L2_ERROR) {
            VDPAU_ERR("PollOutput Error");
            return VDP_STATUS_ERROR;
        } else if (ret == V4L2_BUSY) {
            continue;
        } else if (ret != V4L2_READY) {
            VDPAU_ERR("PollOutput unexpected error, what the? %d", ret);
            return VDP_STATUS_ERROR;
        }

        index = DequeueBuffer(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_USERPTR);
        if (index < 0) {
            if (index != -EAGAIN) {// Dequeue buffer not ready, need more data on input. EAGAIN = 11
                VDPAU_ERR("error dequeue output buffer, got number %d", index);
                return VDP_STATUS_ERROR;
            }
        } else {
            ret = QueueBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, &ctx->captureBuffers[index]);
            if (ret == V4L2_ERROR) {
                VDPAU_ERR("Failed to queue buffer with index %d, errno = %d", index, errno);
                return VDP_STATUS_ERROR;
            }
        }
    }
}

static void *pumpMFC(void *arg)
{
    int ret, index;
    v4l2_decoder_t *ctx = (v4l2_decoder_t *)arg;

    while(1){
        // Dequeue frame from MFC capture and pass it back to mfc capture
        ret = PollInput(ctx->decoderHandle, 1000);
        if (ret == V4L2_ERROR) {
            VDPAU_ERR("PollInput Error");
            return VDP_STATUS_ERROR;
        } else if (ret == V4L2_BUSY) {
            continue;
        } else if (ret != V4L2_READY) {
            VDPAU_ERR("PollInput unexpected error, what the? %d", ret);
            return VDP_STATUS_ERROR;
        }

        index = DequeueBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP);
        if (index < 0) {
            if (index != -EAGAIN) {// Dequeue buffer not ready, need more data on input. EAGAIN = 11
                VDPAU_ERR("error dequeue output buffer, got number %d", index);
                return VDP_STATUS_ERROR;
            }
        } else {
            //Process frame after mfc
            ret = QueueBuffer(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_USERPTR, &ctx->captureBuffers[index]);
            if (ret == V4L2_ERROR) {
                VDPAU_ERR("Failed to queue buffer with index %d", index);
                return VDP_STATUS_ERROR;
            }
        }
    }
}

VdpStatus decoder_get_picture(void *context, int *frame, void ***output)
{
    v4l2_decoder_t *ctx = (v4l2_decoder_t *)context;
    int index = 0;

    *frame = -1;
    *output = NULL;

    if(ctx->needConvert) {
        index = DequeueBuffer(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP);
        if (index < 0) {
            if (index == -EAGAIN) // Dequeue buffer not ready, need more data on input. EAGAIN = 11
                return VDP_STATUS_OK;
            VDPAU_ERR("error dequeue output buffer, got number %d %d", index, errno);
            return VDP_STATUS_ERROR;
        }
        *output = ctx->converterBuffers[index].cPlane;
        *frame = index;
    } else {
        index = DequeueBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP);
        if (index < 0) {
            if (index == -EAGAIN) // Dequeue buffer not ready, need more data on input. EAGAIN = 11
                return VDP_STATUS_OK;
            VDPAU_ERR("error dequeue output buffer, got number %d", index);
            return VDP_STATUS_ERROR;
        }
        *output = ctx->captureBuffers[index].cPlane;
        *frame = index;
    }

    return VDP_STATUS_OK;
}

VdpStatus decoder_release_picture(void *context, int frame)
{
    v4l2_decoder_t *ctx = (v4l2_decoder_t *)context;
    int ret;

    if(ctx->needConvert) {
        ret = QueueBuffer(ctx->converterHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, &ctx->converterBuffers[frame]);
        if (ret == V4L2_ERROR) {
            VDPAU_ERR("Failed to queue buffer with index %d, errno = %d", frame, errno);
            return VDP_STATUS_ERROR;
        }
    } else {
        ret = QueueBuffer(ctx->decoderHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, &ctx->captureBuffers[frame]);
        if (ret == V4L2_ERROR) {
            VDPAU_ERR("Failed to queue buffer with index %d, errno = %d", frame, errno);
            return VDP_STATUS_ERROR;
        }
    }

    return VDP_STATUS_OK;
}
