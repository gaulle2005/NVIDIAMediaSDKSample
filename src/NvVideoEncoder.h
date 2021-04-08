/*
 * NvVideoEncoder.h
 *
 *  Created on: Jan 15, 2020
 *      Author: jason
 */

#ifndef SRC_MEDIA_NVIDIA_NVVIDEOENCODER_H_
#define SRC_MEDIA_NVIDIA_NVVIDEOENCODER_H_

#include <queue>

#include "NvEncodeAPI.h"
#include "dynlink_nvcuvid.h"
#include "MediaDef.h"

class NvVideoEncoder{
public:
	NvVideoEncoder();
	~NvVideoEncoder();
	bool Start(VideoParam & param,VideoBitstreamCB cb,void * user_data);
	bool InputData(VideoRawData & data);
	bool Stop();
private:
	void OutputFrame(NV_ENC_LOCK_BITSTREAM lockBitstreamData);
private:
	NVEncoderAPI *m_nvencoder_api = nullptr;
	uint32_t m_encoder_buffer_count = 0;
	void* m_cuda_device = nullptr;
	EncodeBuffer m_encoder_buffer[MAX_ENCODE_QUEUE];
	CNvQueue<EncodeBuffer> m_encoder_buffer_queue;
	EncodeConfig m_encode_config;
	std::queue<int64_t> m_ptsqueue;
	CUvideoctxlock m_ctx_lock = nullptr;
	bool m_inited = false;
	VideoBitstreamCB m_cb = nullptr;
	void * m_user_data = nullptr;
private:
	NVENCSTATUS Deinitialize();
	NVENCSTATUS EncodeFrame(EncodeFrameConfig * frame);
	NVENCSTATUS InitCuda(uint32_t device_id=0);
	NVENCSTATUS AllocateIOBuffers(uint32_t width, uint32_t height, NV_ENC_BUFFER_FORMAT bufefr_fmt);
	NVENCSTATUS ReleaseIOBuffers();
	NVENCSTATUS FlushEncoder();
};

// NVEncodeAPI entry point
typedef NVENCSTATUS (NVENCAPI *MYPROC)(NV_ENCODE_API_FUNCTION_LIST*);

#endif /* SRC_MEDIA_NVIDIA_NVVIDEOENCODER_H_ */
