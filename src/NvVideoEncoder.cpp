/*
 * NvVideoEncoder.cpp
 *
 *  Created on: Jan 15, 2020
 *      Author: jason
 */

#include "NvVideoEncoder.h"

#define BITSTREAM_BUFFER_SIZE 2 * 1024 * 1024

NvVideoEncoder::NvVideoEncoder() {
	// TODO Auto-generated constructor stub
	for(int i=0;i<MAX_ENCODE_QUEUE;i++){
		memset(&m_encoder_buffer[i],0,sizeof(EncodeBuffer));
	}
}

NvVideoEncoder::~NvVideoEncoder() {
	// TODO Auto-generated destructor stub
	Stop();
}

bool NvVideoEncoder::Start(VideoParam & param,VideoBitstreamCB cb,void * user_data){
	if(m_inited)
		return true;

	NVENCSTATUS nv_status = NV_ENC_SUCCESS;
	memset(&m_encode_config, 0, sizeof(EncodeConfig));
	m_encode_config.endFrameIdx = INT_MAX;
	m_encode_config.deviceType = NV_ENC_CUDA;
	m_encode_config.presetGUID = NV_ENC_PRESET_DEFAULT_GUID;
	m_encode_config.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	m_encode_config.inputFormat = NV_ENC_BUFFER_FORMAT_NV12;
	m_encode_config.rcMode = NV_ENC_PARAMS_RC_VBR;
#ifdef TENBITSUPPORT
	if (pEncParam->bitdepth == 10) {
		m_encodeConfig.inputFormat = NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
	}
#endif

	if(param.codec == VideoCodec::HEVC){
		m_encode_config.codec = NV_ENC_HEVC;
	}else if(param.codec == VideoCodec::H264){
		m_encode_config.codec = NV_ENC_H264;
	}else
		return false;

	m_encode_config.frame_rate_den = param.frame_rate_den;
	m_encode_config.frame_rate_num = param.frame_rate_num;
	m_encode_config.bitrate = param.bit_rate;
	m_encode_config.gopLength = param.gop_size;
	m_encode_config.width = param.width;
	m_encode_config.height = param.height;
	m_encode_config.numB = 0;
	m_encode_config.vbvSize = param.bit_rate;
	m_encode_config.vbvMaxBitrate = param.bit_rate;
	m_encode_config.refnum = 2;

	nv_status = InitCuda(m_encode_config.deviceID);

	if (nv_status != NV_ENC_SUCCESS)
		return false;

	if(!m_nvencoder_api)
		m_nvencoder_api = new NVEncoderAPI();

	nv_status = m_nvencoder_api->Initialize(m_cuda_device, NV_ENC_DEVICE_TYPE_CUDA);

	if (nv_status != NV_ENC_SUCCESS)
		return false;

	m_encode_config.presetGUID = m_nvencoder_api->GetPresetGUID(m_encode_config.encoderPreset, m_encode_config.codec);

	nv_status = m_nvencoder_api->CreateEncoder(&m_encode_config);
	if (nv_status != NV_ENC_SUCCESS)
		return false;

	m_encoder_buffer_count = 10;

	nv_status = AllocateIOBuffers(m_encode_config.width, m_encode_config.height, m_encode_config.inputFormat);

	if (nv_status != NV_ENC_SUCCESS)
		return false;

	while(!m_ptsqueue.empty()){
		m_ptsqueue.pop();
	}

	m_inited = true;
	m_cb = cb;
	m_user_data = user_data;

	return true;
}
bool NvVideoEncoder::InputData(VideoRawData & data){
	if(!m_inited)
		return false;

	EncodeFrameConfig frame = {0};
	if(data.deviceptr){
		frame.dptr = (CUdeviceptr)data.deviceptr;
	}else{
		frame.yuv[0] = data.buffer[0];
		frame.yuv[1] = data.buffer[1];
		frame.yuv[2] = data.buffer[2];
	}
	frame.stride[0] = data.line_size[0];
	frame.stride[1] = data.line_size[1];
	frame.stride[2] = data.line_size[2];
	frame.width = data.width;
	frame.height = data.height;
	m_ptsqueue.push(data.pts);
	EncodeFrame(&frame);

	return true;
}
bool NvVideoEncoder::Stop(){
	if(!m_nvencoder_api)
		return NV_ENC_SUCCESS;
	FlushEncoder();
	Deinitialize();
	delete m_nvencoder_api;
	m_nvencoder_api = nullptr;
	m_inited = false;
	return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
NVENCSTATUS NvVideoEncoder::Deinitialize() {
    NVENCSTATUS nv_status = NV_ENC_SUCCESS;
    ReleaseIOBuffers();

    nv_status = m_nvencoder_api->NvEncDestroyEncoder();

    if (m_cuda_device) {
		CUresult cu_result = CUDA_SUCCESS;
		cu_result = cuCtxDestroy((CUcontext)m_cuda_device);
		if (cu_result != CUDA_SUCCESS)
			PRINTERR("cuCtxDestroy error:0x%x\n", cu_result);
        m_cuda_device = nullptr;
    }
    if(m_ctx_lock){
		cuvidCtxLockDestroy(m_ctx_lock);
		m_ctx_lock = nullptr;
    }
    return nv_status;
}

void NvVideoEncoder::OutputFrame(NV_ENC_LOCK_BITSTREAM lockBitstreamData){

	MediaDataBitStream bs;
	bs.buffer = (unsigned char*)lockBitstreamData.bitstreamBufferPtr;
	bs.buffer_len = lockBitstreamData.bitstreamSizeInBytes;
	bs.is_key = lockBitstreamData.pictureType == NV_ENC_PIC_TYPE_IDR ? true : false;
	if(!m_ptsqueue.empty()){
		bs.pts = m_ptsqueue.front();
		bs.dts = m_ptsqueue.front();
		m_ptsqueue.pop();
	}else{
		bs.pts = 0;
		bs.dts = 0;
	}
	if(m_cb){
		m_cb(bs,m_user_data);
	}
}

static void YUV420ToNV12( unsigned char *yuv_luma, unsigned char *yuv_cb, unsigned char *yuv_cr,
        unsigned char *nv12_luma, unsigned char *nv12_chroma,
        int width, int height , int src_stride, int dst_stride) {
	if (src_stride == 0)
		src_stride = width;
	if (dst_stride == 0)
		dst_stride = width;

	for (int y = 0 ; y < height ; y++) {
		memcpy( nv12_luma + (dst_stride*y), yuv_luma + (src_stride*y) , width );
	}

	for (int y = 0 ; y < height/2 ; y++) {
		for (int x= 0 ; x < width; x=x+2) {
			nv12_chroma[(y*dst_stride) + x] = yuv_cb[((src_stride/2)*y) + (x >>1)];
			nv12_chroma[(y*dst_stride) +(x+1)] = yuv_cr[((src_stride/2)*y) + (x >>1)];
		}
	}
}

NVENCSTATUS NvVideoEncoder::EncodeFrame(EncodeFrameConfig * frame) {
    NVENCSTATUS nv_status = NV_ENC_SUCCESS;
    uint32_t locked_pitch = 0;
    EncodeBuffer * encode_buffer = nullptr;

    if (!frame) {
        return NV_ENC_ERR_INVALID_PARAM;
    }

    encode_buffer = m_encoder_buffer_queue.GetAvailable();
	if(!encode_buffer) {
		NV_ENC_LOCK_BITSTREAM bit_stream;
		encode_buffer = m_encoder_buffer_queue.GetPending();
		if(!encode_buffer)
			return NV_ENC_ERR_OUT_OF_MEMORY;
		m_nvencoder_api->ProcessOutput(encode_buffer,bit_stream);
		OutputFrame(bit_stream);
		if (encode_buffer->stInputBfr.hDeviceInputSurface) {
			nv_status = m_nvencoder_api->NvEncUnmapInputResource(encode_buffer->stInputBfr.hDeviceInputSurface);
			encode_buffer->stInputBfr.hDeviceInputSurface = nullptr;
		}
		encode_buffer = m_encoder_buffer_queue.GetAvailable();
	}
    if(frame->dptr > 0){
    	encode_buffer->stInputBfr.bDeviceSurface = true;
    	CCtxAutoLock lock(m_ctx_lock);
    	CUDA_MEMCPY2D memcpy2D  = {0};
		memcpy2D.srcMemoryType  = CU_MEMORYTYPE_DEVICE;
		memcpy2D.srcDevice      = frame->dptr;
		memcpy2D.srcPitch       = frame->stride[0];
		memcpy2D.dstMemoryType  = CU_MEMORYTYPE_DEVICE;
		memcpy2D.dstDevice      = (CUdeviceptr)encode_buffer->stInputBfr.pNV12devPtr;
		memcpy2D.dstPitch       = encode_buffer->stInputBfr.uNV12Stride;
		memcpy2D.WidthInBytes   = frame->width;
		memcpy2D.Height         = frame->height*3/2;
		CUresult cu_result = cuMemcpy2D(&memcpy2D);
		if(cu_result != CUDA_SUCCESS)
			return NV_ENC_ERR_GENERIC;
		nv_status = m_nvencoder_api->NvEncMapInputResource(encode_buffer->stInputBfr.nvRegisteredResource, &encode_buffer->stInputBfr.hDeviceInputSurface);
		if (nv_status != NV_ENC_SUCCESS) {
			PRINTERR("Failed to Map input buffer %p\n", encode_buffer->stInputBfr.hDeviceInputSurface);
			return nv_status;
		}
    }else{
		encode_buffer->stInputBfr.bDeviceSurface = false;
		unsigned char * input_surface;
		nv_status = m_nvencoder_api->NvEncLockInputBuffer(encode_buffer->stInputBfr.hHostInputSurface, (void**)&input_surface, &locked_pitch);
		if (nv_status != NV_ENC_SUCCESS)
			return nv_status;
		unsigned char * input_surface_ch = input_surface + (encode_buffer->stInputBfr.dwHeight*locked_pitch);
		YUV420ToNV12(frame->yuv[0], frame->yuv[1], frame->yuv[2], input_surface, input_surface_ch,
				frame->width, frame->height, frame->stride[0], locked_pitch);
		nv_status = m_nvencoder_api->NvEncUnlockInputBuffer(encode_buffer->stInputBfr.hHostInputSurface);
		if (nv_status != NV_ENC_SUCCESS)
			return nv_status;
    }

    nv_status = m_nvencoder_api->NvEncEncodeFrame(encode_buffer, nullptr, frame->width,
    		frame->height, (NV_ENC_PIC_STRUCT)m_encode_config.pictureStruct);
    return nv_status;
}
NVENCSTATUS NvVideoEncoder::InitCuda(uint32_t device_id) {
    CUresult cu_result;
    CUdevice device;
    CUcontext cu_ctx;
    int  device_count = 0;
    int  sm_minor = 0, sm_major = 0;

    cu_result = cuInit(0, __CUDA_API_VERSION, nullptr);
    if (cu_result != CUDA_SUCCESS) {
        PRINTERR("cuInit error:0x%x\n", cu_result);
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    cu_result = cuDeviceGetCount(&device_count);
    if (cu_result != CUDA_SUCCESS) {
        PRINTERR("cuDeviceGetCount error:0x%x\n", cu_result);
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    // If dev is negative value, we clamp to 0
    if ((int)device_id < 0)
    	device_id = 0;

    if (device_id >(unsigned int)device_count - 1) {
        PRINTERR("Invalid Device Id = %d\n", device_id);
        return NV_ENC_ERR_INVALID_ENCODERDEVICE;
    }

    cu_result = cuDeviceGet(&device, device_id);
    if (cu_result != CUDA_SUCCESS) {
        PRINTERR("cuDeviceGet error:0x%x\n", cu_result);
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    cu_result = cuDeviceComputeCapability(&sm_major, &sm_minor, device_id);
    if (cu_result != CUDA_SUCCESS) {
        PRINTERR("cuDeviceComputeCapability error:0x%x\n", cu_result);
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    if (((sm_major << 4) + sm_minor) < 0x30) {
        PRINTERR("GPU %d does not have NVENC capabilities exiting\n", device_id);
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    cu_result = cuCtxCreate((CUcontext*)(&m_cuda_device), 0, device);
    if (cu_result != CUDA_SUCCESS) {
        PRINTERR("cuCtxCreate error:0x%x\n", cu_result);
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    cu_result = cuCtxPopCurrent(&cu_ctx);
    if (cu_result != CUDA_SUCCESS) {
        PRINTERR("cuCtxPopCurrent error:0x%x\n", cu_result);
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    cu_result = cuvidCtxLockCreate(&m_ctx_lock, cu_ctx);
    if (cu_result != CUDA_SUCCESS) {
		PRINTERR("cuvidCtxLockCreate error:0x%x\n", cu_result);
		return NV_ENC_ERR_NO_ENCODE_DEVICE;
	}


    return NV_ENC_SUCCESS;
}

NVENCSTATUS NvVideoEncoder::AllocateIOBuffers(uint32_t width, uint32_t height, NV_ENC_BUFFER_FORMAT bufefr_fmt) {
    NVENCSTATUS nv_status = NV_ENC_SUCCESS;

    m_encoder_buffer_queue.Initialize(m_encoder_buffer, m_encoder_buffer_count);
    CCtxAutoLock lock(m_ctx_lock);
    for (uint32_t i = 0; i < m_encoder_buffer_count; i++) {
    	nv_status = m_nvencoder_api->NvEncCreateInputBuffer(width, height, &m_encoder_buffer[i].stInputBfr.hHostInputSurface, bufefr_fmt);
        if (nv_status != NV_ENC_SUCCESS)
            return nv_status;

        m_encoder_buffer[i].stInputBfr.bufferFmt = bufefr_fmt;
        m_encoder_buffer[i].stInputBfr.dwWidth = width;
        m_encoder_buffer[i].stInputBfr.dwHeight = height;

        CUresult cu_result = cuMemAllocPitch(&m_encoder_buffer[i].stInputBfr.pNV12devPtr,
                    (size_t*)&m_encoder_buffer[i].stInputBfr.uNV12Stride,
					width, height * 3 / 2, 16);

        if(cu_result != CUDA_SUCCESS)
        	return NV_ENC_ERR_GENERIC;

        nv_status = m_nvencoder_api->NvEncRegisterResource(NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,
				   (void*)m_encoder_buffer[i].stInputBfr.pNV12devPtr,
				   width, height,
				   m_encoder_buffer[i].stInputBfr.uNV12Stride,
				   &m_encoder_buffer[i].stInputBfr.nvRegisteredResource);

	    if (nv_status != NV_ENC_SUCCESS)
		   return nv_status;

        nv_status = m_nvencoder_api->NvEncCreateBitstreamBuffer(BITSTREAM_BUFFER_SIZE, &m_encoder_buffer[i].stOutputBfr.hBitstreamBuffer);
        if (nv_status != NV_ENC_SUCCESS)
            return nv_status;
        m_encoder_buffer[i].stOutputBfr.dwBitstreamBufferSize = BITSTREAM_BUFFER_SIZE;
    }
    return NV_ENC_SUCCESS;
}
NVENCSTATUS NvVideoEncoder::ReleaseIOBuffers() {
	CCtxAutoLock lock(m_ctx_lock);
    for (uint32_t i = 0; i < m_encoder_buffer_count; i++) {
	    cuMemFree(m_encoder_buffer[i].stInputBfr.pNV12devPtr);
		m_nvencoder_api->NvEncDestroyInputBuffer(m_encoder_buffer[i].stInputBfr.hHostInputSurface);
        m_encoder_buffer[i].stInputBfr.hHostInputSurface = nullptr;
        m_nvencoder_api->NvEncUnregisterResource(m_encoder_buffer[i].stInputBfr.nvRegisteredResource);
		m_nvencoder_api->NvEncDestroyBitstreamBuffer(m_encoder_buffer[i].stOutputBfr.hBitstreamBuffer);
        m_encoder_buffer[i].stOutputBfr.hBitstreamBuffer = nullptr;
    }
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NvVideoEncoder::FlushEncoder() {
    NVENCSTATUS nv_status = m_nvencoder_api->NvEncFlushEncoderQueue(nullptr);
    if (nv_status != NV_ENC_SUCCESS) {
        return nv_status;
    }

    EncodeBuffer * encode_buffer = nullptr;
    do{
    	encode_buffer = m_encoder_buffer_queue.GetPending();
    	if(!encode_buffer){
			break;
		}
    	NV_ENC_LOCK_BITSTREAM bit_stream;
    	m_nvencoder_api->ProcessOutput(encode_buffer,bit_stream);
		OutputFrame(bit_stream);
		if (encode_buffer->stInputBfr.hDeviceInputSurface) {
			nv_status = m_nvencoder_api->NvEncUnmapInputResource(encode_buffer->stInputBfr.hDeviceInputSurface);
			encode_buffer->stInputBfr.hDeviceInputSurface = nullptr;
		}
    }while(true);

    return nv_status;
}


