/*
 * MediaDef.h
 *
 *  Created on: Apr 7, 2021
 *      Author: jason
 */

#ifndef SRC_MEDIADEF_H_
#define SRC_MEDIADEF_H_

#include <stdint.h>

enum class VideoBaseBandFmt{
	NONE,
	YUV420P,
	BGR,
	NV12
};

enum class VideoCodec {
	NONE,
	H264,
	HEVC
};

struct VideoParam{
	VideoBaseBandFmt pix_fmt = VideoBaseBandFmt::NONE;
	VideoCodec codec = VideoCodec::NONE;
	int width = 0;
	int height = 0;
	int frame_rate_num = 0;
	int frame_rate_den = 0;
	int gop_size = 0;
	int b_frames = 0;
	int bit_rate = 0;
};

struct MediaDataBitStream{
	unsigned char * buffer = nullptr;
	int buffer_len = 0;
	int64_t pts = 0;
	int64_t dts = 0;
	bool is_key = false;
};

struct VideoRawData{
	int width = 0;
	int height = 0;
	int line_size[3] = {0};
	VideoBaseBandFmt fmt = VideoBaseBandFmt::NONE;
	int64_t pts = 0;
	unsigned char * buffer[3] = {0};
	int bit_depth = 8;
	unsigned long long deviceptr = 0;
};

typedef void(*VideoFrameCB)(VideoRawData & data, void * user_data);
typedef void(*VideoBitstreamCB)(MediaDataBitStream & data, void * user_data);


#endif /* SRC_MEDIADEF_H_ */
