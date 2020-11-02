//
// Created by xzl on 2020/11/2.
//

#include "Transcode.h"

FFmpegFrame::FFmpegFrame() {
    _frame.reset(av_frame_alloc(), [](AVFrame *ptr) {
        av_frame_unref(ptr);
        av_frame_free(&ptr);
    });
}

AVFrame *FFmpegFrame::get() const{
    return _frame.get();
}

///////////////////////////////////////////////////////////////////////////

template<typename ...ARGS>
AVCodec *getCodecByName(ARGS ...names);

template<typename ...ARGS>
AVCodec *getCodecByName(const char *name) {
    auto codec = avcodec_find_decoder_by_name(name);
    if (codec) {
        InfoL << "got decoder:" << name;
    }
    return codec;
}

template<typename ...ARGS>
AVCodec *getCodecByName(const char *name, ARGS ...names) {
    auto codec = getCodecByName(names...);
    if (codec) {
        return codec;
    }
    return getCodecByName(name);
}

AVCodec *getCodec(enum AVCodecID id) {
    auto codec = avcodec_find_decoder(id);
    if (codec) {
        InfoL << "got decoder:" << avcodec_get_name(id);
    }
    return codec;
}

template<typename ...ARGS>
AVCodec *getCodec(enum AVCodecID id, ARGS ...names) {
    auto codec = getCodecByName(names...);
    if (codec) {
        return codec;
    }
    return getCodec(id);
}

FFmpegDecoder::FFmpegDecoder(const Track::Ptr &track) {
    _frame_pool.setSize(8);
    avcodec_register_all();
    AVCodec *codec;
    switch (track->getCodecId()) {
        case CodecH264:
            codec = getCodec(AV_CODEC_ID_H264, "libx264", "h264_videotoolbox");
            break;
        case CodecH265:
            codec = getCodec(AV_CODEC_ID_HEVC, "libx265", "hevc_videotoolbox");
            break;
        case CodecAAC:
            codec = getCodec(AV_CODEC_ID_AAC);
            break;
        case CodecG711A:
            codec = getCodec(AV_CODEC_ID_PCM_ALAW);
            break;
        case CodecG711U:
            codec = getCodec(AV_CODEC_ID_PCM_MULAW);
            break;
        default: break;
    }

    if (!codec) {
        throw std::runtime_error("未找到解码器");
    }

    _context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) {
        avcodec_close(ctx);
        avcodec_free_context(&ctx);
    });

    if (!_context) {
        throw std::runtime_error("创建解码器失败");
    }

    //保存AVFrame的引用
    _context->refcounted_frames = 1;

    if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
        /* we do not send complete frames */
        _context->flags |= AV_CODEC_FLAG_TRUNCATED;
    }

    if (avcodec_open2(_context.get(), codec, NULL) < 0) {
        throw std::runtime_error("打开编码器失败");
    }
}

void FFmpegDecoder::inputFrame(const Frame::Ptr &frame) {
    AVPacket pkt;
    av_init_packet(&pkt);

    pkt.data = (uint8_t *) frame->data();
    pkt.size = frame->size();
    pkt.dts = frame->dts();
    pkt.pts = frame->pts();

    FFmpegFrame::Ptr out_frame = _frame_pool.obtain();
    int out_flag;
    auto len = avcodec_decode_video2(_context.get(), out_frame->get(), &out_flag, &pkt);
    if(len < 0){
        WarnL << "avcodec_decode_video2 failed:" << len;
        return;
    }
    if (out_flag) {
        onDecode(out_frame);
    }
}

void FFmpegDecoder::setOnDecode(FFmpegDecoder::onDec cb) {
    _cb = std::move(cb);
}

void FFmpegDecoder::onDecode(const FFmpegFrame::Ptr &frame){
    if (_cb) {
        _cb(frame);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

Transcode::Transcode(MediaSinkInterface *sink) {
    _sink = sink;
}

Transcode::~Transcode() {

}

void Transcode::addTrack(const Track::Ptr &track) {

}

void Transcode::inputFrame(const Frame::Ptr &frame) {

}
