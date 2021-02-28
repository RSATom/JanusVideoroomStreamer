#pragma once

#include "Client/Config.h"

#include <string>
#include <deque>
#include <map>

#include <spdlog/common.h>

#include "GstStreaming/Types.h"


struct StreamerConfig
{
    enum class Type {
        Test,
        ReStreamer,
    };

    Type type;
    std::string uri;
    GstStreaming::Videocodec videocodec = GstStreaming::Videocodec::h264;
};

struct Config
{
    spdlog::level::level_enum logLevel = spdlog::level::info;
    spdlog::level::level_enum lwsLogLevel = spdlog::level::warn;

    std::deque<std::string> iceServers;

    std::string janusUrl;
    std::string display;
    int room;

    unsigned reconnectTimeout;
    bool trackParticipants = false;

    StreamerConfig streamer;
};
