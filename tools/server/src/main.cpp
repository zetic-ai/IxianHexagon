// ixian-server: an llama-server compatible HTTP server for the Ixian Hexagon SDK (LFM3 and d1 models on the
// Dragonwing IQ9 NPU). The HTTP server starts first so /health answers 503 "Loading model" while the package loads,
// then the routes open.
#include "server-args.h"
#include "server-engine.h"
#include "server-http.h"
#include "server-log.h"
#include "server-routes.h"
#include "server-utils.h"

#include <atomic>
#include <clocale>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <functional>
#include <mutex>

namespace {

std::function<void(int)> g_shutdown_handler;
std::atomic_flag g_terminating = ATOMIC_FLAG_INIT;

void signal_handler(int signal) {
    if (g_terminating.test_and_set()) {
        fprintf(stderr, "Received second interrupt, terminating immediately.\n");
        _exit(1);
    }
    if (g_shutdown_handler) {
        g_shutdown_handler(signal);
    }
}

}  // namespace

int main(int argc, char ** argv) {
    using namespace ixsrv;

    std::setlocale(LC_NUMERIC, "C");
    signal(SIGPIPE, SIG_IGN);

    server_params params;
    switch (server_params_parse(argc, argv, params)) {
        case args_result::ok: break;
        case args_result::exit_ok: return 0;
        case args_result::exit_error: return 1;
    }

    log_config lcfg;
    lcfg.verbosity = params.log_disable ? 0 : params.verbosity;
    lcfg.prefix = params.log_prefix;
    lcfg.timestamps = params.log_timestamps;
    lcfg.colors = params.log_colors;
    lcfg.file = params.log_file;
    log_init(lcfg);

    SRV_INF("ixian-server %s (%s), Ixian Hexagon SDK %s, engine %s\n", server_version(), build_info(), sdk_version(), IXSRV_ENGINE_NAME);
    SRV_INF("params: %s\n", server_params_describe(params).c_str());
    if (params.n_parallel > 1) {
        SRV_WRN("--parallel %d: the NPU runtime serves one request at a time; other requests wait in the queue\n", params.n_parallel);
    }
    if (params.cors_origins == "*" && params.api_keys.empty()) {
        SRV_WRN("%s", "security: no API key is set and CORS allows all origins\n");
    }

    engine eng(params);
    server_routes routes(eng, params);
    http_server http(params);

    if (!http.init()) {
        SRV_ERR("%s", "failed to initialize the HTTP server\n");
        return 1;
    }
    routes.register_routes(http);

    // serve /health (503) while the model loads
    if (!http.start()) {
        SRV_ERR("%s", "exiting due to HTTP server error\n");
        return 1;
    }
    for (const auto & address : http.listening_addresses) {
        SRV_INF("listening on %s\n", address.c_str());
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    g_shutdown_handler = [&](int) {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
        cv.notify_all();
    };
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    try {
        SRV_INF("loading %s ...\n", params.model_path.c_str());
        eng.load();
    } catch (const std::exception & e) {
        SRV_ERR("%s\n", e.what());
        http.stop();
        http.join();
        return 1;
    }
    http.is_ready.store(true);
    SRV_INF("%s", "model loaded, the API is ready\n");

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return stop; });
    }

    SRV_INF("%s", "shutting down ...\n");
    http.stop();
    eng.shutdown();
    http.join();
    log_flush();
    return 0;
}
