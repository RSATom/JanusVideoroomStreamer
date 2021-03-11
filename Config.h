#pragma once

#include "Client/Config.h"

#include <string>
#include <deque>
#include <map>

#include <spdlog/common.h>

#include "RtcStreaming/GstRtcStreaming/Types.h"


struct StreamerConfig
{
    enum class Type {
        Test,
        Pipeline,
        ReStreamer,
    };

    Type type = Type::Test;
    std::string source;
    GstRtcStreaming::Videocodec videocodec = GstRtcStreaming::Videocodec::vp8;
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
