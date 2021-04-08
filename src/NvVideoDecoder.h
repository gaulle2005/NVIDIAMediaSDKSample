#ifndef NV_DECODER_H
#define NV_DECODER_H

#include <queue>

#include "helper_functions.h"
#include "helper_cuda_drvapi.h"
#include "dynlink_nvcuvid.h"
#include "dynlink_cuda.h"
#include "FrameQueue.h"
#include "MediaDef.h"


class NvVideoDecoder {
public:
	NvVideoDecoder() = default;
    ~NvVideoDecoder();
    bool Start(VideoCodec codec,VideoFrameCB cb,void * user_data,bool download_gpu_buffer = true);
	int InputData(MediaDataBitStream & bs);
	bool Stop();
private:
	int OutputVideoFrame();
private:
	static int CUDAAPI HandleVideoSequence(void* user_data, CUVIDEOFORMAT* format);
	static int CUDAAPI HandlePictureDisplay(void* user_data, CUVIDPARSERDISPINFO* pic_params);
	static int CUDAAPI HandlePictureDecode(void* user_data, CUVIDPICPARAMS* pic_params);
private:
    CUvideoparser  m_video_parser = nullptr;
    CUvideodecoder m_video_decoder = nullptr;
    CUvideoctxlock m_ctx_lock = nullptr;
	CUcontext	   m_current_ctx = nullptr;
	CUVIDDECODECREATEINFO m_vide_decoder_create_info;
	FrameQueue*    m_frame_queue = nullptr;
	std::queue<int64_t> m_ptsqueue;
	unsigned char  *m_gpu_buffer[4] = {nullptr};
	int m_frame_size = 0;
	VideoFrameCB m_frame_cb = nullptr;
	void * m_user_data = nullptr;
	bool m_download_gpu_buffer = true;
};

#endif
