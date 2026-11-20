#include "server-log.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

namespace ixsrv {

namespace {

struct log_state {
    log_config cfg;
    std::mutex mutex;
    FILE * file = nullptr;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

log_state & state() {
    static log_state s;
    return s;
}

const char * level_prefix(int level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "E ";
        case LOG_LEVEL_WARN:  return "W ";
        case LOG_LEVEL_INFO:  return "I ";
        case LOG_LEVEL_TRACE: return "T ";
        case LOG_LEVEL_DEBUG: return "D ";
        default:              return "  ";
    }
}

const char * level_color(int level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "\033[31m";
        case LOG_LEVEL_WARN:  return "\033[35m";
        case LOG_LEVEL_DEBUG: return "\033[33m";
        case LOG_LEVEL_TRACE: return "\033[90m";
        default:              return "";
    }
}

}  // namespace

void log_init(const log_config & cfg) {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.cfg = cfg;
    if (s.file) {
        fclose(s.file);
        s.file = nullptr;
    }
    if (!cfg.file.empty()) {
        s.file = fopen(cfg.file.c_str(), "a");
        if (!s.file) {
            fprintf(stderr, "log: cannot open %s\n", cfg.file.c_str());
        }
    }
}

void log_set_verbosity(int verbosity) {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.cfg.verbosity = verbosity;
}

int log_get_verbosity() {
    return state().cfg.verbosity;
}

void log_flush() {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    fflush(stderr);
    if (s.file) {
        fflush(s.file);
    }
}

void log_printf(int level, const char * fmt, ...) {
    auto & s = state();
    if (level > s.cfg.verbosity) {
        return;
    }

    char buf[4096];
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::vector<char> big;
    const char * text = buf;
    if (n >= (int) sizeof(buf)) {
        big.resize((size_t) n + 1);
        vsnprintf(big.data(), big.size(), fmt, args_copy);
        text = big.data();
    }
    va_end(args_copy);

    std::string line;
    if (s.cfg.timestamps) {
        const auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.t0).count();
        char ts[32];
        snprintf(ts, sizeof(ts), "%10.3f ", dt);
        line += ts;
    }
    if (s.cfg.prefix) {
        line += level_prefix(level);
    }
    line += text;

    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cfg.colors) {
        const char * c = level_color(level);
        if (*c) {
            fputs(c, stderr);
            fputs(line.c_str(), stderr);
            fputs("\033[0m", stderr);
        } else {
            fputs(line.c_str(), stderr);
        }
    } else {
        fputs(line.c_str(), stderr);
    }
    if (level <= LOG_LEVEL_WARN) {
        fflush(stderr);
    }
    if (s.file) {
        fputs(line.c_str(), s.file);
        fflush(s.file);
    }
}

}  // namespace ixsrv
