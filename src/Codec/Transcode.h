//
// Created by xzl on 2020/11/2.
//

#ifndef ZLMEDIAKIT_TRANSCODE_H
#define ZLMEDIAKIT_TRANSCODE_H

#include "Common/MediaSink.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "libavutil/avutil.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#ifdef __cplusplus
}
#endif

class FFmpegFrame {
public:
    using Ptr = std::shared_ptr<FFmpegFrame>;
    FFmpegFrame();
    ~FFmpegFrame() = default;

    AVFrame *get() const;

private:
    std::shared_ptr<AVFrame> _frame;
};

class FFmpegDecoder : public FrameWriterInterface {
public:
    using Ptr = std::shared_ptr<FFmpegDecoder>;
    using onDec = function<void(const FFmpegFrame::Ptr &)>;

    FFmpegDecoder(const Track::Ptr &track);
    ~FFmpegDecoder() {}

    void inputFrame(const Frame::Ptr &frame) override;
    void setOnDecode(onDec cb);

private:
    void onDecode(const FFmpegFrame::Ptr &frame);

private:
    onDec _cb;
    ResourcePool<FFmpegFrame> _frame_pool;
    std::shared_ptr<AVCodecContext> _context;
};

class Transcode : public MediaSinkInterface {
public:
    Transcode(MediaSinkInterface *sink);
    ~Transcode() override;

    void addTrack(const Track::Ptr & track) override;
    void inputFrame(const Frame::Ptr &frame) override;

private:
    MediaSinkInterface *_sink;
};

#endif //ZLMEDIAKIT_TRANSCODE_H
