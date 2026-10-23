/**
 * @file include/logging/Log.hpp
 * @brief Convenience macros for operational logging.
 */
#pragma once

#include "logging/Logger.hpp"
#include <spdlog/common.h>

#ifndef SPDLOG_FUNCTION
#define SPDLOG_FUNCTION __func__
#endif

#define POLYGRAPH_LOG_IMPL(level, ...)                                                            \
    ::polygraph::logging::log(                                                                    \
        ::spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, (level), __VA_ARGS__)

#define LOG_TRACE(...) POLYGRAPH_LOG_IMPL(::spdlog::level::trace, __VA_ARGS__)
#define LOG_DEBUG(...) POLYGRAPH_LOG_IMPL(::spdlog::level::debug, __VA_ARGS__)
#define LOG_INFO(...) POLYGRAPH_LOG_IMPL(::spdlog::level::info, __VA_ARGS__)
#define LOG_WARN(...) POLYGRAPH_LOG_IMPL(::spdlog::level::warn, __VA_ARGS__)
#define LOG_ERROR(...) POLYGRAPH_LOG_IMPL(::spdlog::level::err, __VA_ARGS__)
#define LOG_CRITICAL(...) POLYGRAPH_LOG_IMPL(::spdlog::level::critical, __VA_ARGS__)
