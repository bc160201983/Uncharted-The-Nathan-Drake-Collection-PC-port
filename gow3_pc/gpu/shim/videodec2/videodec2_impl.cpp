// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "videodec2_impl.h"

#include "common/alignment.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define ASSERT(x) do { if (!(x)) { std::fputs("STOP: video decoder invariant failed\n",stderr); std::abort(); } } while (0)
#define UNREACHABLE() std::abort()


#include "videodec_error.h"

#include "common/support/avdec.h"
#include "core/memory.h"
#include "gow3_movie.h"

// The last decoded frame's guest range (runtime_videodec.c): the texture cache reports the
// images the game reads from it (diagnostics for garbled or green movies).
extern "C" volatile unsigned long long runtime_video_frame_lo, runtime_video_frame_hi;

namespace Libraries::Videodec2 {

// The chroma plane follows the luma rows (the decoded height aligned to row_align).
static void CopyNV12Data(u8* dst, u64 size, const AVFrame& src, u32 row_align) {
    const u32 pitch=Common::AlignUp<u32>(src.width,64), rows=Common::AlignUp<u32>(src.height,row_align);
    u8* chroma=dst+(u64)pitch*rows;
    runtime_video_frame_lo=(u64)dst;
    runtime_video_frame_hi=(u64)dst+(u64)pitch*rows*3/2;
    Gow3Movie::RecordFrame((u64)dst, (u32)src.width, (u32)src.height, pitch, rows);
    static int logged;
    if (logged++<4) {
        std::printf("Runtime: Videodec2 frame %dx%d pitch %u rows %u into %p (buffer %llu bytes)\n", src.width,
                    src.height, pitch, rows, (void*)dst, (unsigned long long)size);
        std::fflush(stdout);
    }
    for (u32 y=0;y<rows;++y) {
        const u32 source=y<(u32)src.height ? y : src.height-1;
        std::memcpy(dst+(u64)y*pitch,src.data[0]+(u64)source*src.linesize[0],src.width);
        std::memset(dst+(u64)y*pitch+src.width,0,pitch-src.width);
        if (y<rows/2) {
            const u32 row=y<(u32)src.height/2 ? y : src.height/2-1;
            std::memcpy(chroma+(u64)y*pitch,src.data[1]+(u64)row*src.linesize[1],src.width);
            std::memset(chroma+(u64)y*pitch+src.width,128,pitch-src.width);
        }
    }
}


VdecDecoder::VdecDecoder(const OrbisVideodec2DecoderConfigInfo& config_info,
                         const OrbisVideodec2DecoderMemoryInfo& memory_info) {
    const AVCodec* codec = avcodec_find_decoder(
        config_info.codec_type == OrbisVideodec2CodecType::Avc ? AV_CODEC_ID_H264
                                                               : AV_CODEC_ID_HEVC);
    ASSERT(codec);

    m_codec_context = avcodec_alloc_context3(codec);
    ASSERT(m_codec_context);
    m_codec_context->width = config_info.max_frame_width;
    m_codec_context->height = config_info.max_frame_height;
    m_codec_context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    avcodec_open2(m_codec_context, codec, nullptr);
}

VdecDecoder::~VdecDecoder() {
    avcodec_free_context(&m_codec_context);
    sws_freeContext(m_sws_context);
    sws_freeContext(m_scale_context);
    av_frame_free(&m_scaled);
}

s32 VdecDecoder::Decode(const OrbisVideodec2InputData& input_data,
                        OrbisVideodec2FrameBuffer& frame_buffer,
                        OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    if (!input_data.au_data) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER;
    }
    if (input_data.au_size == 0) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    packet->data = (u8*)input_data.au_data;
    packet->size = input_data.au_size;
    packet->pts = input_data.pts_data;
    packet->dts = input_data.dts_data;
    packet->opaque = reinterpret_cast<void*>(input_data.attached_data);

    int ret = avcodec_send_packet(m_codec_context, packet);
    if (ret == AVERROR_EOF) {
        // Attempt to flush buffers and try again.
        avcodec_flush_buffers(m_codec_context);
        ret = avcodec_send_packet(m_codec_context, packet);
    }
    if (ret < 0) {
        ((void)0);
        av_packet_free(&packet);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        ((void)0);
        av_packet_free(&packet);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    ret = avcodec_receive_frame(m_codec_context, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        ((void)0);
        av_packet_free(&packet);
        av_frame_free(&frame);
        return ORBIS_OK;
    } else if (ret < 0) {
        ((void)0);
        av_packet_free(&packet);
        av_frame_free(&frame);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    if (frame->flags & AV_FRAME_FLAG_INTERLACED) {
        ((void)0);
    }

    if (frame->format != AV_PIX_FMT_NV12) {
        AVFrame* nv12_frame = ConvertNV12Frame(*frame);
        ASSERT(nv12_frame);
        av_frame_free(&frame);
        frame = nv12_frame;
    }
    const u32 row_align = 16;

    const bool is_avc = m_codec_context->codec_id == AV_CODEC_ID_H264;
    const u64 info_size =
        is_avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
    const u64 required=((u64)Common::AlignUp<u32>(frame->width,64)*Common::AlignUp<u32>(frame->height,row_align)*3)/2+info_size;
    if (!frame_buffer.frame_buffer || frame_buffer.frame_buffer_size<required) {
        static int logged;
        if (logged++ < 4) {
            std::printf("Runtime: Videodec2 frame buffer too small: %llu bytes for a %dx%d frame (%llu)\n",
                        (unsigned long long)frame_buffer.frame_buffer_size, frame->width, frame->height,
                        (unsigned long long)required);
            std::fflush(stdout);
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
        return ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
    }
    CopyNV12Data((u8*)frame_buffer.frame_buffer,
                           frame_buffer.frame_buffer_size - info_size, *frame, row_align);
    // With a copy scaled to the movie display size the frame is reported at that size: the movie
    // player's conversion shader spans the frame size it is given (gow3_movie.h).
    u32 shown_width = (u32)frame->width, shown_height = (u32)frame->height;
    if (WriteMovieCopy(*frame, (u64)frame_buffer.frame_buffer)) {
        Gow3Movie::DisplaySize(shown_width, shown_height);
    }
    frame_buffer.is_accepted = true;

    const auto width = Common::AlignUp<u32>(shown_width, 16);
    const auto pitch = Common::AlignUp<u32>(shown_width, 64);
    const auto height = Common::AlignUp<u32>(shown_height, row_align);
    const u64 decoded_size = (u64)Common::AlignUp<u32>(frame->width, 64) *
                             Common::AlignUp<u32>(frame->height, row_align) * 3 / 2;

    output_info.is_valid = true;
    output_info.is_error_frame = false;
    output_info.picture_count = 1; // TODO: 2 pictures for interlaced video
    output_info.codec_type = is_avc ? OrbisVideodec2CodecType::Avc : OrbisVideodec2CodecType::Hevc;
    output_info.frame_width = width;
    output_info.frame_pitch = pitch;
    output_info.frame_height = height;
    output_info.frame_buffer = frame_buffer.frame_buffer;
    output_info.frame_buffer_size = decoded_size; // the picture info follows the decoded frame

    // Only set frameFormat and framePitchInBytes if the game uses the newer struct version.
    if (output_info.this_size == sizeof(OrbisVideodec2OutputInfo)) {
        output_info.frame_format = 0;
        output_info.frame_pitch_in_bytes = pitch;
    }

    if (is_avc) {
        auto& picture_info = *(OrbisVideodec2AvcPictureInfo*)((u8*)output_info.frame_buffer +
                                                              output_info.frame_buffer_size);

        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - shown_width;
        picture_info.frame_crop_bottom_offset = height - shown_height;
    } else {
        auto& picture_info = *(OrbisVideodec2HevcPictureInfo*)((u8*)output_info.frame_buffer +
                                                               output_info.frame_buffer_size);
        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - shown_width;
        picture_info.frame_crop_bottom_offset = height - shown_height;
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    return ORBIS_OK;
}

s32 VdecDecoder::Flush(OrbisVideodec2FrameBuffer& frame_buffer,
                       OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    avcodec_send_packet(m_codec_context, nullptr);
    int ret = avcodec_receive_frame(m_codec_context, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_frame_free(&frame);
        return ORBIS_OK;
    } else if (ret < 0) {
        ((void)0);
        av_frame_free(&frame);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    if (frame->format != AV_PIX_FMT_NV12) {
        AVFrame* nv12_frame = ConvertNV12Frame(*frame);
        ASSERT(nv12_frame);
        av_frame_free(&frame);
        frame = nv12_frame;
    }
    const u32 row_align = 16;

    const bool is_avc = m_codec_context->codec_id == AV_CODEC_ID_H264;
    const u64 info_size =
        is_avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
    const u64 required=((u64)Common::AlignUp<u32>(frame->width,64)*Common::AlignUp<u32>(frame->height,row_align)*3)/2+info_size;
    if (!frame_buffer.frame_buffer || frame_buffer.frame_buffer_size<required) {
        static int logged;
        if (logged++ < 4) {
            std::printf("Runtime: Videodec2 frame buffer too small: %llu bytes for a %dx%d frame (%llu)\n",
                        (unsigned long long)frame_buffer.frame_buffer_size, frame->width, frame->height,
                        (unsigned long long)required);
            std::fflush(stdout);
        }
        av_frame_free(&frame);
        return ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
    }
    CopyNV12Data((u8*)frame_buffer.frame_buffer,
                           frame_buffer.frame_buffer_size - info_size, *frame, row_align);
    // With a copy scaled to the movie display size the frame is reported at that size: the movie
    // player's conversion shader spans the frame size it is given (gow3_movie.h).
    u32 shown_width = (u32)frame->width, shown_height = (u32)frame->height;
    if (WriteMovieCopy(*frame, (u64)frame_buffer.frame_buffer)) {
        Gow3Movie::DisplaySize(shown_width, shown_height);
    }
    frame_buffer.is_accepted = true;

    const auto width = Common::AlignUp<u32>(shown_width, 16);
    const auto pitch = Common::AlignUp<u32>(shown_width, 64);
    const auto height = Common::AlignUp<u32>(shown_height, row_align);
    const u64 decoded_size = (u64)Common::AlignUp<u32>(frame->width, 64) *
                             Common::AlignUp<u32>(frame->height, row_align) * 3 / 2;

    output_info.is_valid = true;
    output_info.is_error_frame = false;
    output_info.picture_count = 1; // TODO: 2 pictures for interlaced video
    output_info.codec_type = is_avc ? OrbisVideodec2CodecType::Avc : OrbisVideodec2CodecType::Hevc;
    output_info.frame_width = width;
    output_info.frame_pitch = pitch;
    output_info.frame_height = height;
    output_info.frame_buffer = frame_buffer.frame_buffer;
    output_info.frame_buffer_size = decoded_size; // the picture info follows the decoded frame

    // Only set frameFormat and framePitchInBytes if the game uses the newer struct version.
    if (output_info.this_size == sizeof(OrbisVideodec2OutputInfo)) {
        output_info.frame_format = 0;
        output_info.frame_pitch_in_bytes = pitch;
    }

    if (m_codec_context->codec_id == AV_CODEC_ID_H264) {
        auto& picture_info = *(OrbisVideodec2AvcPictureInfo*)((u8*)output_info.frame_buffer +
                                                              output_info.frame_buffer_size);

        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - shown_width;
        picture_info.frame_crop_bottom_offset = height - shown_height;
    } else {
        auto& picture_info = *(OrbisVideodec2HevcPictureInfo*)((u8*)output_info.frame_buffer +
                                                               output_info.frame_buffer_size);
        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - shown_width;
        picture_info.frame_crop_bottom_offset = height - shown_height;
    }

    av_frame_free(&frame);
    return ORBIS_OK;
}

s32 VdecDecoder::Reset() {
    avcodec_flush_buffers(m_codec_context);
    return ORBIS_OK;
}

// GOW3_MOVIE_SIZE (gow3_movie.h): the frame also scaled to the display size the movie player reads
// it with, into the scratch frame of the game's frame buffer at base. True when written.
bool VdecDecoder::WriteMovieCopy(const AVFrame& frame, u64 base) {
    u32 width = 0, height = 0;
    if (!Gow3Movie::DisplaySize(width, height) || ((u32)frame.width == width && (u32)frame.height == height)) {
        return false;
    }
    u8* dst = Gow3Movie::ScratchFrame(base);
    if (!dst) {
        return false;
    }
    if (!m_scaled) {
        m_scaled = av_frame_alloc();
    }
    if (!m_scaled) {
        return false;
    }
    // The movie keeps its own shape inside the display size: black bars at the top and bottom
    // (16:10) or at the sides (ultrawide) instead of a stretched picture. 16:9 sizes fill it.
    // gow3 1 Nov 2026 (GitHub #145, ultrawide players): GOW3_MOVIE_STRETCH=1 (launcher "Movies:
    // Stretch") fills the whole display instead, as before 0.3.
    static const bool stretch = [] {
        const char* env = std::getenv("GOW3_MOVIE_STRETCH");
        return env && env[0] == '1';
    }();
    u32 fit_width = width & ~1u, fit_height = height & ~1u;
    if (stretch) {
        // the whole display
    } else if ((u64)frame.width * height > (u64)frame.height * width) {
        fit_height = std::min(height, (u32)((u64)width * frame.height / frame.width) & ~1u);
    } else {
        fit_width = std::min(width, (u32)((u64)height * frame.width / frame.height) & ~1u);
    }
    const u32 left = (width - fit_width) / 2 & ~1u, top = (height - fit_height) / 2 & ~1u;
    if ((u32)m_scaled->width != fit_width || (u32)m_scaled->height != fit_height) {
        av_frame_unref(m_scaled);
        m_scaled->format = AV_PIX_FMT_NV12;
        m_scaled->width = (int)fit_width;
        m_scaled->height = (int)fit_height;
        if (av_frame_get_buffer(m_scaled, 0) < 0) {
            av_frame_unref(m_scaled);
            return false;
        }
    }
    m_scale_context = sws_getCachedContext(m_scale_context, frame.width, frame.height, AV_PIX_FMT_NV12,
                                           (int)fit_width, (int)fit_height, AV_PIX_FMT_NV12, SWS_BICUBIC,
                                           nullptr, nullptr, nullptr);
    if (!m_scale_context || sws_scale(m_scale_context, frame.data, frame.linesize, 0, frame.height,
                                      m_scaled->data, m_scaled->linesize) < 0) {
        return false;
    }
    const u32 pitch = Gow3Movie::ScaledPitch(), rows = Gow3Movie::ScaledRows();
    const u32 chroma_pitch = Gow3Movie::ScaledChromaPitch();
    const u64 bytes = Gow3Movie::ScaledFrameBytes();
    // Black (video range) around the picture.
    m_movie_copy.assign(bytes, 128);
    u8* copy = m_movie_copy.data();
    u8* chroma = copy + (u64)pitch * rows;
    std::memset(copy, 16, (u64)pitch * rows);
    for (u32 y = 0; y < fit_height; ++y) {
        std::memcpy(copy + (u64)(top + y) * pitch + left,
                    m_scaled->data[0] + (u64)y * m_scaled->linesize[0], fit_width);
        if (y < fit_height / 2) {
            std::memcpy(chroma + (u64)(top / 2 + y) * chroma_pitch + left,
                        m_scaled->data[1] + (u64)y * m_scaled->linesize[1], fit_width);
        }
    }
    // The rows up to the aligned height repeat the last one (sampling at the bottom edge reads
    // them at heights that aren't a multiple of 16, e.g. 1620).
    for (u32 y = height; y < rows; ++y) {
        std::memcpy(copy + (u64)y * pitch, copy + (u64)(height - 1) * pitch, width);
    }
    for (u32 y = height / 2; y < rows / 2; ++y) {
        std::memcpy(chroma + (u64)y * chroma_pitch, chroma + (u64)(height / 2 - 1) * chroma_pitch, width);
    }
    // The GPU caches learn of the new frame explicitly (one write through the backing view, no
    // page faults per row). Invalidated before (any GPU copy of these pages goes back first)
    // and after the write.
    auto* memory = Core::Memory::Instance();
    const VAddr address = reinterpret_cast<VAddr>(dst);
    memory->InvalidateMemory(address, bytes);
    if (!memory->TryWriteBacking(dst, copy, bytes)) {
        std::memcpy(dst, copy, bytes);
    }
    memory->InvalidateMemory(address, bytes);
    return true;
}

AVFrame* VdecDecoder::ConvertNV12Frame(AVFrame& frame) {
    AVFrame* nv12_frame = av_frame_alloc();
    nv12_frame->pts = frame.pts;
    nv12_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width = frame.width;
    nv12_frame->height = frame.height;
    nv12_frame->sample_aspect_ratio = frame.sample_aspect_ratio;
    nv12_frame->crop_top = frame.crop_top;
    nv12_frame->crop_bottom = frame.crop_bottom;
    nv12_frame->crop_left = frame.crop_left;
    nv12_frame->crop_right = frame.crop_right;
    nv12_frame->opaque = frame.opaque;

    av_frame_get_buffer(nv12_frame, 0);

    if (m_sws_context == nullptr) {
        m_sws_context = sws_getContext(frame.width, frame.height, AVPixelFormat(frame.format),
                                       nv12_frame->width, nv12_frame->height, AV_PIX_FMT_NV12,
                                       SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    }

    const auto res = sws_scale(m_sws_context, frame.data, frame.linesize, 0, frame.height,
                               nv12_frame->data, nv12_frame->linesize);
    if (res < 0) {
        ((void)0);
        return nullptr;
    }

    return nv12_frame;
}

} // namespace Libraries::Videodec2
