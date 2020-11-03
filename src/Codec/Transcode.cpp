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
    _context->pix_fmt = AV_PIX_FMT_YUV420P;

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

FFmpegSws::FFmpegSws(AVPixelFormat output){
    _target_format = output;
    _frame_pool.setSize(8);
}

FFmpegSws::~FFmpegSws(){
    if (_ctx) {
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame){
    if (frame->get()->format == _target_format) {
        //不转格式
        return frame;
    }
    if(!_ctx){
        _target_width = frame->get()->width;
        _target_height = frame->get()->height;
        _ctx = sws_getContext(frame->get()->width, frame->get()->height, (enum AVPixelFormat) frame->get()->format,
                              _target_width, _target_height, _target_format,
                              SWS_BICUBIC, NULL, NULL, NULL);
    }
    if (_ctx) {
        FFmpegFrame::Ptr out = _frame_pool.obtain();
        if (!out->get()->data[0]) {
            auto out_buffer = new uint8_t[avpicture_get_size(_target_format, _target_width, _target_height)];
            avpicture_fill((AVPicture *) out->get(), out_buffer, _target_format, _target_width, _target_height);
        }
        sws_scale(_ctx, frame->get()->data, frame->get()->linesize, 0, frame->get()->height,
                  out->get()->data, out->get()->linesize);
        out->get()->format = _target_format;
        out->get()->width = _target_width;
        out->get()->height = _target_height;
        return out;
    }

    WarnL << "sws_getContext failed";
    return frame;
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
        throw std::runtime_error("未找到编码器");
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
    _context->time_base.den = 25;
    _context->time_base.num = 1;
    _context->bit_rate = 4 * 1024 * 1024;
    _context->gop_size = 50;
    _context->max_b_frames = 0;
    _context->pix_fmt = AV_PIX_FMT_YUV420P;
    _context->width = 1080;
    _context->height = 720;
    _context->codec_type = getTrackType(type) == TrackVideo ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    int ret = avcodec_open2(_context.get(), codec, NULL);
    if (ret < 0) {
        throw std::runtime_error(string("打开编码器失败:") + av_err2str(ret));
    }
    auto pix_fmt = codec->pix_fmts;
    while (pix_fmt && *pix_fmt != AV_PIX_FMT_NONE) {
        _supported_pix_fmts.emplace(*pix_fmt);
        ++pix_fmt;
    }
    assert(_supported_pix_fmts.size() > 0);
}

FFmpegEncoder::~FFmpegEncoder(){

}

void FFmpegEncoder::inputFrame(const FFmpegFrame::Ptr &frame_in){
    if (getTrackType(frame_in->getSourceCodec()) != getTrackType(_codec)) {
        throw std::invalid_argument("音视频类型不一致, 无法编码");
    }
    if (!_sws) {
        if (_supported_pix_fmts.find((enum AVPixelFormat) frame_in->get()->format) != _supported_pix_fmts.end()) {
            //优先不转换图片类型
            _sws = std::make_shared<FFmpegSws>((enum AVPixelFormat) frame_in->get()->format);
        } else if (_supported_pix_fmts.find(AV_PIX_FMT_YUV420P) != _supported_pix_fmts.end()) {
            //其次选择AV_PIX_FMT_YUV420P
            _sws = std::make_shared<FFmpegSws>(AV_PIX_FMT_YUV420P);
        } else {
            //没最佳选择，随便选择一种
            _sws = std::make_shared<FFmpegSws>(*_supported_pix_fmts.begin());
        }
    }
    if (!_sws) {
        InfoL << "创建swscale对象失败";
        return;
    }

    auto frame = _sws->inputFrame(frame_in);
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
