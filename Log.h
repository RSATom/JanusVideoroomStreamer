#pragma once

#include <memory>

#include <spdlog/spdlog.h>


void InitJanusClientLogger(spdlog::level::level_enum level);

const std::shared_ptr<spdlog::logger>& ClientLog();
