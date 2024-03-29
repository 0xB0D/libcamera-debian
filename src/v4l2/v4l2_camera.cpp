/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * v4l2_camera.cpp - V4L2 compatibility camera
 */

#include "v4l2_camera.h"

#include <errno.h>
#include <unistd.h>

#include "libcamera/internal/log.h"

using namespace libcamera;

LOG_DECLARE_CATEGORY(V4L2Compat);

V4L2Camera::V4L2Camera(std::shared_ptr<Camera> camera)
	: camera_(camera), isRunning_(false), bufferAllocator_(nullptr),
	  efd_(-1), bufferAvailableCount_(0)
{
	camera_->requestCompleted.connect(this, &V4L2Camera::requestComplete);
}

V4L2Camera::~V4L2Camera()
{
	close();
}

int V4L2Camera::open()
{
	if (camera_->acquire() < 0) {
		LOG(V4L2Compat, Error) << "Failed to acquire camera";
		return -EINVAL;
	}

	config_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
	if (!config_) {
		camera_->release();
		return -EINVAL;
	}

	bufferAllocator_ = new FrameBufferAllocator(camera_);

	return 0;
}

void V4L2Camera::close()
{
	delete bufferAllocator_;
	bufferAllocator_ = nullptr;

	camera_->release();
}

void V4L2Camera::bind(int efd)
{
	efd_ = efd;
}

void V4L2Camera::unbind()
{
	efd_ = -1;
}

void V4L2Camera::getStreamConfig(StreamConfiguration *streamConfig)
{
	*streamConfig = config_->at(0);
}

std::vector<V4L2Camera::Buffer> V4L2Camera::completedBuffers()
{
	std::vector<Buffer> v;

	bufferLock_.lock();
	for (std::unique_ptr<Buffer> &metadata : completedBuffers_)
		v.push_back(*metadata.get());
	completedBuffers_.clear();
	bufferLock_.unlock();

	return v;
}

void V4L2Camera::requestComplete(Request *request)
{
	if (request->status() == Request::RequestCancelled)
		return;

	/* We only have one stream at the moment. */
	bufferLock_.lock();
	FrameBuffer *buffer = request->buffers().begin()->second;
	std::unique_ptr<Buffer> metadata =
		std::make_unique<Buffer>(request->cookie(), buffer->metadata());
	completedBuffers_.push_back(std::move(metadata));
	bufferLock_.unlock();

	uint64_t data = 1;
	int ret = ::write(efd_, &data, sizeof(data));
	if (ret != sizeof(data))
		LOG(V4L2Compat, Error) << "Failed to signal eventfd POLLIN";

	{
		MutexLocker locker(bufferMutex_);
		bufferAvailableCount_++;
	}
	bufferCV_.notify_all();
}

int V4L2Camera::configure(StreamConfiguration *streamConfigOut,
			  const Size &size, const PixelFormat &pixelformat,
			  unsigned int bufferCount)
{
	StreamConfiguration &streamConfig = config_->at(0);
	streamConfig.size.width = size.width;
	streamConfig.size.height = size.height;
	streamConfig.pixelFormat = pixelformat;
	streamConfig.bufferCount = bufferCount;
	/* \todo memoryType (interval vs external) */

	CameraConfiguration::Status validation = config_->validate();
	if (validation == CameraConfiguration::Invalid) {
		LOG(V4L2Compat, Debug) << "Configuration invalid";
		return -EINVAL;
	}
	if (validation == CameraConfiguration::Adjusted)
		LOG(V4L2Compat, Debug) << "Configuration adjusted";

	LOG(V4L2Compat, Debug) << "Validated configuration is: "
			      << streamConfig.toString();

	int ret = camera_->configure(config_.get());
	if (ret < 0)
		return ret;

	*streamConfigOut = config_->at(0);

	return 0;
}

int V4L2Camera::allocBuffers(unsigned int count)
{
	Stream *stream = *camera_->streams().begin();

	return bufferAllocator_->allocate(stream);
}

void V4L2Camera::freeBuffers()
{
	pendingRequests_.clear();

	Stream *stream = *camera_->streams().begin();
	bufferAllocator_->free(stream);
}

FileDescriptor V4L2Camera::getBufferFd(unsigned int index)
{
	Stream *stream = *camera_->streams().begin();
	const std::vector<std::unique_ptr<FrameBuffer>> &buffers =
		bufferAllocator_->buffers(stream);

	if (buffers.size() <= index)
		return FileDescriptor();

	return buffers[index]->planes()[0].fd;
}

int V4L2Camera::streamOn()
{
	if (isRunning_)
		return 0;

	int ret = camera_->start();
	if (ret < 0)
		return ret == -EACCES ? -EBUSY : ret;

	isRunning_ = true;

	for (std::unique_ptr<Request> &req : pendingRequests_) {
		/* \todo What should we do if this returns -EINVAL? */
		ret = camera_->queueRequest(req.release());
		if (ret < 0)
			return ret == -EACCES ? -EBUSY : ret;
	}

	pendingRequests_.clear();

	return 0;
}

int V4L2Camera::streamOff()
{
	if (!isRunning_)
		return 0;

	pendingRequests_.clear();

	int ret = camera_->stop();
	if (ret < 0)
		return ret == -EACCES ? -EBUSY : ret;

	{
		MutexLocker locker(bufferMutex_);
		isRunning_ = false;
	}
	bufferCV_.notify_all();

	return 0;
}

int V4L2Camera::qbuf(unsigned int index)
{
	std::unique_ptr<Request> request =
		std::unique_ptr<Request>(camera_->createRequest(index));
	if (!request) {
		LOG(V4L2Compat, Error) << "Can't create request";
		return -ENOMEM;
	}

	Stream *stream = config_->at(0).stream();
	FrameBuffer *buffer = bufferAllocator_->buffers(stream)[index].get();
	int ret = request->addBuffer(stream, buffer);
	if (ret < 0) {
		LOG(V4L2Compat, Error) << "Can't set buffer for request";
		return -ENOMEM;
	}

	if (!isRunning_) {
		pendingRequests_.push_back(std::move(request));
		return 0;
	}

	ret = camera_->queueRequest(request.release());
	if (ret < 0) {
		LOG(V4L2Compat, Error) << "Can't queue request";
		return ret == -EACCES ? -EBUSY : ret;
	}

	return 0;
}

void V4L2Camera::waitForBufferAvailable()
{
	MutexLocker locker(bufferMutex_);
	bufferCV_.wait(locker, [&] {
			       return bufferAvailableCount_ >= 1 || !isRunning_;
		       });
	if (isRunning_)
		bufferAvailableCount_--;
}

bool V4L2Camera::isBufferAvailable()
{
	MutexLocker locker(bufferMutex_);
	if (bufferAvailableCount_ < 1)
		return false;

	bufferAvailableCount_--;
	return true;
}

bool V4L2Camera::isRunning()
{
	return isRunning_;
}
