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

CodecId FFmpegFrame::getSourceCodec() const{
    return _src_codec;
}

///////////////////////////////////////////////////////////////////////////

template<bool decoder = true, typename ...ARGS>
AVCodec *getCodecByName(ARGS ...names);

template<bool decoder = true, typename ...ARGS>
AVCodec *getCodecByName(const char *name) {
    auto codec = decoder ? avcodec_find_decoder_by_name(name) : avcodec_find_encoder_by_name(name);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << name;
    }
    return codec;
}

template<bool decoder = true, typename ...ARGS>
AVCodec *getCodecByName(const char *name, ARGS ...names) {
    auto codec = getCodecByName<decoder>(names...);
    if (codec) {
        return codec;
    }
    return getCodecByName<decoder>(name);
}

template<bool decoder = true>
AVCodec *getCodec(enum AVCodecID id) {
    auto codec = decoder ? avcodec_find_decoder(id) : avcodec_find_encoder(id);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << avcodec_get_name(id);
    }
    return codec;
}

template<bool decoder = true, typename ...ARGS>
AVCodec *getCodec(enum AVCodecID id, ARGS ...names) {
    auto codec = getCodecByName<decoder>(names...);
    if (codec) {
        return codec;
    }
    return getCodec<decoder>(id);
}

FFmpegDecoder::FFmpegDecoder(const Track::Ptr &track) {
    _frame_pool.setSize(8);
    avcodec_register_all();
    AVCodec *codec;
    switch (track->getCodecId()) {
        case CodecH264:
            codec = getCodec(AV_CODEC_ID_H264, "h264_videotoolbox");
            break;
        case CodecH265:
            codec = getCodec(AV_CODEC_ID_HEVC, "hevc_videotoolbox");
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

    int ret = avcodec_open2(_context.get(), codec, NULL);
    if (ret < 0) {
        throw std::runtime_error(string("打开解码器失败:") + av_err2str(ret));
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
    int out_flag = 0;
    auto len = avcodec_decode_video2(_context.get(), out_frame->get(), &out_flag, &pkt);
    if(len < 0){
        WarnL << "avcodec_decode_video2 failed:" << av_err2str(len);
        return;
    }
    if (out_flag) {
        out_frame->_src_codec = frame->getCodecId();
        onDecode(out_frame);
    }
}

void FFmpegDecoder::setOnDecode(FFmpegDecoder::onDec cb) {
    _cb = std::move(cb);
}

void FFmpegDecoder::onDecode(const FFmpegFrame::Ptr &frame){
    //todo test encoder
    static FFmpegEncoder encoder(CodecH264);
    encoder.inputFrame(frame);
    if (_cb) {
        _cb(frame);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

FFmpegEncoder::FFmpegEncoder(CodecId type){
    _codec = type;
    avcodec_register_all();
    AVCodec *codec;
    switch (type) {
        case CodecH264:
            codec = getCodec<false>(AV_CODEC_ID_H264, "libx264", "h264_videotoolbox");
            break;
        case CodecH265:
            codec = getCodec<false>(AV_CODEC_ID_HEVC, "libx265", "hevc_videotoolbox");
            break;
        case CodecAAC:
            codec = getCodec<false>(AV_CODEC_ID_AAC);
            break;
        case CodecG711A:
            codec = getCodec<false>(AV_CODEC_ID_PCM_ALAW);
            break;
        case CodecG711U:
            codec = getCodec<false>(AV_CODEC_ID_PCM_MULAW);
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
        throw std::runtime_error("创建编码器失败");
    }

    //保存AVFrame的引用
    _context->refcounted_frames = 1;
    _context->time_base.den = 1;
    _context->time_base.num = 1000;
    _context->bit_rate = 4 * 1024 * 1024;
    _context->gop_size = 50;
    _context->max_b_frames = 0;
    _context->pix_fmt = AV_PIX_FMT_YUV420P;
    _context->width = 1080;
    _context->height = 720;
    int ret = avcodec_open2(_context.get(), codec, NULL);
    if (ret < 0) {
        throw std::runtime_error(string("打开编码器失败:") + av_err2str(ret));
    }
}

FFmpegEncoder::~FFmpegEncoder(){

}

void FFmpegEncoder::inputFrame(const FFmpegFrame::Ptr &frame){
    if (getTrackType(frame->getSourceCodec()) != getTrackType(_codec)) {
        throw std::invalid_argument("音视频类型不一致, 无法编码");
    }
    AVPacket pkt;
    av_init_packet(&pkt);

    int got_packet = 0;
    auto ret = avcodec_encode_video2(_context.get(), &pkt, frame->get(), &got_packet);
    if (ret < 0) {
        WarnL << "avcodec_encode_video2 failed:" << av_err2str(ret);
        return;
    }
    if (got_packet) {
       InfoL << pkt.data << " " << pkt.buf;
    }
}

void FFmpegEncoder::onEncode(const Frame::Ptr &frame){

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
