// Ixian Hexagon server - logging (printf style, levels, optional file, thread-safe).
#pragma once

#include <cstdarg>
#include <string>

namespace ixsrv {

enum log_level : int {
    LOG_LEVEL_NONE  = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_INFO  = 3,
    LOG_LEVEL_TRACE = 4,
    LOG_LEVEL_DEBUG = 5,
};

struct log_config {
    int  verbosity  = LOG_LEVEL_INFO;  // messages above this level are dropped
    bool prefix     = false;           // "I ", "W " ... level prefix
    bool timestamps = false;           // "12.345 " seconds since start
    bool colors     = false;
    std::string file;                  // also append to this file when set
};

void log_init(const log_config & cfg);
void log_set_verbosity(int verbosity);
int  log_get_verbosity();
void log_flush();

void log_printf(int level, const char * fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace ixsrv

#define SRV_ERR(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_ERROR, "srv  %12.*s: " fmt, 12, __func__, __VA_ARGS__)
#define SRV_WRN(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_WARN,  "srv  %12.*s: " fmt, 12, __func__, __VA_ARGS__)
#define SRV_INF(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_INFO,  "srv  %12.*s: " fmt, 12, __func__, __VA_ARGS__)
#define SRV_TRC(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_TRACE, "srv  %12.*s: " fmt, 12, __func__, __VA_ARGS__)
#define SRV_DBG(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_DEBUG, "srv  %12.*s: " fmt, 12, __func__, __VA_ARGS__)

// no function prefix
#define LOG_ERR(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_ERROR, fmt, __VA_ARGS__)
#define LOG_WRN(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_WARN,  fmt, __VA_ARGS__)
#define LOG_INF(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_INFO,  fmt, __VA_ARGS__)
#define LOG_DBG(fmt, ...) ::ixsrv::log_printf(::ixsrv::LOG_LEVEL_DEBUG, fmt, __VA_ARGS__)
