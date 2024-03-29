/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * dng_writer.cpp - DNG writer
 */

#include "dng_writer.h"

#include <algorithm>
#include <iostream>
#include <map>

#include <tiffio.h>

#include <libcamera/control_ids.h>
#include <libcamera/formats.h>

using namespace libcamera;

enum CFAPatternColour : uint8_t {
	CFAPatternRed = 0,
	CFAPatternGreen = 1,
	CFAPatternBlue = 2,
};

struct FormatInfo {
	uint8_t bitsPerSample;
	CFAPatternColour pattern[4];
	void (*packScanline)(void *output, const void *input,
			     unsigned int width);
	void (*thumbScanline)(const FormatInfo &info, void *output,
			      const void *input, unsigned int width,
			      unsigned int stride);
};

void packScanlineSBGGR10P(void *output, const void *input, unsigned int width)
{
	const uint8_t *in = static_cast<const uint8_t *>(input);
	uint8_t *out = static_cast<uint8_t *>(output);

	/* \todo Can this be made more efficient? */
	for (unsigned int x = 0; x < width; x += 4) {
		*out++ = in[0];
		*out++ = (in[4] & 0x03) << 6 | in[1] >> 2;
		*out++ = (in[1] & 0x03) << 6 | (in[4] & 0x0c) << 2 | in[2] >> 4;
		*out++ = (in[2] & 0x0f) << 4 | (in[4] & 0x30) >> 2 | in[3] >> 6;
		*out++ = (in[3] & 0x3f) << 2 | (in[4] & 0xc0) >> 6;
		in += 5;
	}
}

void packScanlineSBGGR12P(void *output, const void *input, unsigned int width)
{
	const uint8_t *in = static_cast<const uint8_t *>(input);
	uint8_t *out = static_cast<uint8_t *>(output);

	/* \todo Can this be made more efficient? */
	for (unsigned int i = 0; i < width; i += 2) {
		*out++ = in[0];
		*out++ = (in[2] & 0x0f) << 4 | in[1] >> 4;
		*out++ = (in[1] & 0x0f) << 4 | in[2] >> 4;
		in += 3;
	}
}

void thumbScanlineSBGGRxxP(const FormatInfo &info, void *output,
			   const void *input, unsigned int width,
			   unsigned int stride)
{
	const uint8_t *in = static_cast<const uint8_t *>(input);
	uint8_t *out = static_cast<uint8_t *>(output);

	/* Number of bytes corresponding to 16 pixels. */
	unsigned int skip = info.bitsPerSample * 16 / 8;

	for (unsigned int x = 0; x < width; x++) {
		uint8_t value = (in[0] + in[1] + in[stride] + in[stride + 1]) >> 2;
		*out++ = value;
		*out++ = value;
		*out++ = value;
		in += skip;
	}
}

void packScanlineIPU3(void *output, const void *input, unsigned int width)
{
	const uint8_t *in = static_cast<const uint8_t *>(input);
	uint16_t *out = static_cast<uint16_t *>(output);

	/*
	 * Upscale the 10-bit format to 16-bit as it's not trivial to pack it
	 * as 10-bit without gaps.
	 *
	 * \todo Improve packing to keep the 10-bit sample size.
	 */
	unsigned int x = 0;
	while (true) {
		for (unsigned int i = 0; i < 6; i++) {
			*out++ = (in[1] & 0x03) << 14 | (in[0] & 0xff) << 6;
			if (++x >= width)
				return;

			*out++ = (in[2] & 0x0f) << 12 | (in[1] & 0xfc) << 4;
			if (++x >= width)
				return;

			*out++ = (in[3] & 0x3f) << 10 | (in[2] & 0xf0) << 2;
			if (++x >= width)
				return;

			*out++ = (in[4] & 0xff) <<  8 | (in[3] & 0xc0) << 0;
			if (++x >= width)
				return;

			in += 5;
		}

		*out++ = (in[1] & 0x03) << 14 | (in[0] & 0xff) << 6;
		if (++x >= width)
			return;

		in += 2;
	}
}

void thumbScanlineIPU3(const FormatInfo &info, void *output,
		       const void *input, unsigned int width,
		       unsigned int stride)
{
	uint8_t *out = static_cast<uint8_t *>(output);

	for (unsigned int x = 0; x < width; x++) {
		unsigned int pixel = x * 16;
		unsigned int block = pixel / 25;
		unsigned int pixelInBlock = pixel - block * 25;

		/*
		 * If the pixel is the last in the block cheat a little and
		 * move one pixel backward to avoid reading between two blocks
		 * and having to deal with the padding bits.
		 */
		if (pixelInBlock == 24)
			pixelInBlock--;

		const uint8_t *in = static_cast<const uint8_t *>(input)
				  + block * 32 + (pixelInBlock / 4) * 5;

		uint16_t val1, val2, val3, val4;
		switch (pixelInBlock % 4) {
		case 0:
			val1 = (in[1] & 0x03) << 14 | (in[0] & 0xff) << 6;
			val2 = (in[2] & 0x0f) << 12 | (in[1] & 0xfc) << 4;
			val3 = (in[stride + 1] & 0x03) << 14 | (in[stride + 0] & 0xff) << 6;
			val4 = (in[stride + 2] & 0x0f) << 12 | (in[stride + 1] & 0xfc) << 4;
			break;
		case 1:
			val1 = (in[2] & 0x0f) << 12 | (in[1] & 0xfc) << 4;
			val2 = (in[3] & 0x3f) << 10 | (in[2] & 0xf0) << 2;
			val3 = (in[stride + 2] & 0x0f) << 12 | (in[stride + 1] & 0xfc) << 4;
			val4 = (in[stride + 3] & 0x3f) << 10 | (in[stride + 2] & 0xf0) << 2;
			break;
		case 2:
			val1 = (in[3] & 0x3f) << 10 | (in[2] & 0xf0) << 2;
			val2 = (in[4] & 0xff) <<  8 | (in[3] & 0xc0) << 0;
			val3 = (in[stride + 3] & 0x3f) << 10 | (in[stride + 2] & 0xf0) << 2;
			val4 = (in[stride + 4] & 0xff) <<  8 | (in[stride + 3] & 0xc0) << 0;
			break;
		case 3:
			val1 = (in[4] & 0xff) <<  8 | (in[3] & 0xc0) << 0;
			val2 = (in[6] & 0x03) << 14 | (in[5] & 0xff) << 6;
			val3 = (in[stride + 4] & 0xff) <<  8 | (in[stride + 3] & 0xc0) << 0;
			val4 = (in[stride + 6] & 0x03) << 14 | (in[stride + 5] & 0xff) << 6;
			break;
		}

		uint8_t value = (val1 + val2 + val3 + val4) >> 10;
		*out++ = value;
		*out++ = value;
		*out++ = value;
	}
}

static const std::map<PixelFormat, FormatInfo> formatInfo = {
	{ formats::SBGGR10_CSI2P, {
		.bitsPerSample = 10,
		.pattern = { CFAPatternBlue, CFAPatternGreen, CFAPatternGreen, CFAPatternRed },
		.packScanline = packScanlineSBGGR10P,
		.thumbScanline = thumbScanlineSBGGRxxP,
	} },
	{ formats::SGBRG10_CSI2P, {
		.bitsPerSample = 10,
		.pattern = { CFAPatternGreen, CFAPatternBlue, CFAPatternRed, CFAPatternGreen },
		.packScanline = packScanlineSBGGR10P,
		.thumbScanline = thumbScanlineSBGGRxxP,
	} },
	{ formats::SGRBG10_CSI2P, {
		.bitsPerSample = 10,
		.pattern = { CFAPatternGreen, CFAPatternRed, CFAPatternBlue, CFAPatternGreen },
		.packScanline = packScanlineSBGGR10P,
		.thumbScanline = thumbScanlineSBGGRxxP,
	} },
	{ formats::SRGGB10_CSI2P, {
		.bitsPerSample = 10,
		.pattern = { CFAPatternRed, CFAPatternGreen, CFAPatternGreen, CFAPatternBlue },
		.packScanline = packScanlineSBGGR10P,
		.thumbScanline = thumbScanlineSBGGRxxP,
	} },
	{ formats::SBGGR12_CSI2P, {
		.bitsPerSample = 12,
		.pattern = { CFAPatternBlue, CFAPatternGreen, CFAPatternGreen, CFAPatternRed },
		.packScanline = packScanlineSBGGR12P,
		.thumbScanline = thumbScanlineSBGGRxxP,
	} },
	{ formats::SGBRG12_CSI2P, {
		.bitsPerSample = 12,
		.pattern = { CFAPatternGreen, CFAPatternBlue, CFAPatternRed, CFAPatternGreen },
		.packScanline = packScanlineSBGGR12P,
		.thumbScanline = thumbScanlineSBGGRxxP,
	} },
	{ formats::SGRBG12_CSI2P, {
		.bitsPerSample = 12,
		.pattern = { CFAPatternGreen, CFAPatternRed, CFAPatternBlue, CFAPatternGreen },
		.packScanline = packScanlineSBGGR12P,
		.thumbScanline = thumbScanlineSBGGRxxP,
	} },
	{ formats::SRGGB12_CSI2P, {
		.bitsPerSample = 12,
		.pattern = { CFAPatternRed, CFAPatternGreen, CFAPatternGreen, CFAPatternBlue },
		.packScanline = packScanlineSBGGR12P,
		.thumbScanline = thumbScanlineSBGGRxxP,
	} },
	{ formats::SBGGR10_IPU3, {
		.bitsPerSample = 16,
		.pattern = { CFAPatternBlue, CFAPatternGreen, CFAPatternGreen, CFAPatternRed },
		.packScanline = packScanlineIPU3,
		.thumbScanline = thumbScanlineIPU3,
	} },
	{ formats::SGBRG10_IPU3, {
		.bitsPerSample = 16,
		.pattern = { CFAPatternGreen, CFAPatternBlue, CFAPatternRed, CFAPatternGreen },
		.packScanline = packScanlineIPU3,
		.thumbScanline = thumbScanlineIPU3,
	} },
	{ formats::SGRBG10_IPU3, {
		.bitsPerSample = 16,
		.pattern = { CFAPatternGreen, CFAPatternRed, CFAPatternBlue, CFAPatternGreen },
		.packScanline = packScanlineIPU3,
		.thumbScanline = thumbScanlineIPU3,
	} },
	{ formats::SRGGB10_IPU3, {
		.bitsPerSample = 16,
		.pattern = { CFAPatternRed, CFAPatternGreen, CFAPatternGreen, CFAPatternBlue },
		.packScanline = packScanlineIPU3,
		.thumbScanline = thumbScanlineIPU3,
	} },
};

int DNGWriter::write(const char *filename, const Camera *camera,
		     const StreamConfiguration &config,
		     const ControlList &metadata,
		     const FrameBuffer *buffer, const void *data)
{
	const auto it = formatInfo.find(config.pixelFormat);
	if (it == formatInfo.cend()) {
		std::cerr << "Unsupported pixel format" << std::endl;
		return -EINVAL;
	}
	const FormatInfo *info = &it->second;

	TIFF *tif = TIFFOpen(filename, "w");
	if (!tif) {
		std::cerr << "Failed to open tiff file" << std::endl;
		return -EINVAL;
	}

	/*
	 * Scanline buffer, has to be large enough to store both a RAW scanline
	 * or a thumbnail scanline. The latter will always be much smaller than
	 * the former as we downscale by 16 in both directions.
	 */
	uint8_t scanline[(config.size.width * info->bitsPerSample + 7) / 8];

	toff_t rawIFDOffset = 0;
	toff_t exifIFDOffset = 0;

	/*
	 * Start with a thumbnail in IFD 0 for compatibility with TIFF baseline
	 * readers, as required by the TIFF/EP specification. Tags that apply to
	 * the whole file are stored here.
	 */
	const uint8_t version[] = { 1, 2, 0, 0 };

	TIFFSetField(tif, TIFFTAG_DNGVERSION, version);
	TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, version);
	TIFFSetField(tif, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
	TIFFSetField(tif, TIFFTAG_MAKE, "libcamera");
	TIFFSetField(tif, TIFFTAG_MODEL, camera->name().c_str());
	TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, camera->name().c_str());
	TIFFSetField(tif, TIFFTAG_SOFTWARE, "qcam");
	TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);

	/*
	 * Thumbnail-specific tags. The thumbnail is stored as an RGB image
	 * with 1/16 of the raw image resolution. Greyscale would save space,
	 * but doesn't seem well supported by RawTherapee.
	 */
	TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, config.size.width / 16);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, config.size.height / 16);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
	TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);

	/*
	 * Reserve space for the SubIFD and ExifIFD tags, pointing to the IFD
	 * for the raw image and EXIF data respectively. The real offsets will
	 * be set later.
	 */
	TIFFSetField(tif, TIFFTAG_SUBIFD, 1, &rawIFDOffset);
	TIFFSetField(tif, TIFFTAG_EXIFIFD, exifIFDOffset);

	/* Write the thumbnail. */
	const uint8_t *row = static_cast<const uint8_t *>(data);
	for (unsigned int y = 0; y < config.size.height / 16; y++) {
		info->thumbScanline(*info, &scanline, row,
				    config.size.width / 16, config.stride);

		if (TIFFWriteScanline(tif, &scanline, y, 0) != 1) {
			std::cerr << "Failed to write thumbnail scanline"
				  << std::endl;
			TIFFClose(tif);
			return -EINVAL;
		}

		row += config.stride * 16;
	}

	TIFFWriteDirectory(tif);

	/* Create a new IFD for the RAW image. */
	const uint16_t cfaRepeatPatternDim[] = { 2, 2 };
	const uint8_t cfaPlaneColor[] = {
		CFAPatternRed,
		CFAPatternGreen,
		CFAPatternBlue
	};

	TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, config.size.width);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, config.size.height);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, info->bitsPerSample);
	TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
	TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfaRepeatPatternDim);
	TIFFSetField(tif, TIFFTAG_CFAPATTERN, info->pattern);
	TIFFSetField(tif, TIFFTAG_CFAPLANECOLOR, 3, cfaPlaneColor);
	TIFFSetField(tif, TIFFTAG_CFALAYOUT, 1);

	const uint16_t blackLevelRepeatDim[] = { 2, 2 };
	float blackLevel[] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t whiteLevel = (1 << info->bitsPerSample) - 1;

	if (metadata.contains(controls::SensorBlackLevels)) {
		Span<const int32_t> levels = metadata.get(controls::SensorBlackLevels);

		/*
		 * The black levels control is specified in R, Gr, Gb, B order.
		 * Map it to the TIFF tag that is specified in CFA pattern
		 * order.
		 */
		unsigned int green = (info->pattern[0] == CFAPatternRed ||
				      info->pattern[1] == CFAPatternRed)
				   ? 0 : 1;

		for (unsigned int i = 0; i < 4; ++i) {
			unsigned int level;

			switch (info->pattern[i]) {
			case CFAPatternRed:
				level = levels[0];
				break;
			case CFAPatternGreen:
				level = levels[green + 1];
				green = (green + 1) % 2;
				break;
			case CFAPatternBlue:
			default:
				level = levels[3];
				break;
			}

			/* Map the 16-bit value to the bits per sample range. */
			blackLevel[i] = level >> (16 - info->bitsPerSample);
		}
	}

	TIFFSetField(tif, TIFFTAG_BLACKLEVELREPEATDIM, &blackLevelRepeatDim);
	TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 4, &blackLevel);
	TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &whiteLevel);

	/* Write RAW content. */
	row = static_cast<const uint8_t *>(data);
	for (unsigned int y = 0; y < config.size.height; y++) {
		info->packScanline(&scanline, row, config.size.width);

		if (TIFFWriteScanline(tif, &scanline, y, 0) != 1) {
			std::cerr << "Failed to write RAW scanline"
				  << std::endl;
			TIFFClose(tif);
			return -EINVAL;
		}

		row += config.stride;
	}

	/* Checkpoint the IFD to retrieve its offset, and write it out. */
	TIFFCheckpointDirectory(tif);
	rawIFDOffset = TIFFCurrentDirOffset(tif);
	TIFFWriteDirectory(tif);

	/* Create a new IFD for the EXIF data and fill it. */
	TIFFCreateEXIFDirectory(tif);

	/* Store creation time. */
	time_t rawtime;
	struct tm *timeinfo;
	char strTime[20];

	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(strTime, 20, "%Y:%m:%d %H:%M:%S", timeinfo);

	/*
	 * \todo Handle timezone information by setting OffsetTimeOriginal and
	 * OffsetTimeDigitized once libtiff catches up to the specification and
	 * has EXIFTAG_ defines to handle them.
	 */
	TIFFSetField(tif, EXIFTAG_DATETIMEORIGINAL, strTime);
	TIFFSetField(tif, EXIFTAG_DATETIMEDIGITIZED, strTime);

	if (metadata.contains(controls::AnalogueGain)) {
		float gain = metadata.get(controls::AnalogueGain);
		uint16_t iso = std::min(std::max(gain * 100, 0.0f), 65535.0f);
		TIFFSetField(tif, EXIFTAG_ISOSPEEDRATINGS, 1, &iso);
	}

	if (metadata.contains(controls::ExposureTime)) {
		float exposureTime = metadata.get(controls::ExposureTime) / 1e6;
		TIFFSetField(tif, EXIFTAG_EXPOSURETIME, exposureTime);
	}

	TIFFWriteCustomDirectory(tif, &exifIFDOffset);

	/* Update the IFD offsets and close the file. */
	TIFFSetDirectory(tif, 0);
	TIFFSetField(tif, TIFFTAG_SUBIFD, 1, &rawIFDOffset);
	TIFFSetField(tif, TIFFTAG_EXIFIFD, exifIFDOffset);
	TIFFWriteDirectory(tif);

	TIFFClose(tif);

	return 0;
}
