#include "Log.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>


static std::shared_ptr<spdlog::logger> Logger;

void InitJanusClientLogger(spdlog::level::level_enum level)
{
    spdlog::sink_ptr sink = std::make_shared<spdlog::sinks::stdout_sink_st>();

    Logger = std::make_shared<spdlog::logger>("JanusVideoroomClient", sink);

    Logger->set_level(level);
}

const std::shared_ptr<spdlog::logger>& ClientLog()
{
    if(!Logger)
#ifndef NDEBUG
        InitJanusClientLogger(spdlog::level::debug);
#else
        InitJanusClientLogger(spdlog::level::info);
#endif

    return Logger;
}
