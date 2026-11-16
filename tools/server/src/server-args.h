// Ixian Hexagon server - command line and environment (LLAMA_ARG_*), llama-server compatible.
//
// Every flag of llama-server that has a meaning here is accepted under the same name and environment variable, so
// an existing launch script keeps working. Flags that only make sense for llama.cpp (GPU layers, batch sizes, draft
// models, LoRA, ...) are accepted and ignored with a note in the log. A few flags are specific to this server
// (--target, --image-fit, --model-type, ...) and are documented in the usage text.
#pragma once

#include "server-media.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace ixsrv {

struct server_params {
    // model (one package per process, like the SDK)
    std::string model_path;           // -m: a model root (one directory per target) or a target directory
    std::string model_type = "auto";  // --model-type auto | llm | decision
    std::string target;               // --target: "" = detect (v73, both NSPs), "v73-1nsp"
    std::string native_lib_dir;       // --native-lib-dir: where libhexrt_v73_skel.so lives ("" = next to the SDK library)
    bool verify_checksums = false;    // --verify
    bool warmup           = true;     // --warmup / --no-warmup
    std::string host_affinity;        // --host-affinity: "" = package, "off", or a hex CPU mask
    std::set<std::string> model_alias;  // -a, --alias (comma separated); the id of GET /models is the first one
    std::set<std::string> model_tags;   // --tags

    // generation defaults (per-request values override these; -1 = the package's generation_config)
    int32_t n_ctx          = 0;    // -c: LLMOptions::max_context, 0 = the package default (2048)
    int32_t n_predict      = -1;   // -n: default max_tokens; -1 = as many as fit the context
    float   temperature    = -1;   // --temp
    int32_t top_k          = -1;   // --top-k
    float   top_p          = -1;   // --top-p
    float   min_p          = -1;   // --min-p
    float   repeat_penalty = -1;   // --repeat-penalty
    int32_t repeat_last_n  = 64;   // --repeat-last-n
    int64_t seed           = -1;   // -s: -1 = random
    bool    cache_prompt   = true; // --cache-prompt / --no-cache-prompt

    // chat behaviour
    std::string reasoning        = "auto";  // --reasoning on|off|auto (auto = off: thinking is opt-in on the NPU)
    std::string reasoning_format = "auto";  // --reasoning-format none|deepseek|deepseek-legacy|auto
    int32_t     reasoning_budget = -1;      // --reasoning-budget (accepted, the SDK has no budget control)
    std::string chat_template_kwargs;       // --chat-template-kwargs JSON object
    bool        prefill_assistant = true;   // --prefill-assistant / --no-prefill-assistant (prefill is not possible, see README)
    std::string tts_lang;                   // --tts-lang: ISO 639-1
    std::string tts_voice = "female";       // --tts-voice: female | male

    // media
    std::string media_path;                 // --media-path: directory for file:// media URLs ("" = disabled)
    bool        media_allow_private_urls = false;  // --media-allow-private-urls
    ImageFit    image_fit = ImageFit::Letterbox;   // --image-fit letterbox|stretch|crop: how images reach 512 x 512
    std::string video_ffmpeg_dir;           // --video-ffmpeg-dir: where ffmpeg / ffprobe live (default: PATH)
    double      video_fps = 2.0;            // --video-fps

    // HTTP
    std::vector<std::string> hostnames = {"127.0.0.1"};  // --host, comma separated; a path ending in .sock is a UNIX socket
    int         port = 9931;                // --port
    bool        reuse_port = false;         // --reuse-port
    std::string api_prefix;                 // --api-prefix
    std::vector<std::string> api_keys;      // --api-key (comma separated), --api-key-file
    std::string ssl_file_key;               // --ssl-key-file
    std::string ssl_file_cert;              // --ssl-cert-file
    int         timeout_read  = 3600;       // -to
    int         timeout_write = 3600;
    int         n_threads_http = -1;        // --threads-http
    int         sse_ping_interval = 30;     // --sse-ping-interval (-1 disables)
    std::string cors_origins = "*";         // --cors-origins
    std::string cors_methods = "GET, POST, DELETE, OPTIONS";
    std::string cors_headers = "*";
    bool        cors_credentials = true;    // --cors-credentials / --no-cors-credentials
    bool        ui = true;                  // --ui / --no-ui (--webui / --no-webui)
    std::string public_path;                // --path: serve static files from here instead of the built-in page
    std::string ui_config;                  // --ui-config JSON (passed to GET /props as ui_settings)
    bool        endpoint_slots   = true;    // --slots / --no-slots
    bool        endpoint_props   = false;   // --props
    bool        endpoint_metrics = false;   // --metrics
    std::string slot_save_path;             // --slot-save-path (accepted; only action=erase is possible)
    int         n_parallel = 1;             // -np: accepted, the NPU serves one request at a time

    // logging
    int         verbosity = 3;              // -lv; -v = 5
    std::string log_file;                   // --log-file
    bool        log_prefix     = false;
    bool        log_timestamps = false;
    bool        log_colors     = false;
    bool        log_disable    = false;
    std::string prompts_log_dir;            // --log-prompts-dir

    // engine threading: helpers (tokenize, templates) may run while a request generates (see README)
    bool helpers_concurrent = true;         // --serialize-helpers turns it off
};

enum class args_result {
    ok,
    exit_ok,     // --help / --version printed
    exit_error,  // bad usage, message printed
};

args_result server_params_parse(int argc, char ** argv, server_params & params);
void        server_params_print_usage(const char * argv0);

// the server's own flags plus the compatible llama-server ones, as a JSON object (for GET /props debugging)
std::string server_params_describe(const server_params & params);

}  // namespace ixsrv
