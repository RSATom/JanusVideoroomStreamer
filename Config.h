#pragma once

#include <string>
#include <deque>
#include <map>

#include <spdlog/common.h>

#include "RtStreaming/GstRtStreaming/Types.h"


struct StreamerConfig
{
    enum class Type {
        Test,
        Pipeline,
        ReStreamer,
    };

    Type type = Type::Test;
    std::string source;
    GstRtStreaming::Videocodec videocodec = GstRtStreaming::Videocodec::vp8;
};

struct Config
{
    spdlog::level::level_enum logLevel = spdlog::level::info;
    spdlog::level::level_enum lwsLogLevel = spdlog::level::warn;

    std::deque<std::string> iceServers;

    std::string janusUrl;
    std::string cipherList;
    std::string display;
    int room;

    unsigned reconnectTimeout;
    bool trackParticipants = false;

    StreamerConfig streamer;
};
