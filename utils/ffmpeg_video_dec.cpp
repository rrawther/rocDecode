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
FFMpegVideoDecoder::FFMpegVideoDecoder(int device_id, OutputSurfaceMemoryType out_mem_type, rocDecVideoCodec codec, bool force_zero_latency,
              const Rect *p_crop_rect, bool extract_user_sei_Message, uint32_t disp_delay, int max_width, int max_height, uint32_t clk_rate) :
              device_id_{device_id}, out_mem_type_(out_mem_type), codec_id_(codec), b_force_zero_latency_(force_zero_latency), 
              b_extract_sei_message_(extract_user_sei_Message), disp_delay_(disp_delay), max_width_ (max_width), max_height_(max_height) {

    if (!InitHIP(device_id_)) {
        THROW("Failed to initilize the HIP");
    }
    if (p_crop_rect) crop_rect_ = *p_crop_rect;
    if (b_extract_sei_message_) {
        fp_sei_ = fopen("rocdec_sei_message.txt", "wb");
        curr_sei_message_ptr_ = new RocdecSeiMessageInfo;
        memset(&sei_message_display_q_, 0, sizeof(sei_message_display_q_));
    }
    // Initialize FFMpeg 
    // find decoder
    decoder_ = avcodec_find_decoder(RocDecVideoCodec2AVCodec(codec))   
    dec_context_ = avcodec_alloc_context3(decoder_);        //alloc dec_context_
    if (!dec_context_) {
        THROW("Could not allocate video codec context");
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

int FFMpegVideoDecoder::DecodeFrame(const uint8_t *data, size_t size, int pkt_flags, int64_t pts, int *num_decoded_pics) {
    int status;
    output_frame_cnt_ = 0, output_frame_cnt_ret_ = 0;
    decoded_pic_cnt_ = 0;

    av_pkt_->data = data;
    av_pkt_->size = size;
    av_pkt_->flags = 0;
    av_pkt_->pts = pts;
    //send packet to av_codec
    status = avcodec_send_packet(dec_context_, av_pkt_);
    if (status < 0) {
        std::cerr << "Error sending av packet for decoding" << std::endl;
        return 0;
    }
    while (status >= 0) {
        status = avcodec_receive_frame(dec_context_, avframe_);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
            return 0;
        else if (status < 0) {
            std::cerr << "Error during decoding" stc::endl;
            return 0;
        }

        printf("saving frame %3d\n", dec_context_->frame_number);
        fflush(stdout);
        decoded_pic_cnt_++;

        /* the picture is allocated by the decoder. no need to
           free it */
        snprintf(buf, sizeof(buf), "%s-%d", filename, dec_ctx->frame_number);
        //pgm_save(frame->data[0], frame->linesize[0],
        //         frame->width, frame->height, buf);
    }
    if (num_decoded_pics) {
        *num_decoded_pics = decoded_pic_cnt_;
    }

    if (!data || size == 0) {
        packet.flags |= ROCDEC_PKT_ENDOFSTREAM;
    }
    ROCDEC_API_CALL(rocDecParseVideoData(rocdec_parser_, &packet));
    if (num_decoded_pics) {
        *num_decoded_pics = decoded_pic_cnt_;
    }
    return output_frame_cnt_;
}
