/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * stream.h - Video stream for a Camera
 */
#ifndef __LIBCAMERA_STREAM_H__
#define __LIBCAMERA_STREAM_H__

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <libcamera/buffer.h>
#include <libcamera/geometry.h>
#include <libcamera/pixel_format.h>

namespace libcamera {

class Camera;
class Stream;

class StreamFormats
{
public:
	StreamFormats();
	StreamFormats(const std::map<PixelFormat, std::vector<SizeRange>> &formats);

	std::vector<PixelFormat> pixelformats() const;
	std::vector<Size> sizes(const PixelFormat &pixelformat) const;

	SizeRange range(const PixelFormat &pixelformat) const;

private:
	std::map<PixelFormat, std::vector<SizeRange>> formats_;
};

struct StreamConfiguration {
	StreamConfiguration();
	StreamConfiguration(const StreamFormats &formats);

	PixelFormat pixelFormat;
	Size size;
	unsigned int stride;

	unsigned int bufferCount;

	Stream *stream() const { return stream_; }
	void setStream(Stream *stream) { stream_ = stream; }
	const StreamFormats &formats() const { return formats_; }

	std::string toString() const;

private:
	Stream *stream_;
	StreamFormats formats_;
};

enum StreamRole {
	StillCapture,
	StillCaptureRaw,
	VideoRecording,
	Viewfinder,
};

using StreamRoles = std::vector<StreamRole>;

class Stream
{
public:
	Stream();

	const StreamConfiguration &configuration() const { return configuration_; }

protected:
	friend class Camera;

	StreamConfiguration configuration_;
};

} /* namespace libcamera */

#endif /* __LIBCAMERA_STREAM_H__ */
