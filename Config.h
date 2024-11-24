#pragma once

#include <string>
#include <deque>
#include <map>

#include <spdlog/common.h>

#include "RtStreaming/GstRtStreaming/Types.h"


struct Config
{
    spdlog::level::level_enum logLevel = spdlog::level::info;
    spdlog::level::level_enum lwsLogLevel = spdlog::level::warn;

    std::deque<std::string> iceServers;

    std::string janusUrl;
    std::string cipherList;

    unsigned reconnectTimeout;
};
