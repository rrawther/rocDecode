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

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #if USE_AVCODEC_GREATER_THAN_58_134
        #include <libavcodec/bsf.h>
    #endif
}
#include "rocvideodecode/roc_video_dec.h"       // for derived class

typedef struct DecFrameBufferFFMpeg_ {
    AVFrame *av_frame_ptr;      /**< av_frame pointer for the decoded frame */
    uint8_t *frame_ptr;       /**< host/device memory pointer for the decoded frame depending on mem_type*/
    int64_t  pts;             /**<  timestamp for the decoded frame */
    int picture_index;         /**<  surface index for the decoded frame */
} DecFrameBufferFFMpeg;


class FFMpegVideoDecoder: public RocVideoDecoder {
    public:
        /**
         * @brief Construct a new FFMpegVideoDecoder object
         * 
         * @param num_threads : number of cpu threads for the decoder
         * @param out_mem_type : out_mem_type for the decoded surface
         * @param codec 
         * @param force_zero_latency 
         * @param p_crop_rect 
         * @param extract_user_SEI_Message 
         * @param disp_delay 
         * @param max_width 
         * @param max_height 
         * @param clk_rate 
         */
        FFMpegVideoDecoder(int num_threads,  OutputSurfaceMemoryType out_mem_type, rocDecVideoCodec codec, bool force_zero_latency = false,
                          const Rect *p_crop_rect = nullptr, bool extract_user_SEI_Message = false, uint32_t disp_delay = 0, int max_width = 0, int max_height = 0,
                          uint32_t clk_rate = 1000);
        /**
         * @brief destructor
         * 
         */
        ~FFMpegVideoDecoder();

        /**
         * @brief this function decodes a frame and returns the number of frames avalable for display
         * 
         * @param data - pointer to the compressed data buffer that is to be decoded
         * @param size - size of the data buffer in bytes
         * @param pts - presentation timestamp
         * @param flags - video packet flags
         * @param num_decoded_pics - nummber of pictures decoded in this call
         * @return int - num of frames to display
         */
        int DecodeFrame(const uint8_t *data, size_t size, int pkt_flags, int64_t pts = 0, int *num_decoded_pics = nullptr);

protected:
        /**
         *   @brief  This function gets called when a sequence is ready to be decoded. The function also gets called
             when there is format change
        */
        int HandleVideoSequence(RocdecVideoFormat *p_video_format);

        /**
         *   @brief  This function gets called when a picture is ready to be decoded. cuvidDecodePicture is called from this function
         *   to decode the picture
         */
        int HandlePictureDecode(RocdecPicParams *p_pic_params);

        /**
         *   @brief  This function gets called after a picture is decoded and available for display. Frames are fetched and stored in 
             internal buffer
        */
        int HandlePictureDisplay(RocdecParserDispInfo *p_disp_info);
        /**
         *   @brief  This function gets called when all unregistered user SEI messages are parsed for a frame
         */
        int GetSEIMessage(RocdecSeiMessageInfo *p_sei_message_info);

        /**
         *   @brief  This function reconfigure decoder if there is a change in sequence params.
         */
        int ReconfigureDecoder(RocdecVideoFormat *p_video_format);

    private:
        void PushPacket(AVPacket *pkt){
            std::unique_lock<std::mutex> lock(mtx_pkt_q_);
            av_packet_q_.push(pkt);
            cv_pkt_.notify_one();
        }
        
        AVPacket *PopPacket(){
            std::unique_lock<std::mutex> lock(mtx_pkt_q_);
            cv_pkt_.wait(lock, [&] { return !av_packet_q_.empty(); });
            AVPacket *pkt = av_packet_q_.front();
            av_packet_q_.pop();
            return pkt;
        }

        void PushFrame(AVFrame *av_frame){
            std::unique_lock<std::mutex> lock(mtx_frame_q_);
            av_frame_q_.push(av_frame);
            cv_frame_.notify_one();
        };

        AVFrame *PopFrame(){
            std::unique_lock<std::mutex> lock(mtx_frame_q_);
            cv_pkt_.wait(lock, [&] { return !av_frame_q_.empty(); });
            AVFrame *p_frame = av_frame_q_.front();
            av_frame_q_.pop();
            return av_frame_q_;
        }

        typedef enum { CMD_ABORT, CMD_DECODE } CommandType;
        typedef enum { STATUS_SUCCESS = 0, STATUS_FAILURE = -1 } StatusType;
        std::thread *ffmpeg_decoder_thread_ = nullptr;
        std::queue<AVPacket *> av_packet_q_;        // queue for compressed packets
        std::queue<AVFrame *> av_frame_q_;
        std::mutex mtx_pkt_q_, mtx_frame_q_;               //for command and status
        std::condition_variable cv_pkt_, cv_frame_;     //for command and status
        std::atomic<bool> end_of_stream_ = false;
        // Variables for FFMpeg decoding
        AVCodecContext * dec_context_ = nullptr;
        AVPixelFormat decoder_pixel_format_;
        AVFrame *avframe_ = nullptr;
        AVPacket *av_pkt_ = nullptr;
        AVCodec *decoder_ = nullptr;
        AVFormatContext * formatContext = nullptr;
        AVInputFormat * inputFormat = nullptr;
        AVStream *video = nullptr;
        int64_t last_packet_pts_ = 0;
};
