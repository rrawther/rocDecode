/*
Copyright (c) 2023 - 2024 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#pragma once

#include "roc_video_dec.h"

/**
 * @brief helper function for inferring AVCodecID from rocDecVideoCodec
 * 
 * @param rocdec_codec 
 * @return AVCodecID 
 */
static inline AVCodecID RocDecVideoCodec2AVCodec(rocDecVideoCodec rocdec_codec) {
    switch (rocdec_codec) {
        case rocDecVideoCodec_MPEG1 : return AV_CODEC_ID_MPEG1VIDEO;
        case rocDecVideoCodec_MPEG2 : return AV_CODEC_ID_MPEG2VIDEO;
        case rocDecVideoCodec_MPEG4 : return AV_CODEC_ID_MPEG4;
        case rocDecVideoCodec_AVC   : return AV_CODEC_ID_H264;
        case rocDecVideoCodec_HEVC  : return AV_CODEC_ID_HEVC;
        case rocDecVideoCodec_VP8   : return AV_CODEC_ID_VP8;
        case rocDecVideoCodec_VP9   : return AV_CODEC_ID_VP9;
        case rocDecVideoCodec_JPEG  : return AV_CODEC_ID_MJPEG;
        case rocDecVideoCodec_AV1   : return AV_CODEC_ID_AV1;
        default                     : return AV_CODEC_ID_NONE;
    }
}

/**
 * @brief helper function for inferring AVCodecID from rocDecVideoSurfaceFormat
 * 
 * @param rocdec_codec 
 * @return AVCodecID 
 */
static inline rocDecVideoChromaFormat AVPixelFormat2rocDecVideoSurfaceFormat(AVPixelFormat av_pixel_format) {
    switch (av_pixel_format) {
        case AV_PIX_FMT_YUV420P : 
        case AV_PIX_FMT_YUVJ420P : 
            return rocDecVideoSurfaceFormat_NV12;           // need to see if this is actually correct
        case AV_PIX_FMT_YUV444P : 
        case AV_PIX_FMT_YUVJ444P : 
            return rocDecVideoSurfaceFormat_YUV444;
        case AV_PIX_FMT_YUV422P : 
        case AV_PIX_FMT_YUVJ422P : 
            return rocDecVideoSurfaceFormat_NV12;           // this is wrong: we don't really need to deal with 422 surface formats for most videos
        case AV_PIX_FMT_YUV420P10LE :
        case AV_PIX_FMT_YUV420P12LE :
            return rocDecVideoSurfaceFormat_P016;
        default :
            std::cerr << "ERROR: " << av_get_pix_fmt_name(av_pixel_format) << " pixel_format is not supported!" << std::endl;          
            return rocDecVideoSurfaceFormat_NV12;       // for sanity
    }
}

FFMpegVideoDecoder::FFMpegVideoDecoder(int device_id, OutputSurfaceMemoryType out_mem_type, rocDecVideoCodec codec, bool force_zero_latency,
              const Rect *p_crop_rect, bool extract_user_sei_Message, uint32_t disp_delay, int max_width, int max_height, uint32_t clk_rate) :
              device_id_{device_id}, out_mem_type_(out_mem_type), codec_id_(codec), b_force_zero_latency_(force_zero_latency), 
              b_extract_sei_message_(extract_user_sei_Message), disp_delay_(disp_delay), max_width_ (max_width), max_height_(max_height) {

    if ((out_mem_type_ == OUT_SURFACE_MEM_DEV_INTERNAL) || (out_mem_type_ == OUT_SURFACE_MEM_NOT_MAPPED)) {
        THROW("Output Memory Type is not supported");
    }
    if (!InitHIP(device_id_)) {
        THROW("Failed to initilize the HIP");
    }
    if (p_crop_rect) crop_rect_ = *p_crop_rect;
    if (b_extract_sei_message_) {
        fp_sei_ = fopen("rocdec_sei_message.txt", "wb");
        curr_sei_message_ptr_ = new RocdecSeiMessageInfo;
        memset(&sei_message_display_q_, 0, sizeof(sei_message_display_q_));
    }
    //alloc av_frame
    avframe_ = av_frame_alloc();
    if (!avframe_) {
        THROW("FFMpegVideoDecoder::av_frame_alloc failed");

    }
    av_pkt_ = av_packet_alloc();
    if (!av_pkt_) {
        THROW("FFMpegVideoDecoder::av_packet_alloc failed");
    }
    // start the FFMpeg decoding thread
    ffmpeg_decoder_thread_ = new std::thread(&FFMpegVideoDecoder::DecodeThread, this);
    if (!ffmpeg_decoder_thread_) {
        THROW("FFMpegVideoDecoder create thread failed");
    }


FFMpegVideoDecoder::~FFMpegVideoDecoder() {
    auto start_time = StartTimer();
    if (curr_sei_message_ptr_) {
        delete curr_sei_message_ptr_;
        curr_sei_message_ptr_ = nullptr;
    }

    if (fp_sei_) {
        fclose(fp_sei_);
        fp_sei_ = nullptr;
    }

    if (dec_context_) {
        avcodec_free_context(&dec_context_);
    }

    if (avframe_) {
        av_frame_free(&avframe_);
    }

    if (av_pkt_) {
        av_packet_free(&av_pkt_);
    }
#if 0 // todo:: check if the following is required    
    if (curr_video_format_ptr_) {
        delete curr_video_format_ptr_;
        curr_video_format_ptr_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(mtx_vp_frame_);
    if (out_mem_type_ != OUT_SURFACE_MEM_DEV_INTERNAL) {
        for (auto &p_frame : vp_frames_) {
            if (p_frame.frame_ptr) {
              if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
                  hipError_t hip_status = hipFree(p_frame.frame_ptr);
                  if (hip_status != hipSuccess) {
                      std::cerr << "ERROR: hipFree failed! (" << hip_status << ")" << std::endl;
                  }
              }
              else
                  delete[] (p_frame.frame_ptr);
              p_frame.frame_ptr = nullptr;
            }
        }
    }
#endif    

    if (hip_stream_) {
        hipError_t hip_status = hipSuccess;
        hip_status = hipStreamDestroy(hip_stream_);
        if (hip_status != hipSuccess) {
            std::cerr << "ERROR: hipStream_Destroy failed! (" << hip_status << ")" << std::endl;
        }
    }
    if (fp_out_) {
        fclose(fp_out_);
        fp_out_ = nullptr;
    }

    double elapsed_time = StopTimer(start_time);
    AddDecoderSessionOverHead(std::this_thread::get_id(), elapsed_time);
}

/* Return value from HandleVideoSequence() are interpreted as   :
*  0: fail, 1: succeeded, > 1: override dpb size of parser (set by CUVIDPARSERPARAMS::max_num_decode_surfaces while creating parser)
*/
int FFMpegVideoDecoder::HandleVideoSequence(RocdecVideoFormat *p_video_format) {

    if (p_video_format == nullptr) {
        ROCDEC_THROW("Rocdec:: Invalid video format in HandleVideoSequence: ", ROCDEC_INVALID_PARAMETER);
        return 0;
    }
    auto start_time = StartTimer();
    input_video_info_str_.str("");
    input_video_info_str_.clear();
    input_video_info_str_ << "Input Video Information" << std::endl
        << "\tCodec        : " << GetCodecFmtName(p_video_format->codec) << std::endl;
        if (p_video_format->frame_rate.numerator && p_video_format->frame_rate.denominator) {
            input_video_info_str_ << "\tFrame rate   : " << p_video_format->frame_rate.numerator << "/" << p_video_format->frame_rate.denominator << " = " << 1.0 * p_video_format->frame_rate.numerator / p_video_format->frame_rate.denominator << " fps" << std::endl;
        }
    input_video_info_str_ << "\tSequence     : " << (p_video_format->progressive_sequence ? "Progressive" : "Interlaced") << std::endl
        << "\tCoded size   : [" << p_video_format->coded_width << ", " << p_video_format->coded_height << "]" << std::endl
        << "\tDisplay area : [" << p_video_format->display_area.left << ", " << p_video_format->display_area.top << ", "
            << p_video_format->display_area.right << ", " << p_video_format->display_area.bottom << "]" << std::endl
        << "\tChroma       : " << GetVideoChromaFormatName(p_video_format->chroma_format) << std::endl
        << "\tBit depth    : " << p_video_format->bit_depth_luma_minus8 + 8
    ;
    input_video_info_str_ << std::endl;

    int num_decode_surfaces = p_video_format->min_num_decode_surfaces;
    // check if the codec is supported in FFMpeg
    // Initialize FFMpeg and find the decoder for codec
    if (!decoder) decoder_ = avcodec_find_decoder(RocDecVideoCodec2AVCodec(p_video_format->codec))
    if(!decoder_) {
        ROCDEC_THROW("rocDecode<FFMpeg>:: Codec not supported by FFMpeg ", ROCDEC_NOT_SUPPORTED);
        return 0;
    }
    if (!dec_context_) {
        dec_context_ = avcodec_alloc_context3(decoder_);        //alloc dec_context_
        if (!dec_context_) {
            THROW("Could not allocate video codec context");
        }
        // get the output pixel format from dec_context_
        decoder_pixel_format_ = dec_context_->pix_fmt;
        
    }
    if (curr_video_format_ptr_ == nullptr) {
        curr_video_format_ptr_ = new RocdecVideoFormat();
    }
    // store current video format: this is required to call reconfigure from application in case of random seek
    if (curr_video_format_ptr_) memcpy(curr_video_format_ptr_, p_video_format, sizeof(RocdecVideoFormat));

    if (coded_width_ && coded_height_) {
        end_of_stream_ = false;
        // rocdecCreateDecoder() has been called before, and now there's possible config change
        return ReconfigureDecoder(p_video_format);
    }
    // e_codec has been set in the constructor (for parser). Here it's set again for potential correction
    codec_id_ = p_video_format->codec;
    video_chroma_format_ = p_video_format->chroma_format;
    bitdepth_minus_8_ = p_video_format->bit_depth_luma_minus8;
    byte_per_pixel_ = bitdepth_minus_8_ > 0 ? 2 : 1;

    // convert AVPixelFormat to rocDecVideoChromaFormat
    video_surface_format_ = AVPixelFormat2rocDecVideoSurfaceFormat(decoder_pixel_format_);
    coded_width_ = p_video_format->coded_width;
    coded_height_ = p_video_format->coded_height;
    disp_rect_.top = p_video_format->display_area.top;
    disp_rect_.bottom = p_video_format->display_area.bottom;
    disp_rect_.left = p_video_format->display_area.left;
    disp_rect_.right = p_video_format->display_area.right;
    disp_width_ = p_video_format->display_area.right - p_video_format->display_area.left;
    disp_height_ = p_video_format->display_area.bottom - p_video_format->display_area.top;

    // AV1 has max width/height of sequence in sequence header
    if (codec_id_ == rocDecVideoCodec_AV1 && p_video_format->seqhdr_data_length > 0) {
        // dont overwrite if it is already set from cmdline or reconfig.txt
        if (!(max_width_ > p_video_format->coded_width || max_height_ > p_video_format->coded_height)) {
            RocdecVideoFormatEx *vidFormatEx = (RocdecVideoFormatEx *)p_video_format;
            max_width_ = vidFormatEx->max_width;
            max_height_ = vidFormatEx->max_height;
        }
    }
    if (max_width_ < (int)p_video_format->coded_width)
        max_width_ = p_video_format->coded_width;
    if (max_height_ < (int)p_video_format->coded_height)
        max_height_ = p_video_format->coded_height;

    if (!(crop_rect_.right && crop_rect_.bottom)) {
        target_width_ = (disp_width_ + 1) & ~1;
        target_height_ = (disp_height_ + 1) & ~1;
    } else {
        target_width_ = (crop_rect_.right - crop_rect_.left + 1) & ~1;
        target_height_ = (crop_rect_.bottom - crop_rect_.top + 1) & ~1;
    }

    chroma_height_ = (int)(ceil(target_height_ * GetChromaHeightFactor(video_surface_format_)));
    num_chroma_planes_ = GetChromaPlaneCount(video_surface_format_);
    if (video_chroma_format_ == rocDecVideoChromaFormat_Monochrome) num_chroma_planes_ = 0;
    surface_stride_ = videoDecodeCreateInfo.target_width * byte_per_pixel_;   
    chroma_vstride_ = (int)(ceil(surface_vstride_ * GetChromaHeightFactor(video_surface_format_)));
    // fill output_surface_info_
    output_surface_info_.output_width = target_width_;
    output_surface_info_.output_height = target_height_;
    output_surface_info_.output_pitch  = surface_stride_;
    output_surface_info_.output_vstride = videoDecodeCreateInfo.target_height;
    output_surface_info_.bit_depth = bitdepth_minus_8_ + 8;
    output_surface_info_.bytes_per_pixel = byte_per_pixel_;
    output_surface_info_.surface_format = video_surface_format_;
    output_surface_info_.num_chroma_planes = num_chroma_planes_;
    if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
        output_surface_info_.output_surface_size_in_bytes = GetFrameSize();
        output_surface_info_.mem_type = OUT_SURFACE_MEM_DEV_COPIED;
    } else if (out_mem_type_ == OUT_SURFACE_MEM_HOST_COPIED){
        output_surface_info_.output_surface_size_in_bytes = GetFrameSize();
        output_surface_info_.mem_type = OUT_SURFACE_MEM_HOST_COPIED;
        
    double elapsed_time = StopTimer(start_time);
    AddDecoderSessionOverHead(std::this_thread::get_id(), elapsed_time);
    return num_decode_surfaces;
}

/**
 * @brief function to reconfigure decoder if there is a change in sequence params.
 *
 * @param p_video_format
 * @return int 1: success 0: fail
 */
int FFMpegVideoDecoder::ReconfigureDecoder(RocdecVideoFormat *p_video_format) {
    if (p_video_format->codec != codec_id_) {
        ROCDEC_THROW("Reconfigure Not supported for codec change", ROCDEC_NOT_SUPPORTED);
        return 0;
    }
    if (p_video_format->chroma_format != video_chroma_format_) {
        ROCDEC_THROW("Reconfigure Not supported for chroma format change", ROCDEC_NOT_SUPPORTED);
        return 0;
    }
    if (p_video_format->bit_depth_luma_minus8 != bitdepth_minus_8_){
        ROCDEC_THROW("Reconfigure Not supported for bit depth change", ROCDEC_NOT_SUPPORTED);
        return 0;
    }
    bool is_decode_res_changed = !(p_video_format->coded_width == coded_width_ && p_video_format->coded_height == coded_height_);
    bool is_display_rect_changed = !(p_video_format->display_area.bottom == disp_rect_.bottom &&
                                     p_video_format->display_area.top == disp_rect_.top &&
                                     p_video_format->display_area.left == disp_rect_.left &&
                                     p_video_format->display_area.right == disp_rect_.right);

    if (!is_decode_res_changed && !is_display_rect_changed && !b_force_recofig_flush_) {
        return 1;
    }

    // Flush and clear internal frame store to reconfigure when either coded size or display size has changed.
    if (p_reconfig_params_ && p_reconfig_params_->p_fn_reconfigure_flush) 
        num_frames_flushed_during_reconfig_ += p_reconfig_params_->p_fn_reconfigure_flush(this, p_reconfig_params_->reconfig_flush_mode, static_cast<void *>(p_reconfig_params_->p_reconfig_user_struct));
    // clear the existing output buffers of different size
    // note that app lose the remaining frames in the vp_frames/vp_frames_q in case application didn't set p_fn_reconfigure_flush_ callback
    std::lock_guard<std::mutex> lock(mtx_vp_frame_);
    while(!vp_frames_.empty()) {
        DecFrameBuffer *p_frame = &vp_frames_.back();
        // pop decoded frame
        vp_frames_.pop_back();
        if (p_frame->frame_ptr) {
            if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
                hipError_t hip_status = hipFree(p_frame->frame_ptr);
                if (hip_status != hipSuccess) std::cerr << "ERROR: hipFree failed! (" << hip_status << ")" << std::endl;
            }
            else
                delete [] (p_frame->frame_ptr);
        }
    }
    output_frame_cnt_ = 0;     // reset frame_count
    if (is_decode_res_changed) {
        coded_width_ = p_video_format->coded_width;
        coded_height_ = p_video_format->coded_height;
    }
    if (is_display_rect_changed) {
        disp_rect_.left = p_video_format->display_area.left;
        disp_rect_.right = p_video_format->display_area.right;
        disp_rect_.top = p_video_format->display_area.top;
        disp_rect_.bottom = p_video_format->display_area.bottom;
        disp_width_ = p_video_format->display_area.right - p_video_format->display_area.left;
        disp_height_ = p_video_format->display_area.bottom - p_video_format->display_area.top;
        chroma_height_ = static_cast<int>(std::ceil(target_height_ * GetChromaHeightFactor(video_surface_format_)));
        if (!(crop_rect_.right && crop_rect_.bottom)) {
            target_width_ = (disp_width_ + 1) & ~1;
            target_height_ = (disp_height_ + 1) & ~1;
        } else {
            target_width_ = (crop_rect_.right - crop_rect_.left + 1) & ~1;
            target_height_ = (crop_rect_.bottom - crop_rect_.top + 1) & ~1;
        }
    }

    surface_stride_ = target_width_ * byte_per_pixel_;
    chroma_height_ = static_cast<int>(ceil(target_height_ * GetChromaHeightFactor(video_surface_format_)));
    num_chroma_planes_ = GetChromaPlaneCount(video_surface_format_);
    if (p_video_format->chroma_format == rocDecVideoChromaFormat_Monochrome) num_chroma_planes_ = 0;
    chroma_vstride_ = static_cast<int>(std::ceil(surface_vstride_ * GetChromaHeightFactor(video_surface_format_)));
    // Fill output_surface_info_
    output_surface_info_.output_width = target_width_;
    output_surface_info_.output_height = target_height_;
    output_surface_info_.output_pitch  = surface_stride_;
    output_surface_info_.output_vstride = (out_mem_type_ == OUT_SURFACE_MEM_DEV_INTERNAL) ? surface_vstride_ : target_height_;
    output_surface_info_.bit_depth = bitdepth_minus_8_ + 8;
    output_surface_info_.bytes_per_pixel = byte_per_pixel_;
    output_surface_info_.surface_format = video_surface_format_;
    output_surface_info_.num_chroma_planes = num_chroma_planes_;
    if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
        output_surface_info_.output_surface_size_in_bytes = GetFrameSize();
        output_surface_info_.mem_type = OUT_SURFACE_MEM_DEV_COPIED;
    } else if (out_mem_type_ == OUT_SURFACE_MEM_HOST_COPIED) {
        output_surface_info_.output_surface_size_in_bytes = GetFrameSize();
        output_surface_info_.mem_type = OUT_SURFACE_MEM_HOST_COPIED;
    }

    // If the coded_width or coded_height hasn't changed but display resolution has changed, then need to update width and height for
    // correct output with cropping. There is no need to reconfigure the decoder.
    if (!is_decode_res_changed && is_display_rect_changed) {
        return 1;
    }

    input_video_info_str_.str("");
    input_video_info_str_.clear();
    input_video_info_str_ << "Input Video Resolution Changed:" << std::endl
        << "\tCoded size   : [" << p_video_format->coded_width << ", " << p_video_format->coded_height << "]" << std::endl
        << "\tDisplay area : [" << p_video_format->display_area.left << ", " << p_video_format->display_area.top << ", "
            << p_video_format->display_area.right << ", " << p_video_format->display_area.bottom << "]" << std::endl;
    input_video_info_str_ << std::endl;
    is_decoder_reconfigured_ = true;
    return 1;
}

/**
 * @brief 
 * 
 * @param pPicParams 
 * @return int 1: success 0: fail
 */
int FFMpegVideoDecoder::HandlePictureDecode(RocdecPicParams *pPicParams) {
    AVPacket av_pkt = { 0 };
    av_pkt.data = pPicParams->bitstream_data;
    av_pkt.size = pPicParams->bitstream_data_len;
    av_pkt.flags = 0;
    av_pkt.pts = last_packet_pts_;

    pic_num_in_dec_order_[pPicParams->curr_pic_idx] = decode_poc_++;
    if (!av_pkt.data || !av_pkt.size) {
        end_of_stream_ = true;
    }
    //push packet into packet q for decoding
    PushPacket(av_pkt);
    last_decode_surf_idx_ = pPicParams->curr_pic_idx;
    return 1;
}

/**
 * @brief function to handle display picture
 * 
 * @param pDispInfo 
 * @return int 0:fail 1: success
 */
int FFMpegVideoDecoder::HandlePictureDisplay(RocdecParserDispInfo *pDispInfo) {
    //RocdecProcParams video_proc_params = {};
    //video_proc_params.progressive_frame = pDispInfo->progressive_frame;
    //video_proc_params.top_field_first = pDispInfo->top_field_first;

    if (b_extract_sei_message_) {
        if (sei_message_display_q_[pDispInfo->picture_index].sei_data) {
            // Write SEI Message
            uint8_t *sei_buffer = (uint8_t *)(sei_message_display_q_[pDispInfo->picture_index].sei_data);
            uint32_t sei_num_messages = sei_message_display_q_[pDispInfo->picture_index].sei_message_count;
            RocdecSeiMessage *sei_message = sei_message_display_q_[pDispInfo->picture_index].sei_message;
            if (fp_sei_) {
                for (uint32_t i = 0; i < sei_num_messages; i++) {
                    if (codec_id_ == rocDecVideoCodec_AVC || rocDecVideoCodec_HEVC) {
                        switch (sei_message[i].sei_message_type) {
                            case SEI_TYPE_TIME_CODE: {
                                //todo:: check if we need to write timecode
                            }
                            break;
                            case SEI_TYPE_USER_DATA_UNREGISTERED: {
                                fwrite(sei_buffer, sei_message[i].sei_message_size, 1, fp_sei_);
                            }
                            break;
                        }
                    }
                    if (codec_id_ == rocDecVideoCodec_AV1) {
                        fwrite(sei_buffer, sei_message[i].sei_message_size, 1, fp_sei_);
                    }    
                    sei_buffer += sei_message[i].sei_message_size;
                }
            }
            free(sei_message_display_q_[pDispInfo->picture_index].sei_data);
            sei_message_display_q_[pDispInfo->picture_index].sei_data = NULL; // to avoid double free
            free(sei_message_display_q_[pDispInfo->picture_index].sei_message);
            sei_message_display_q_[pDispInfo->picture_index].sei_message = NULL; // to avoid double free
        }
    }
    // vp_frames_.size() is empty, wait for decoding to finish
    // this will happen during PopFrame()
    AVFrame *p_av_frame = PopFrame();
    if (p_av_frame == nullptr) {
        std::cerr << "Invalid avframe decode output" << std::endl;
        return 0;
    }
    void * src_dev_ptr[3] = { 0 };
    uint32_t src_pitch[3] = { 0 };

    // copy the decoded surface info device or host
    uint8_t *p_dec_frame = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_vp_frame_);
        // if not enough frames in stock, allocate
        if ((unsigned)++output_frame_cnt_ > vp_frames_.size()) {
            num_alloced_frames_++;
            DecFrameBufferFFMpeg dec_frame = { 0 };
            if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
                // allocate device memory
                HIP_API_CALL(hipMalloc((void **)&dec_frame.frame_ptr, GetFrameSize()));
            } else {
                dec_frame.frame_ptr = p_av_frame->data[0];
            }
            dec_frame.av_frame_ptr = p_av_frame;
            dec_frame.pts = pDispInfo->pts;
            dec_frame.picture_index = pDispInfo->picture_index;
            vp_frames_.push_back(dec_frame);
        }
        p_dec_frame = vp_frames_[output_frame_cnt_ - 1].frame_ptr;
    }

    if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
        // Copy luma data
        int dst_pitch = disp_width_ * byte_per_pixel_;
        uint8_t *p_src_ptr_y = static_cast<uint8_t *>(src_dev_ptr[0]) + (disp_rect_.top + crop_rect_.top) * src_pitch[0] + (disp_rect_.left + crop_rect_.left) * byte_per_pixel_;
        if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
            if (src_pitch[0] == dst_pitch) {
                int luma_size = src_pitch[0] * coded_height_;
                HIP_API_CALL(hipMemcpyHtoDAsync(p_dec_frame, p_src_ptr_y, luma_size, hip_stream_));
            } else {
                // use 2d copy to copy an ROI
                HIP_API_CALL(hipMemcpy2DAsync(p_dec_frame, dst_pitch, p_src_ptr_y, src_pitch[0], dst_pitch, disp_height_, hipMemcpyDeviceToDevice, hip_stream_));
            }
        } else
            HIP_API_CALL(hipMemcpy2DAsync(p_dec_frame, dst_pitch, p_src_ptr_y, src_pitch[0], dst_pitch, disp_height_, hipMemcpyDeviceToHost, hip_stream_));

        // Copy chroma plane ( )
        // rocDec output gives pointer to luma and chroma pointers seperated for the decoded frame
        uint8_t *p_frame_uv = p_dec_frame + dst_pitch * disp_height_;
        uint8_t *p_src_ptr_uv = (num_chroma_planes_ == 1) ? static_cast<uint8_t *>(src_dev_ptr[1]) + ((disp_rect_.top + crop_rect_.top) >> 1) * src_pitch[1] + (disp_rect_.left + crop_rect_.left) * byte_per_pixel_ :
        static_cast<uint8_t *>(src_dev_ptr[1]) + (disp_rect_.top + crop_rect_.top) * src_pitch[1] + (disp_rect_.left + crop_rect_.left) * byte_per_pixel_;
        if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
            if (src_pitch[1] == dst_pitch) {
                int chroma_size = chroma_height_ * dst_pitch;
                HIP_API_CALL(hipMemcpyDtoDAsync(p_frame_uv, p_src_ptr_uv, chroma_size, hip_stream_));
            } else {
                // use 2d copy to copy an ROI
                HIP_API_CALL(hipMemcpy2DAsync(p_frame_uv, dst_pitch, p_src_ptr_uv, src_pitch[1], dst_pitch, chroma_height_, hipMemcpyDeviceToDevice, hip_stream_));
            }
        } else
            HIP_API_CALL(hipMemcpy2DAsync(p_frame_uv, dst_pitch, p_src_ptr_uv, src_pitch[1], dst_pitch, chroma_height_, hipMemcpyDeviceToHost, hip_stream_));

        if (num_chroma_planes_ == 2) {
            uint8_t *p_frame_v = p_dec_frame + dst_pitch * (disp_height_ + chroma_height_);
            uint8_t *p_src_ptr_v = static_cast<uint8_t *>(src_dev_ptr[2]) + (disp_rect_.top + crop_rect_.top) * src_pitch[2] + (disp_rect_.left + crop_rect_.left) * byte_per_pixel_;
            if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
                if (src_pitch[2] == dst_pitch) {
                    int chroma_size = chroma_height_ * dst_pitch;
                    HIP_API_CALL(hipMemcpyDtoDAsync(p_frame_v, p_src_ptr_v, chroma_size, hip_stream_));
                } else {
                    // use 2d copy to copy an ROI
                    HIP_API_CALL(hipMemcpy2DAsync(p_frame_v, dst_pitch, p_src_ptr_v, src_pitch[2], dst_pitch, chroma_height_, hipMemcpyDeviceToDevice, hip_stream_));
                }
            } else
                HIP_API_CALL(hipMemcpy2DAsync(p_frame_v, dst_pitch, p_src_ptr_v, src_pitch[2], dst_pitch, chroma_height_, hipMemcpyDeviceToHost, hip_stream_));
        }
    }

    return 1;
}

void FFMpegVideoDecoder::DecodeThread()
{
    for (AVPacket *p_av_pkt; !end_of_stream_ && ((p_av_pkt = PopPacket()) != nullptr);) {
        DecodeAVFrame(p_av_pkt);
    }

}

int FFMpegVideoDecoder::DecodeAVFrame(AVPacket *av_pkt) {
    int status;
    //send packet to av_codec
    status = avcodec_send_packet(dec_context_, av_pkt_);
    if (status < 0) {
        std::cerr << "Error sending av packet for decoding" << std::endl;
        return 0;
    }
    while (status >= 0) {
        // allocate new av_frame if there is not avalable in the queue
        AVFrame *p_frame = av_frame_alloc();
        if (!p_frame) {
            THROW("FFMpegVideoDecoder::av_frame_alloc failed");
        }
        status = avcodec_receive_frame(dec_context_, p_frame);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            av_frame_free(&p_frame);
            end_of_stream_ = (status == AVERROR_EOF);
            return 0;
        }
        else if (status < 0) {
            av_frame_free(&p_frame);
            std::cerr << "Error during decoding" stc::endl;
            return 0;
        }

        std::cout<<"Decoding frame: " << dec_context_->frame_number << std::endl;
        decoded_pic_cnt_++;
        // add frame to the frame_q
        PushFrame(p_frame);

        #if 0
        // copy the decoded surface info device or host
        uint8_t *p_dec_frame = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx_vp_frame_);
            // if not enough frames in stock, allocate
            if ((unsigned)++output_frame_cnt_ > vp_frames_.size()) {
                num_alloced_frames_++;
                DecFrameBufferFFMpeg dec_frame = { 0 };
                if (out_mem_type_ == OUT_SURFACE_MEM_DEV_COPIED) {
                    // allocate device memory
                    HIP_API_CALL(hipMalloc((void **)&dec_frame.frame_ptr, GetFrameSize()));
                } else {
                    dec_frame.frame_ptr = avframe_->data[0];
                }
                dec_frame.av_frame_ptr = avframe_;
                dec_frame.pts = pDispInfo->pts;
                dec_frame.picture_index = pDispInfo->picture_index;
                vp_frames_.push_back(dec_frame);
            }
            p_dec_frame = vp_frames_[output_frame_cnt_ - 1].frame_ptr;
        }
        #endif
    }
}

int FFMpegVideoDecoder::DecodeFrame(const uint8_t *data, size_t size, int pkt_flags, int64_t pts, int *num_decoded_pics) {
    output_frame_cnt_ = 0, output_frame_cnt_ret_ = 0;
    decoded_pic_cnt_ = 0;
    RocdecSourceDataPacket packet = { 0 };
    packet.payload = data;
    packet.payload_size = size;
    packet.flags = pkt_flags | ROCDEC_PKT_TIMESTAMP;
    packet.pts = pts;
    last_packet_pts_ = pts;     // todo: see if parser can return this in HandlePictureDecode
    if (!data || size == 0) {
        packet.flags |= ROCDEC_PKT_ENDOFSTREAM;
    }
    ROCDEC_API_CALL(rocDecParseVideoData(rocdec_parser_, &packet));
    if (num_decoded_pics) {
        *num_decoded_pics = decoded_pic_cnt_;
    }
    return output_frame_cnt_;
}


uint8_t* FFMpegVideoDecoder::GetFrame(int64_t *pts) {
    if (output_frame_cnt_ > 0) {
        std::lock_guard<std::mutex> lock(mtx_vp_frame_);
        if (vp_frames_.size() > 0){
            output_frame_cnt_--;
            if (pts) *pts = vp_frames_[output_frame_cnt_ret_].pts;
            return vp_frames_[output_frame_cnt_ret_++].frame_ptr;
        }
    }
    return nullptr;
}

bool FFMpegVideoDecoder::ReleaseFrame(int64_t pTimestamp, bool b_flushing) {
    // if not flushing the buffers are re-used, so keep them
    if (out_mem_type_ == OUT_SURFACE_MEM_NOT_MAPPED || !b_flushing)
        return true;    // nothing to do
    if (!b_flushing)  
        return true;
    else {
        std::lock_guard<std::mutex> lock(mtx_vp_frame_);
        DecFrameBuffer *fb = &vp_frames_[0];
        if (pTimestamp != fb->pts) {
            std::cerr << "Decoded Frame is released out of order" << std::endl;
            return false;
        }
        vp_frames_.erase(vp_frames_.begin());     // get rid of the frames from the framestore
    }
    return true;
}
