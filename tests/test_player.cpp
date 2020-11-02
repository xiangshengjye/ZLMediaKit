/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <signal.h>
#include <iostream>
#include "Player/MediaPlayer.h"
#include "Util/logger.h"
#include "YuvDisplayer.h"
#include "Codec/Transcode.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

int main(int argc, char *argv[]) {
    static char *url = argv[1];
    //设置退出信号处理函数
    signal(SIGINT, [](int) { SDLDisplayerHelper::Instance().shutdown(); });
    //设置日志
    Logger::Instance().add(std::make_shared<ConsoleChannel>());

    if (argc != 3) {
        ErrorL << "\r\n测试方法：./test_player rtxp_url rtp_type\r\n"
               << "例如：./test_player rtsp://admin:123456@127.0.0.1/live/0 0\r\n"
               << endl;
        return 0;
    }

    FFmpegDecoder::Ptr video_decoder, audio_decoder;
    std::shared_ptr<YuvDisplayer> displayer;
    MediaPlayer::Ptr player(new MediaPlayer());
    weak_ptr<MediaPlayer> weakPlayer = player;

    player->setOnPlayResult([weakPlayer, &video_decoder, &audio_decoder, &displayer](const SockException &ex) {
        InfoL << "OnPlayResult:" << ex.what();
        auto strongPlayer = weakPlayer.lock();
        if (ex || !strongPlayer) {
            return;
        }

        {
            auto videoTrack = strongPlayer->getTrack(TrackVideo, false);
            if (videoTrack) {
                video_decoder = std::make_shared<FFmpegDecoder>(videoTrack);
                video_decoder->setOnDecode([&displayer](const FFmpegFrame::Ptr &frame) {
                    if (!displayer) {
                        displayer = std::make_shared<YuvDisplayer>(nullptr, url);
                    }
                    SDLDisplayerHelper::Instance().doTask([frame, displayer]() {
                        displayer->displayYUV(frame->get());
                        return true;
                    });
                });
                videoTrack->addDelegate(std::make_shared<FrameWriterInterfaceHelper>([&video_decoder](const Frame::Ptr &frame) {
                    video_decoder->inputFrame(frame);
                }));
            }
        }

        {
            auto audioTrack = strongPlayer->getTrack(TrackAudio, false);
            if (audioTrack) {
                audio_decoder = std::make_shared<FFmpegDecoder>(audioTrack);
                audio_decoder->setOnDecode([](const FFmpegFrame::Ptr &frame){
                    //todo, play audio
                });
                audioTrack->addDelegate(std::make_shared<FrameWriterInterfaceHelper>([&audio_decoder](const Frame::Ptr &frame) {
                    audio_decoder->inputFrame(frame);
                }));
            }
        }
    });

    player->setOnShutdown([](const SockException &ex) {
        ErrorL << "OnShutdown:" << ex.what();
    });
    (*player)[kRtpType] = atoi(argv[2]);
    player->play(argv[1]);
    SDLDisplayerHelper::Instance().runLoop();
    return 0;
}

