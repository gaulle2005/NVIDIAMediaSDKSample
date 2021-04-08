#include <string.h>
#include <assert.h>
#include "NvVideoDecoder.h"

template <class T>
void TransferToYUV(const T *psrc,
	T *pdst_Y, T *pdst_U, T *pdst_V,
	int width, int height, int pitch,
	int bit_depth_minus8 = 0) {
	int x, y, width_2, height_2;
	const T *py = psrc;
	const T *puv = psrc + height*pitch / sizeof(T);
	int rsh = bit_depth_minus8 ? 8 - bit_depth_minus8 : 0;
	// luma
	for (y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			pdst_Y[y*width + x] = py[x] >> rsh;
		}
		py += pitch / sizeof(T);
	}
	// De-interleave chroma
	width_2 = width >> 1;
	height_2 = height >> 1;
	for (y = 0; y < height_2; y++) {
		for (x = 0; x < width_2; x++) {
			pdst_U[y*width_2 + x] = puv[x * 2] >> rsh;
			pdst_V[y*width_2 + x] = puv[x * 2 + 1] >> rsh;
		}
		puv += pitch / sizeof(T);
	}
}



static bool IsDecoderFitting(CUVIDDECODECREATEINFO& create_info, CUVIDEOFORMAT* format) {
	return format->codec == create_info.CodecType &&
			format->coded_width == create_info.ulWidth &&
			format->coded_height == create_info.ulHeight &&
			format->chroma_format == create_info.ChromaFormat &&
			format->bit_depth_chroma_minus8 == create_info.bitDepthMinus8 &&
		(unsigned long)(format->display_area.right - format->display_area.left) == create_info.ulTargetWidth &&
		(unsigned long)(format->display_area.bottom - format->display_area.top) == create_info.ulTargetHeight &&
		(format->bit_depth_chroma_minus8 ? cudaVideoSurfaceFormat_P016 : cudaVideoSurfaceFormat_NV12) == create_info.OutputFormat;
}

int CUDAAPI NvVideoDecoder::HandleVideoSequence(void* user_data, CUVIDEOFORMAT* format) {
	NvVideoDecoder* obj = (NvVideoDecoder*)user_data;

	if (IsDecoderFitting(obj->m_vide_decoder_create_info, format)) {
		return 0;
	}

	memset(&obj->m_vide_decoder_create_info, 0, sizeof(CUVIDDECODECREATEINFO));
	if (obj->m_video_decoder) {
		cuvidDestroyDecoder(obj->m_video_decoder);
	}

	obj->m_vide_decoder_create_info.CodecType = format->codec;
	obj->m_vide_decoder_create_info.ulWidth = format->coded_width;
	obj->m_vide_decoder_create_info.ulHeight = format->coded_height;
	obj->m_vide_decoder_create_info.ulNumDecodeSurfaces = 8;
	if ((obj->m_vide_decoder_create_info.CodecType == cudaVideoCodec_H264) ||
		(obj->m_vide_decoder_create_info.CodecType == cudaVideoCodec_H264_SVC) ||
		(obj->m_vide_decoder_create_info.CodecType == cudaVideoCodec_H264_MVC)) {
		//assume worst-case of 20 decode surfaces for H264
		obj->m_vide_decoder_create_info.ulNumDecodeSurfaces = 20;
	}
	if (obj->m_vide_decoder_create_info.CodecType == cudaVideoCodec_VP9)
		obj->m_vide_decoder_create_info.ulNumDecodeSurfaces = 12;
	if (obj->m_vide_decoder_create_info.CodecType == cudaVideoCodec_HEVC) {
		//ref HEVC spec: A.4.1 General tier and level limits
		int max_luma_ps = 35651584; // currently assuming level 6.2, 8Kx4K
		int max_dpb_pic_buf = 6;
		int pic_size_in_samples_y = obj->m_vide_decoder_create_info.ulWidth * obj->m_vide_decoder_create_info.ulHeight;
		int max_dpb_size = 0;
		if (pic_size_in_samples_y <= (max_luma_ps >> 2))
			max_dpb_size = max_dpb_pic_buf * 4;
		else if (pic_size_in_samples_y <= (max_luma_ps >> 1))
			max_dpb_size = max_dpb_pic_buf * 2;
		else if (pic_size_in_samples_y <= ((3 * max_luma_ps) >> 2))
			max_dpb_size = (max_dpb_pic_buf * 4) / 3;
		else
			max_dpb_size = max_dpb_pic_buf;
		max_dpb_size = max_dpb_size < 16 ? max_dpb_size : 16;
		obj->m_vide_decoder_create_info.ulNumDecodeSurfaces = max_dpb_size + 4;
	}
	obj->m_vide_decoder_create_info.ChromaFormat = format->chroma_format;
	obj->m_vide_decoder_create_info.OutputFormat = format->bit_depth_chroma_minus8 ? cudaVideoSurfaceFormat_P016 : cudaVideoSurfaceFormat_NV12;
	obj->m_vide_decoder_create_info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
	obj->m_vide_decoder_create_info.bitDepthMinus8 = format->bit_depth_chroma_minus8;
	obj->m_vide_decoder_create_info.ulTargetWidth = format->display_area.right - format->display_area.left;
	obj->m_vide_decoder_create_info.ulTargetHeight = format->display_area.bottom - format->display_area.top;
	obj->m_vide_decoder_create_info.display_area.left = 0;
	obj->m_vide_decoder_create_info.display_area.right = (short)obj->m_vide_decoder_create_info.ulTargetWidth;
	obj->m_vide_decoder_create_info.display_area.top = 0;
	obj->m_vide_decoder_create_info.display_area.bottom = (short)obj->m_vide_decoder_create_info.ulTargetHeight;
	obj->m_vide_decoder_create_info.ulNumOutputSurfaces = 2;
	obj->m_vide_decoder_create_info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
	obj->m_vide_decoder_create_info.vidLock = obj->m_ctx_lock;

	CUresult cu_result = cuvidCreateDecoder(&obj->m_video_decoder, &obj->m_vide_decoder_create_info);
	if (cu_result != CUDA_SUCCESS) {
		return -1;
	}

	obj->m_frame_queue->init(obj->m_vide_decoder_create_info.ulTargetWidth, obj->m_vide_decoder_create_info.ulTargetHeight);

	return 1;
}

int CUDAAPI NvVideoDecoder::HandlePictureDecode(void* user_data, CUVIDPICPARAMS* pic_params) {
	if(!user_data)
		return -1;
	NvVideoDecoder* obj = (NvVideoDecoder*)user_data;
	obj->m_frame_queue->waitUntilFrameAvailable(pic_params->CurrPicIdx);
	CUresult cu_result = cuvidDecodePicture(obj->m_video_decoder, pic_params);
	if(cu_result != CUDA_SUCCESS)
		return -1;
	return 1;
}

int CUDAAPI NvVideoDecoder::HandlePictureDisplay(void* user_data, CUVIDPARSERDISPINFO* disp_params) {
	if(!user_data)
		return -1;
	NvVideoDecoder* obj = (NvVideoDecoder*)user_data;
	obj->OutputVideoFrame();
	obj->m_frame_queue->enqueue(disp_params);
	return 1;
}

NvVideoDecoder::~NvVideoDecoder() {
	if (m_frame_queue)
		m_frame_queue->endDecode();
	if (m_video_decoder)
		cuvidDestroyDecoder(m_video_decoder);
	if (m_video_parser)
		cuvidDestroyVideoParser(m_video_parser);
	if(m_ctx_lock)
		cuvidCtxLockDestroy(m_ctx_lock);
	if(m_current_ctx)
		cuCtxDestroy(m_current_ctx);

	if (m_frame_queue){
		delete m_frame_queue;
		m_frame_queue = nullptr;
	}

	for(int i=0;i<4;i++){
		if(m_gpu_buffer[i])
			cuMemFreeHost(m_gpu_buffer[i]);
	}
}

bool NvVideoDecoder::Start(VideoCodec codec,VideoFrameCB cb,void * user_data,bool download_gpu_buffer){
	CUresult cu_result = CUDA_SUCCESS;
	cu_result = cuInit(0, __CUDA_API_VERSION, nullptr);
	if(cu_result != CUDA_SUCCESS)
		return false;
	cu_result = cuvidInit();
	if(cu_result != CUDA_SUCCESS)
		return false;
	CUdevice device;
	int bestdevice = gpuGetMaxGflopsDeviceIdDRV();
	cu_result = cuDeviceGet(&device, bestdevice);
	if(cu_result != CUDA_SUCCESS)
		return false;
	cu_result = cuCtxCreate(&m_current_ctx, CU_CTX_SCHED_AUTO, device);
	if(cu_result != CUDA_SUCCESS)
		return false;
	cu_result = cuvidCtxLockCreate(&m_ctx_lock, m_current_ctx);
	if(cu_result != CUDA_SUCCESS)
		return false;
	cu_result = cuCtxPushCurrent(m_current_ctx);
	if(cu_result != CUDA_SUCCESS)
		return false;

	if(!m_frame_queue)
		m_frame_queue = new CUVIDFrameQueue(m_ctx_lock);

	if (!m_video_parser){

		CUVIDPARSERPARAMS video_parser_params;
		memset(&video_parser_params, 0, sizeof(CUVIDPARSERPARAMS));

		if(codec == VideoCodec::H264)
			video_parser_params.CodecType = cudaVideoCodec_H264;
		else if(codec == VideoCodec::HEVC)
			video_parser_params.CodecType = cudaVideoCodec_HEVC;
		else
			return false;
		video_parser_params.ulMaxNumDecodeSurfaces = 10;
		video_parser_params.ulMaxDisplayDelay = 1;
		video_parser_params.pUserData = this;
		video_parser_params.pfnSequenceCallback = HandleVideoSequence;
		video_parser_params.pfnDecodePicture = HandlePictureDecode;
		video_parser_params.pfnDisplayPicture = HandlePictureDisplay;

		cu_result = cuvidCreateVideoParser(&m_video_parser, &video_parser_params);
		if (cu_result != CUDA_SUCCESS) {
			return false;
		}
	}

	while(!m_ptsqueue.empty()){
		m_ptsqueue.pop();
	}

	m_frame_cb = cb;
	m_user_data = user_data;
	m_download_gpu_buffer = download_gpu_buffer;

	return true;
}


int NvVideoDecoder::InputData(MediaDataBitStream & bs){
	if(!m_video_parser)
		return false;

	CUVIDSOURCEDATAPACKET packet;
	packet.payload = bs.buffer;
	packet.payload_size = bs.buffer_len;
	packet.flags = CUVID_PKT_TIMESTAMP;
	packet.timestamp = 0;
	m_ptsqueue.push(bs.pts);
	if (packet.payload_size != 0 && packet.payload != nullptr) {
		cuvidParseVideoData(m_video_parser, &packet);
	}

	return true;
}
bool NvVideoDecoder::Stop(){
	if(!m_video_parser || !m_frame_queue)
		return false;
	CUVIDSOURCEDATAPACKET packet;
	packet.payload = 0;
	packet.payload_size = 0;
	packet.flags = CUVID_PKT_ENDOFSTREAM;
	packet.timestamp = 0;
	cuvidParseVideoData(m_video_parser, &packet);
	while (!m_frame_queue->isEmpty()) {
		OutputVideoFrame();
	}
	m_frame_queue->endDecode();
	return true;
}


int NvVideoDecoder::OutputVideoFrame() {
	CUdeviceptr  device_ptr;
	unsigned int pic_pitch = 0;
	CUVIDPROCPARAMS video_processing_params;
	memset(&video_processing_params, 0, sizeof(CUVIDPROCPARAMS));

	if (!(m_frame_queue->isEndOfDecode() && m_frame_queue->isEmpty())) {
		CUVIDPARSERDISPINFO pic_info;
		if (m_frame_queue->dequeue(&pic_info)) {
			CCtxAutoLock lock(m_ctx_lock);
			video_processing_params.progressive_frame = pic_info.progressive_frame;
			video_processing_params.top_field_first = pic_info.top_field_first;
			video_processing_params.unpaired_field = (pic_info.repeat_first_field < 0);
			video_processing_params.second_field = 0;

			cuvidMapVideoFrame(m_video_decoder,pic_info.picture_index,&device_ptr,&pic_pitch, &video_processing_params);

			int bit_depth_minus8 = m_vide_decoder_create_info.bitDepthMinus8;
			int width = m_vide_decoder_create_info.ulTargetWidth;
			int height = m_vide_decoder_create_info.ulTargetHeight;

			VideoRawData data;
			data.width = width;
			data.height = height;
			data.deviceptr = device_ptr;

			if(m_frame_cb){
				if(m_download_gpu_buffer){
					int factor = bit_depth_minus8 ? 2 : 1;
					int frame_size = pic_pitch * height * 3 / 2;
					if(!m_gpu_buffer[0] || m_frame_size != frame_size){
						for(int i=0;i<4;i++){
							if(m_gpu_buffer[i])
								cuMemFreeHost(m_gpu_buffer[i]);
						}

						m_frame_size = frame_size;
						cuMemAllocHost((void **)&m_gpu_buffer[0], m_frame_size);
						cuMemAllocHost((void **)&m_gpu_buffer[1], factor*(width * height));
						cuMemAllocHost((void **)&m_gpu_buffer[2], factor*(width * height / 4));
						cuMemAllocHost((void **)&m_gpu_buffer[3], factor*(width * height / 4));

						for(int i=0;i<4;i++){
							if(!m_gpu_buffer[i])
								return -1;
						}
					}
					cuMemcpyDtoH(m_gpu_buffer[0], device_ptr, frame_size);

					if (bit_depth_minus8 == 0) {
						TransferToYUV(m_gpu_buffer[0], m_gpu_buffer[1], m_gpu_buffer[2], m_gpu_buffer[3], width, height, pic_pitch, bit_depth_minus8);
					}
					else {
						TransferToYUV((unsigned short *)m_gpu_buffer[0], (unsigned short *)m_gpu_buffer[1], (unsigned short *)m_gpu_buffer[2], (unsigned short *)m_gpu_buffer[3],
								width, height, pic_pitch, bit_depth_minus8);
					}
					data.fmt = VideoBaseBandFmt::YUV420P;
					data.buffer[0] = m_gpu_buffer[1];
					data.buffer[1] = m_gpu_buffer[2];
					data.buffer[2] = m_gpu_buffer[3];
					data.line_size[0] = width * factor;
					data.line_size[1] = width * factor / 2;
					data.line_size[2] =  width * factor / 2;
				}else{
					data.line_size[0] = pic_pitch;
					data.line_size[1] = pic_pitch / 2;
					data.line_size[2] =  pic_pitch / 2;
					data.fmt = VideoBaseBandFmt::NV12;
				}

				if(!m_ptsqueue.empty()){
					data.pts = m_ptsqueue.front();
					m_ptsqueue.pop();
				}else
					data.pts = 0;
				m_frame_cb(data,m_user_data);
			}

			cuvidUnmapVideoFrame(m_video_decoder, device_ptr);
			m_frame_queue->releaseFrame(&pic_info);
		}

	}
	return 0;
}


