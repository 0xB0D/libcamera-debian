/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * v4l2_camera_proxy.h - Proxy to V4L2 compatibility camera
 */

#ifndef __V4L2_CAMERA_PROXY_H__
#define __V4L2_CAMERA_PROXY_H__

#include <linux/videodev2.h>
#include <map>
#include <memory>
#include <set>
#include <sys/mman.h>
#include <sys/types.h>
#include <vector>

#include <libcamera/camera.h>

#include "v4l2_camera.h"

using namespace libcamera;

class V4L2CameraFile;

class V4L2CameraProxy
{
public:
	V4L2CameraProxy(unsigned int index, std::shared_ptr<Camera> camera);

	int open(V4L2CameraFile *file);
	void close(V4L2CameraFile *file);
	void *mmap(void *addr, size_t length, int prot, int flags, off64_t offset);
	int munmap(void *addr, size_t length);

	int ioctl(V4L2CameraFile *file, unsigned long request, void *arg);

private:
	bool validateBufferType(uint32_t type);
	bool validateMemoryType(uint32_t memory);
	void setFmtFromConfig(StreamConfiguration &streamConfig);
	unsigned int calculateSizeImage(StreamConfiguration &streamConfig);
	void querycap(std::shared_ptr<Camera> camera);
	void tryFormat(struct v4l2_format *arg);
	enum v4l2_priority maxPriority();
	void updateBuffers();
	void freeBuffers();

	int vidioc_querycap(struct v4l2_capability *arg);
	int vidioc_enum_framesizes(V4L2CameraFile *file, struct v4l2_frmsizeenum *arg);
	int vidioc_enum_fmt(V4L2CameraFile *file, struct v4l2_fmtdesc *arg);
	int vidioc_g_fmt(V4L2CameraFile *file, struct v4l2_format *arg);
	int vidioc_s_fmt(V4L2CameraFile *file, struct v4l2_format *arg);
	int vidioc_try_fmt(V4L2CameraFile *file, struct v4l2_format *arg);
	int vidioc_g_priority(V4L2CameraFile *file, enum v4l2_priority *arg);
	int vidioc_s_priority(V4L2CameraFile *file, enum v4l2_priority *arg);
	int vidioc_enuminput(V4L2CameraFile *file, struct v4l2_input *arg);
	int vidioc_g_input(V4L2CameraFile *file, int *arg);
	int vidioc_s_input(V4L2CameraFile *file, int *arg);
	int vidioc_reqbufs(V4L2CameraFile *file, struct v4l2_requestbuffers *arg);
	int vidioc_querybuf(V4L2CameraFile *file, struct v4l2_buffer *arg);
	int vidioc_qbuf(V4L2CameraFile *file, struct v4l2_buffer *arg);
	int vidioc_dqbuf(V4L2CameraFile *file, struct v4l2_buffer *arg, MutexLocker *locker);
	int vidioc_streamon(V4L2CameraFile *file, int *arg);
	int vidioc_streamoff(V4L2CameraFile *file, int *arg);

	bool hasOwnership(V4L2CameraFile *file);
	int acquire(V4L2CameraFile *file);
	void release(V4L2CameraFile *file);

	static unsigned int bplMultiplier(uint32_t format);
	static unsigned int imageSize(uint32_t format, unsigned int width,
				      unsigned int height);

	static PixelFormat v4l2ToDrm(uint32_t format);
	static uint32_t drmToV4L2(const PixelFormat &format);

	static const std::set<unsigned long> supportedIoctls_;

	unsigned int refcount_;
	unsigned int index_;

	struct v4l2_format curV4L2Format_;
	StreamConfiguration streamConfig_;
	struct v4l2_capability capabilities_;
	unsigned int bufferCount_;
	unsigned int currentBuf_;
	unsigned int sizeimage_;

	std::vector<struct v4l2_buffer> buffers_;
	std::map<void *, unsigned int> mmaps_;

	std::set<V4L2CameraFile *> files_;

	std::unique_ptr<V4L2Camera> vcam_;

	/*
	 * This is the exclusive owner of this V4L2CameraProxy instance.
	 * When there is no owner, anybody can call any ioctl before reqbufs.
	 * The first file to call reqbufs with count > 0 or s_fmt will become
	 * the owner, and when the owner calls reqbufs with count = 0 it will
	 * release ownership. Any buffer-related ioctl (except querybuf) or
	 * s_fmt that is called by a non-owner while there exists an owner
	 * will return -EBUSY.
	 */
	V4L2CameraFile *owner_;

	/* This mutex is to serialize access to the proxy. */
	Mutex proxyMutex_;
};

#endif /* __V4L2_CAMERA_PROXY_H__ */
