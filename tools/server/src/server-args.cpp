#include "server-args.h"

#include "server-log.h"
#include "server-utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <functional>
#include <map>
#include <stdexcept>

namespace ixsrv {

namespace {

struct arg_def {
    std::vector<std::string> names;  // "-m", "--model"
    std::string value_hint;          // "FNAME", "" for flags
    std::string env;                 // "LLAMA_ARG_MODEL"
    std::string help;
    // n_args: 0 for a flag, 1 for a value
    std::function<void(server_params &, const std::string &)> apply;
    int n_args = 1;
    bool ixian_only = false;
};

bool parse_bool(const std::string & v) {
    const std::string s = string_lower(string_strip(v));
    if (s == "1" || s == "true" || s == "on" || s == "enabled" || s == "yes") return true;
    if (s == "0" || s == "false" || s == "off" || s == "disabled" || s == "no") return false;
    throw std::invalid_argument("expected a boolean, got '" + v + "'");
}

int64_t parse_int(const std::string & v) {
    size_t pos = 0;
    long long r = std::stoll(v, &pos);
    if (pos != v.size()) throw std::invalid_argument("expected an integer, got '" + v + "'");
    return r;
}

double parse_float(const std::string & v) {
    size_t pos = 0;
    double r = std::stod(v, &pos);
    if (pos != v.size()) throw std::invalid_argument("expected a number, got '" + v + "'");
    return r;
}

std::set<std::string> parse_set(const std::string & v) {
    std::set<std::string> out;
    for (const auto & p : string_split(v, ',')) {
        const std::string s = string_strip(p);
        if (!s.empty()) out.insert(s);
    }
    return out;
}

std::vector<arg_def> make_defs() {
    std::vector<arg_def> defs;
    auto add = [&](std::vector<std::string> names, std::string hint, std::string env, std::string help,
                   std::function<void(server_params &, const std::string &)> apply, bool ixian_only = false) {
        arg_def d;
        d.names = std::move(names);
        d.value_hint = std::move(hint);
        d.env = std::move(env);
        d.help = std::move(help);
        d.apply = std::move(apply);
        d.n_args = d.value_hint.empty() ? 0 : 1;
        d.ixian_only = ixian_only;
        defs.push_back(std::move(d));
    };

    // model
    add({"-m", "--model"}, "DIR", "LLAMA_ARG_MODEL",
        "model package to load: a model root (one directory per target, e.g. models/lfm3-omni-2b) or a target directory",
        [](server_params & p, const std::string & v) { p.model_path = v; });
    add({"--model-type"}, "TYPE", "IXIAN_ARG_MODEL_TYPE",
        "what the package is: auto (detect), llm (LFM3, chat / speech), decision (d1, /v1/systemone) (default: auto)",
        [](server_params & p, const std::string & v) {
            if (v != "auto" && v != "llm" && v != "decision") throw std::invalid_argument("--model-type must be auto, llm or decision");
            p.model_type = v;
        }, true);
    add({"--target"}, "TARGET", "IXIAN_ARG_TARGET",
        "package target: v73 (both NSPs, default) or v73-1nsp (one NSP, LFM3 only)",
        [](server_params & p, const std::string & v) { p.target = v; }, true);
    add({"--native-lib-dir"}, "DIR", "IXIAN_ARG_NATIVE_LIB_DIR",
        "directory of libhexrt_v73_skel.so (default: next to libzetic_ixian_hexagon.so)",
        [](server_params & p, const std::string & v) { p.native_lib_dir = v; }, true);
    add({"--verify"}, "", "IXIAN_ARG_VERIFY",
        "check the sha256 of every package file at load (a few seconds per GB)",
        [](server_params & p, const std::string &) { p.verify_checksums = true; }, true);
    add({"--warmup"}, "", "", "whether to perform warmup (first-run DSP setup) at load (default: enabled)",
        [](server_params & p, const std::string &) { p.warmup = true; });
    add({"--no-warmup"}, "", "", "skip the warmup at load",
        [](server_params & p, const std::string &) { p.warmup = false; });
    add({"--host-affinity"}, "MASK", "IXIAN_ARG_HOST_AFFINITY",
        "CPU affinity of the SDK's host threads: off, or a hex mask (default: the package's setting)",
        [](server_params & p, const std::string & v) { p.host_affinity = v; }, true);
    add({"-a", "--alias"}, "STRING", "LLAMA_ARG_ALIAS",
        "model name aliases, comma separated (the first one in alphabetical order is the id of GET /models)",
        [](server_params & p, const std::string & v) { p.model_alias = parse_set(v); });
    add({"--tags"}, "STRING", "LLAMA_ARG_TAGS", "model tags, comma separated (informational)",
        [](server_params & p, const std::string & v) { p.model_tags = parse_set(v); });

    // generation defaults
    add({"-c", "--ctx-size"}, "N", "LLAMA_ARG_CTX_SIZE",
        "context size in positions (default: 0 = the package default, 2048; up to 131072 on the IQ9 with the v73 package)",
        [](server_params & p, const std::string & v) { p.n_ctx = (int32_t) parse_int(v); });
    add({"-n", "--predict", "--n-predict"}, "N", "LLAMA_ARG_N_PREDICT",
        "default number of tokens to predict (default: -1 = as many as fit the context)",
        [](server_params & p, const std::string & v) { p.n_predict = (int32_t) parse_int(v); });
    add({"--temp", "--temperature"}, "N", "LLAMA_ARG_TEMPERATURE",
        "temperature (default: the package's generation_config; 0 = greedy on the NPU)",
        [](server_params & p, const std::string & v) { p.temperature = (float) parse_float(v); });
    add({"--top-k"}, "N", "LLAMA_ARG_TOP_K", "top-k sampling (default: the package's)",
        [](server_params & p, const std::string & v) { p.top_k = (int32_t) parse_int(v); });
    add({"--top-p"}, "N", "LLAMA_ARG_TOP_P", "top-p sampling (default: the package's)",
        [](server_params & p, const std::string & v) { p.top_p = (float) parse_float(v); });
    add({"--min-p"}, "N", "LLAMA_ARG_MIN_P", "min-p sampling (default: the package's)",
        [](server_params & p, const std::string & v) { p.min_p = (float) parse_float(v); });
    add({"--repeat-penalty"}, "N", "LLAMA_ARG_REPEAT_PENALTY", "repetition penalty (default: the package's)",
        [](server_params & p, const std::string & v) { p.repeat_penalty = (float) parse_float(v); });
    add({"--repeat-last-n"}, "N", "", "last n tokens to consider for the penalty (default: 64)",
        [](server_params & p, const std::string & v) { p.repeat_last_n = (int32_t) parse_int(v); });
    add({"-s", "--seed"}, "SEED", "", "RNG seed (default: -1, random)",
        [](server_params & p, const std::string & v) { p.seed = parse_int(v); });
    add({"--cache-prompt"}, "", "LLAMA_ARG_CACHE_PROMPT", "reuse the KV cache of the previous request's prefix (default: enabled)",
        [](server_params & p, const std::string &) { p.cache_prompt = true; });
    add({"--no-cache-prompt"}, "", "", "always prefill from position 0",
        [](server_params & p, const std::string &) { p.cache_prompt = false; });

    // chat
    add({"-rea", "--reasoning"}, "on|off|auto", "LLAMA_ARG_REASONING",
        "thinking (LFM3 <think>): on, off, or auto (default: auto = off; a request sets it with chat_template_kwargs.enable_thinking)",
        [](server_params & p, const std::string & v) {
            if (v != "on" && v != "off" && v != "auto") throw std::invalid_argument("--reasoning must be on, off or auto");
            p.reasoning = v;
        });
    add({"--reasoning-format"}, "FORMAT", "LLAMA_ARG_THINK",
        "where the thinking goes: none (in message.content), deepseek (message.reasoning_content), deepseek-legacy (both) (default: auto = deepseek)",
        [](server_params & p, const std::string & v) {
            if (v != "none" && v != "deepseek" && v != "deepseek-legacy" && v != "auto") throw std::invalid_argument("--reasoning-format must be none, deepseek, deepseek-legacy or auto");
            p.reasoning_format = v;
        });
    add({"--reasoning-budget"}, "N", "LLAMA_ARG_THINK_BUDGET", "accepted for compatibility; the SDK has no thinking budget",
        [](server_params & p, const std::string & v) { p.reasoning_budget = (int32_t) parse_int(v); });
    add({"--chat-template-kwargs"}, "STRING", "LLAMA_ARG_CHAT_TEMPLATE_KWARGS",
        "default template kwargs as a JSON object, e.g. '{\"enable_thinking\": true}'",
        [](server_params & p, const std::string & v) { p.chat_template_kwargs = v; });
    add({"--prefill-assistant"}, "", "LLAMA_ARG_PREFILL_ASSISTANT", "accepted for compatibility (see README: prefill is not possible on the NPU runtime)",
        [](server_params & p, const std::string &) { p.prefill_assistant = true; });
    add({"--no-prefill-assistant"}, "", "", "treat a trailing assistant message as a complete turn (the only behaviour available)",
        [](server_params & p, const std::string &) { p.prefill_assistant = false; });
    add({"--tts-lang"}, "LANG", "", "default language (ISO 639-1) of speech output, e.g. en, ko",
        [](server_params & p, const std::string & v) { p.tts_lang = v; });
    add({"--tts-voice"}, "VOICE", "", "default voice of speech output: female or male (default: female)",
        [](server_params & p, const std::string & v) { p.tts_voice = v; });

    // media
    add({"--media-path"}, "PATH", "", "directory for loading local media files through file:// URLs (default: disabled)",
        [](server_params & p, const std::string & v) {
            p.media_path = v;
            if (!p.media_path.empty() && p.media_path.back() != '/') p.media_path += '/';
        });
    add({"--media-allow-private-urls"}, "", "LLAMA_ARG_MEDIA_ALLOW_PRIVATE_URLS",
        "allow http(s) media URLs that point to local or private addresses (default: disabled)",
        [](server_params & p, const std::string &) { p.media_allow_private_urls = true; });
    add({"--image-fit"}, "MODE", "IXIAN_ARG_IMAGE_FIT",
        "how an image of another size reaches the model's 512 x 512: letterbox (whole image, black bars), stretch, crop (default: letterbox)",
        [](server_params & p, const std::string & v) { p.image_fit = image_fit_from_name(v); }, true);
    add({"--video-ffmpeg-dir"}, "DIR", "LLAMA_ARG_VIDEO_FFMPEG_DIR",
        "directory containing ffmpeg, used to decode video inputs (default: search in PATH)",
        [](server_params & p, const std::string & v) { p.video_ffmpeg_dir = v; });
    add({"--video-fps"}, "N", "LLAMA_ARG_VIDEO_FPS", "video sampling rate in frames per second (default: 2.0, the model's)",
        [](server_params & p, const std::string & v) { p.video_fps = parse_float(v); });

    // HTTP
    add({"--host"}, "HOST", "LLAMA_ARG_HOST",
        "IP addresses to listen on, comma separated, or UNIX socket paths ending in .sock (default: 127.0.0.1)",
        [](server_params & p, const std::string & v) {
            p.hostnames.clear();
            for (const auto & h : string_split(v, ',')) {
                const std::string s = string_strip(h);
                if (!s.empty()) p.hostnames.push_back(s);
            }
            if (p.hostnames.empty()) throw std::invalid_argument("--host needs at least one address");
        });
    add({"--port"}, "PORT", "LLAMA_ARG_PORT", "port to listen on (default: 9931)",
        [](server_params & p, const std::string & v) { p.port = (int) parse_int(v); });
    add({"--reuse-port"}, "", "LLAMA_ARG_REUSE_PORT", "allow multiple sockets to bind to the same port",
        [](server_params & p, const std::string &) { p.reuse_port = true; });
    add({"--api-prefix"}, "PREFIX", "LLAMA_ARG_API_PREFIX", "prefix path the server serves from, without the trailing slash",
        [](server_params & p, const std::string & v) { p.api_prefix = v; });
    add({"--api-key"}, "KEY", "LLAMA_API_KEY", "API key(s) for authentication, comma separated (default: none)",
        [](server_params & p, const std::string & v) {
            for (const auto & k : string_split(v, ',')) {
                const std::string s = string_strip(k);
                if (!s.empty()) p.api_keys.push_back(s);
            }
        });
    add({"--api-key-file"}, "FNAME", "LLAMA_ARG_API_KEY_FILE", "file with API keys, one per line (# comments)",
        [](server_params & p, const std::string & v) {
            std::ifstream f(v);
            if (!f) throw std::invalid_argument("cannot read --api-key-file " + v);
            std::string line;
            while (std::getline(f, line)) {
                line = string_strip(line);
                if (!line.empty() && line[0] != '#') p.api_keys.push_back(line);
            }
        });
    add({"--ssl-key-file"}, "FNAME", "LLAMA_ARG_SSL_KEY_FILE", "PEM-encoded SSL private key (needs an OpenSSL build)",
        [](server_params & p, const std::string & v) { p.ssl_file_key = v; });
    add({"--ssl-cert-file"}, "FNAME", "LLAMA_ARG_SSL_CERT_FILE", "PEM-encoded SSL certificate",
        [](server_params & p, const std::string & v) { p.ssl_file_cert = v; });
    add({"-to", "--timeout"}, "N", "LLAMA_ARG_TIMEOUT", "server read/write timeout in seconds (default: 3600)",
        [](server_params & p, const std::string & v) { p.timeout_read = p.timeout_write = (int) parse_int(v); });
    add({"--threads-http"}, "N", "LLAMA_ARG_THREADS_HTTP", "threads of the HTTP server (default: -1, automatic)",
        [](server_params & p, const std::string & v) { p.n_threads_http = (int) parse_int(v); });
    add({"--sse-ping-interval"}, "N", "LLAMA_ARG_SSE_PING_INTERVAL", "SSE ping interval in seconds (-1 = disabled, default: 30)",
        [](server_params & p, const std::string & v) { p.sse_ping_interval = (int) parse_int(v); });
    add({"--cors-origins"}, "ORIGINS", "LLAMA_ARG_CORS_ORIGINS",
        "comma separated allowed origins for CORS (default: *); 'localhost' allows same-origin requests only",
        [](server_params & p, const std::string & v) { p.cors_origins = v; });
    add({"--cors-methods"}, "METHODS", "LLAMA_ARG_CORS_METHODS", "allowed methods for CORS (default: GET, POST, DELETE, OPTIONS)",
        [](server_params & p, const std::string & v) { p.cors_methods = v; });
    add({"--cors-headers"}, "HEADERS", "LLAMA_ARG_CORS_HEADERS", "allowed headers for CORS (default: *)",
        [](server_params & p, const std::string & v) { p.cors_headers = v; });
    add({"--cors-credentials"}, "", "LLAMA_ARG_CORS_CREDENTIALS", "allow credentials for CORS (default: enabled)",
        [](server_params & p, const std::string &) { p.cors_credentials = true; });
    add({"--no-cors-credentials"}, "", "", "do not allow credentials for CORS",
        [](server_params & p, const std::string &) { p.cors_credentials = false; });
    add({"--ui", "--webui"}, "", "LLAMA_ARG_UI", "serve the built-in web page at / (default: enabled)",
        [](server_params & p, const std::string &) { p.ui = true; });
    add({"--no-ui", "--no-webui"}, "", "", "do not serve a web page at /",
        [](server_params & p, const std::string &) { p.ui = false; });
    add({"--path"}, "PATH", "LLAMA_ARG_STATIC_PATH", "serve static files from this directory instead of the built-in page",
        [](server_params & p, const std::string & v) { p.public_path = v; });
    add({"--ui-config", "--webui-config"}, "JSON", "LLAMA_ARG_UI_CONFIG", "JSON with default UI settings (returned by GET /props)",
        [](server_params & p, const std::string & v) { p.ui_config = v; });
    add({"--ui-config-file", "--webui-config-file"}, "PATH", "LLAMA_ARG_UI_CONFIG_FILE", "file with default UI settings",
        [](server_params & p, const std::string & v) {
            std::ifstream f(v);
            if (!f) throw std::invalid_argument("cannot read " + v);
            p.ui_config.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        });
    add({"--slots"}, "", "LLAMA_ARG_ENDPOINT_SLOTS", "expose the slots monitoring endpoint (default: enabled)",
        [](server_params & p, const std::string &) { p.endpoint_slots = true; });
    add({"--no-slots"}, "", "", "disable the slots monitoring endpoint",
        [](server_params & p, const std::string &) { p.endpoint_slots = false; });
    add({"--props"}, "", "LLAMA_ARG_ENDPOINT_PROPS", "enable POST /props (default: disabled)",
        [](server_params & p, const std::string &) { p.endpoint_props = true; });
    add({"--metrics"}, "", "LLAMA_ARG_ENDPOINT_METRICS", "enable the Prometheus endpoint GET /metrics (default: disabled)",
        [](server_params & p, const std::string &) { p.endpoint_metrics = true; });
    add({"--slot-save-path"}, "PATH", "LLAMA_ARG_SLOT_SAVE_PATH", "enables POST /slots/:id (only action=erase is possible on this runtime)",
        [](server_params & p, const std::string & v) {
            p.slot_save_path = v;
            if (!p.slot_save_path.empty() && p.slot_save_path.back() != '/') p.slot_save_path += '/';
        });
    add({"-np", "--parallel"}, "N", "LLAMA_ARG_N_PARALLEL", "accepted for compatibility: the NPU serves one request at a time, others wait in the queue",
        [](server_params & p, const std::string & v) { p.n_parallel = (int) parse_int(v); });

    // logging
    add({"-v", "--verbose", "--log-verbose"}, "", "", "log everything, including requests and responses",
        [](server_params & p, const std::string &) { p.verbosity = 5; });
    add({"-lv", "--verbosity", "--log-verbosity"}, "N", "LLAMA_ARG_LOG_VERBOSITY", "verbosity: 0 silent, 1 error, 2 warning, 3 info, 4 trace, 5 debug (default: 3)",
        [](server_params & p, const std::string & v) { p.verbosity = (int) parse_int(v); });
    add({"--log-file"}, "FNAME", "LLAMA_ARG_LOG_FILE", "also log to this file",
        [](server_params & p, const std::string & v) { p.log_file = v; });
    add({"--log-disable"}, "", "", "disable logging",
        [](server_params & p, const std::string &) { p.log_disable = true; });
    add({"--log-prefix"}, "", "LLAMA_ARG_LOG_PREFIX", "prefix log lines with the level",
        [](server_params & p, const std::string &) { p.log_prefix = true; });
    add({"--no-log-prefix"}, "", "", "no level prefix",
        [](server_params & p, const std::string &) { p.log_prefix = false; });
    add({"--log-timestamps"}, "", "LLAMA_ARG_LOG_TIMESTAMPS", "timestamps in log lines",
        [](server_params & p, const std::string &) { p.log_timestamps = true; });
    add({"--no-log-timestamps"}, "", "", "no timestamps",
        [](server_params & p, const std::string &) { p.log_timestamps = false; });
    add({"--log-colors"}, "MODE", "LLAMA_ARG_LOG_COLORS", "colored logging: on, off or auto",
        [](server_params & p, const std::string & v) { p.log_colors = (v == "on") || (v == "auto" && isatty(2)); });
    add({"--log-prompts-dir"}, "PATH", "", "log every rendered prompt to a file in this directory (debugging)",
        [](server_params & p, const std::string & v) { p.prompts_log_dir = v; });
    add({"--serialize-helpers"}, "", "IXIAN_ARG_SERIALIZE_HELPERS",
        "run tokenize / template helpers on the inference thread instead of concurrently (slower /tokenize during generation, maximum safety)",
        [](server_params & p, const std::string &) { p.helpers_concurrent = false; }, true);

    return defs;
}

// llama-server flags with no meaning here: accepted and ignored, with the number of values they take
const std::map<std::string, int> & ignored_args() {
    static const std::map<std::string, int> m = {
        {"-t", 1}, {"--threads", 1}, {"-tb", 1}, {"--threads-batch", 1}, {"-C", 1}, {"--cpu-mask", 1}, {"-Cr", 1}, {"--cpu-range", 1},
        {"--cpu-strict", 1}, {"--prio", 1}, {"--poll", 1}, {"-Cb", 1}, {"--cpu-mask-batch", 1}, {"-Crb", 1}, {"--cpu-range-batch", 1},
        {"--cpu-strict-batch", 1}, {"--prio-batch", 1}, {"--poll-batch", 1},
        {"-b", 1}, {"--batch-size", 1}, {"-ub", 1}, {"--ubatch-size", 1}, {"--keep", 1}, {"--swa-full", 0}, {"-fa", 1}, {"--flash-attn", 1},
        {"--perf", 0}, {"--no-perf", 0}, {"-e", 0}, {"--escape", 0}, {"--no-escape", 0},
        {"--rope-scaling", 1}, {"--rope-scale", 1}, {"--rope-freq-base", 1}, {"--rope-freq-scale", 1}, {"--yarn-orig-ctx", 1},
        {"--yarn-ext-factor", 1}, {"--yarn-attn-factor", 1}, {"--yarn-beta-slow", 1}, {"--yarn-beta-fast", 1},
        {"-kvo", 0}, {"--kv-offload", 0}, {"-nkvo", 0}, {"--no-kv-offload", 0}, {"--repack", 0}, {"-nr", 0}, {"--no-repack", 0}, {"--no-host", 0},
        {"-ctk", 1}, {"--cache-type-k", 1}, {"-ctv", 1}, {"--cache-type-v", 1}, {"--rpc", 1}, {"-lm", 1}, {"--load-mode", 1}, {"-lzm", 1}, {"--lazy-mode", 1},
        {"--mmap", 0}, {"--no-mmap", 0}, {"--mlock", 0}, {"--numa", 1}, {"-dev", 1}, {"--device", 1}, {"--list-devices", 0},
        {"-ot", 1}, {"--override-tensor", 1}, {"-cmoe", 0}, {"--cpu-moe", 0}, {"-ncmoe", 1}, {"--n-cpu-moe", 1}, {"--moe-cache-mib", 1},
        {"--moe-cache-cpu-share", 1}, {"-ncffn", 1}, {"--n-cpu-ffn", 1}, {"-ngl", 1}, {"--gpu-layers", 1}, {"--n-gpu-layers", 1},
        {"-sm", 1}, {"--split-mode", 1}, {"-ts", 1}, {"--tensor-split", 1}, {"-mg", 1}, {"--main-gpu", 1}, {"-fit", 1}, {"--fit", 1},
        {"-fitt", 1}, {"--fit-target", 1}, {"-fitc", 1}, {"--fit-ctx", 1}, {"--check-tensors", 0}, {"--override-kv", 1},
        {"--op-offload", 0}, {"--no-op-offload", 0}, {"--lora", 1}, {"--lora-scaled", 1}, {"--control-vector", 1},
        {"--control-vector-scaled", 1}, {"--control-vector-layer-range", 2}, {"--lora-init-without-apply", 0},
        {"--ple", 1}, {"--shard", 1}, {"-mu", 1}, {"--model-url", 1}, {"-dr", 1}, {"--docker-repo", 1}, {"-hf", 1}, {"-hfr", 1}, {"--hf-repo", 1},
        {"-hff", 1}, {"--hf-file", 1}, {"-hft", 1}, {"--hf-token", 1}, {"--offline", 0}, {"-cl", 0}, {"--cache-list", 0},
        {"--log-jsonl", 0}, {"--no-log-jsonl", 0},
        {"--samplers", 1}, {"--sampler-seq", 1}, {"--sampling-seq", 1}, {"--ignore-eos", 0}, {"--top-nsigma", 1}, {"--top-n-sigma", 1},
        {"--xtc-probability", 1}, {"--xtc-threshold", 1}, {"--typical", 1}, {"--typical-p", 1}, {"--presence-penalty", 1},
        {"--frequency-penalty", 1}, {"--dry-multiplier", 1}, {"--dry-base", 1}, {"--dry-allowed-length", 1}, {"--dry-penalty-last-n", 1},
        {"--dry-sequence-breaker", 1}, {"--adaptive-target", 1}, {"--adaptive-decay", 1}, {"--dynatemp-range", 1}, {"--dynatemp-exp", 1},
        {"--mirostat", 1}, {"--mirostat-lr", 1}, {"--mirostat-ent", 1}, {"-l", 1}, {"--logit-bias", 1}, {"--grammar", 1}, {"--grammar-file", 1},
        {"-j", 1}, {"--json-schema", 1}, {"-jf", 1}, {"--json-schema-file", 1}, {"-bs", 0}, {"--backend-sampling", 0}, {"--backend_sampling", 0},
        {"-lcs", 1}, {"--lookup-cache-static", 1}, {"-lcd", 1}, {"--lookup-cache-dynamic", 1}, {"--kv-unified-per-slot", 1},
        {"-ctxcp", 1}, {"--ctx-checkpoints", 1}, {"--swa-checkpoints", 1}, {"-cms", 1}, {"--checkpoint-min-step", 1},
        {"-cram", 1}, {"--cache-ram", 1}, {"-kvu", 0}, {"--kv-unified", 0}, {"-no-kvu", 0}, {"--no-kv-unified", 0},
        {"--cache-idle-slots", 0}, {"--no-cache-idle-slots", 0}, {"--context-shift", 0}, {"--no-context-shift", 0},
        {"-r", 1}, {"--reverse-prompt", 1}, {"-sp", 0}, {"--special", 0}, {"--spm-infill", 0}, {"--pooling", 1},
        {"-cb", 0}, {"--cont-batching", 0}, {"-nocb", 0}, {"--no-cont-batching", 0},
        {"-mm", 1}, {"--mmproj", 1}, {"-mmu", 1}, {"--mmproj-url", 1}, {"--mmproj-auto", 0}, {"--no-mmproj", 0}, {"--no-mmproj-auto", 0},
        {"--mmproj-offload", 0}, {"--no-mmproj-offload", 0}, {"-mmdev", 1}, {"--mmproj-device", 1},
        {"--image-min-tokens", 1}, {"--image-max-tokens", 1}, {"--mtmd-batch-max-tokens", 1}, {"--video-timestamp-interval", 1},
        {"--embd-normalize", 1}, {"--embedding", 0}, {"--embeddings", 0}, {"--rerank", 0}, {"--reranking", 0},
        {"--jinja", 0}, {"--no-jinja", 0}, {"--chat-template", 1}, {"--chat-template-file", 1}, {"--skip-chat-parsing", 0}, {"--no-skip-chat-parsing", 0},
        {"--reasoning-effort", 1}, {"--reasoning-budget-message", 1}, {"--reasoning-preserve", 0}, {"--no-reasoning-preserve", 0},
        {"-sps", 1}, {"--slot-prompt-similarity", 1}, {"--sleep-idle-seconds", 1}, {"--cache-reuse", 1},
        {"--models-dir", 1}, {"--models-preset", 1}, {"--models-max", 1}, {"--models-memory-margin", 1}, {"--models-autoload", 0}, {"--no-models-autoload", 0},
        {"--ui-mcp-proxy", 0}, {"--webui-mcp-proxy", 0}, {"--no-ui-mcp-proxy", 0}, {"--no-webui-mcp-proxy", 0},
        {"--tools", 1}, {"--tools-runtime", 1}, {"--allow-unauthenticated-tools", 0}, {"--mcp-servers-config", 1}, {"--mcp-servers-json", 1},
        {"-ag", 0}, {"--agent", 0}, {"-no-ag", 0}, {"--no-agent", 0}, {"--connect", 0}, {"--connect-code", 1},
        {"--spec-type", 1}, {"--spec-default", 0}, {"-md", 1}, {"--model-draft", 1}, {"--spec-draft-model", 1}, {"-hfd", 1}, {"-hfrd", 1}, {"--hf-repo-draft", 1},
        {"--spec-draft-hf", 1}, {"--spec-draft-n-max", 1}, {"--spec-draft-n-min", 1}, {"--spec-draft-n-ubatch", 1}, {"--spec-draft-ngl", 1},
        {"-ngld", 1}, {"--gpu-layers-draft", 1}, {"--n-gpu-layers-draft", 1}, {"--spec-draft-p-split", 1}, {"--draft-p-split", 1},
        {"--spec-draft-p-min", 1}, {"--draft-p-min", 1}, {"--spec-draft-sampling", 1}, {"--spec-draft-backend-sampling", 0}, {"--no-spec-draft-backend-sampling", 0},
        {"--spec-draft-device", 1}, {"-devd", 1}, {"--device-draft", 1}, {"--spec-synth-len", 1}, {"--spec-synth-rates", 1},
        {"--spec-draft-threads", 1}, {"-td", 1}, {"--threads-draft", 1}, {"--spec-draft-threads-batch", 1}, {"-tbd", 1}, {"--threads-batch-draft", 1},
        {"--spec-draft-cpu-mask", 1}, {"-Cd", 1}, {"--cpu-mask-draft", 1}, {"--spec-draft-cpu-range", 1}, {"-Crd", 1}, {"--cpu-range-draft", 1},
        {"--spec-draft-cpu-strict", 1}, {"--cpu-strict-draft", 1}, {"--spec-draft-prio", 1}, {"--prio-draft", 1}, {"--spec-draft-poll", 1}, {"--poll-draft", 1},
        {"--spec-draft-cpu-mask-batch", 1}, {"-Cbd", 1}, {"--cpu-mask-batch-draft", 1}, {"--spec-draft-cpu-strict-batch", 1}, {"--cpu-strict-batch-draft", 1},
        {"--spec-draft-prio-batch", 1}, {"--prio-batch-draft", 1}, {"--spec-draft-poll-batch", 1}, {"--poll-batch-draft", 1},
        {"--spec-draft-override-tensor", 1}, {"-otd", 1}, {"--override-tensor-draft", 1}, {"--spec-draft-cpu-moe", 0}, {"-cmoed", 0}, {"--cpu-moe-draft", 0},
        {"--spec-draft-n-cpu-moe", 1}, {"--spec-draft-ncmoe", 1}, {"-ncmoed", 1}, {"--n-cpu-moe-draft", 1},
        {"--spec-ngram-mod-n-min", 1}, {"--spec-ngram-mod-n-max", 1}, {"--spec-ngram-mod-n-match", 1},
        {"--spec-ngram-simple-size-n", 1}, {"--spec-ngram-simple-size-m", 1}, {"--spec-ngram-simple-min-hits", 1},
        {"--spec-ngram-map-k-size-n", 1}, {"--spec-ngram-map-k-size-m", 1}, {"--spec-ngram-map-k-min-hits", 1},
        {"--spec-ngram-map-k4v-size-n", 1}, {"--spec-ngram-map-k4v-size-m", 1}, {"--spec-ngram-map-k4v-min-hits", 1},
        {"--draft", 1}, {"--draft-n", 1}, {"--draft-max", 1}, {"--draft-min", 1}, {"--draft-n-min", 1},
        {"--spec-ngram-size-n", 1}, {"--spec-ngram-size-m", 1}, {"--spec-ngram-min-hits", 1},
        {"--spec-draft-type-k", 1}, {"-ctkd", 1}, {"--cache-type-k-draft", 1}, {"--spec-draft-type-v", 1}, {"-ctvd", 1}, {"--cache-type-v-draft", 1},
        {"--grp-attn-n", 1}, {"--grp-attn-w", 1}, {"--completion-bash", 0},
    };
    return m;
}

}  // namespace

void server_params_print_usage(const char * argv0) {
    const auto defs = make_defs();
    printf("usage: %s -m <model dir> [options]\n\n", argv0);
    printf("ixian-server %s: an llama-server compatible HTTP server for the Ixian Hexagon SDK (%s).\n", server_version(), sdk_version());
    printf("Options that llama-server has and this server does not need (GPU layers, batch sizes, draft models, ...) are accepted\n");
    printf("and ignored. Every option has an environment variable (shown in parentheses); the command line wins.\n\n");
    printf("%-40s %s\n", "-h, --help, --usage", "print usage and exit");
    printf("%-40s %s\n", "--version", "show version and build info");
    for (const auto & d : defs) {
        std::string names;
        for (size_t i = 0; i < d.names.size(); i++) {
            if (i) names += ", ";
            names += d.names[i];
        }
        if (!d.value_hint.empty()) {
            names += " " + d.value_hint;
        }
        std::string help = d.help;
        if (!d.env.empty()) {
            help += " (env: " + d.env + ")";
        }
        if (d.ixian_only) {
            help += " [ixian]";
        }
        if (names.size() <= 40) {
            printf("%-40s %s\n", names.c_str(), help.c_str());
        } else {
            printf("%s\n%-40s %s\n", names.c_str(), "", help.c_str());
        }
    }
}

args_result server_params_parse(int argc, char ** argv, server_params & params) {
    const auto defs = make_defs();

    // environment first, the command line overrides
    for (const auto & d : defs) {
        if (d.env.empty()) continue;
        const char * value = getenv(d.env.c_str());
        if (!value) continue;
        try {
            if (d.n_args == 0) {
                if (parse_bool(value)) {
                    d.apply(params, "");
                } else {
                    // "--flag" set to false through the environment: look for the matching --no-flag
                    for (const auto & o : defs) {
                        for (const auto & n : o.names) {
                            if (string_starts_with(d.names.back(), "--") && n == "--no-" + d.names.back().substr(2)) {
                                o.apply(params, "");
                            }
                        }
                    }
                }
            } else {
                d.apply(params, value);
            }
        } catch (const std::exception & e) {
            fprintf(stderr, "error: environment variable %s: %s\n", d.env.c_str(), e.what());
            return args_result::exit_error;
        }
    }

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "--usage") {
            server_params_print_usage(argv[0]);
            return args_result::exit_ok;
        }
        if (arg == "--version") {
            printf("ixian-server %s (%s), Ixian Hexagon SDK %s, engine %s\n", server_version(), build_info(), sdk_version(),
#ifdef IXSRV_ENGINE_NAME
                   IXSRV_ENGINE_NAME
#else
                   "hexagon"
#endif
            );
            return args_result::exit_ok;
        }

        // "--key=value"
        std::string key = arg;
        std::string inline_value;
        bool has_inline = false;
        if (string_starts_with(arg, "--")) {
            const size_t eq = arg.find('=');
            if (eq != std::string::npos) {
                key = arg.substr(0, eq);
                inline_value = arg.substr(eq + 1);
                has_inline = true;
            }
        }

        const arg_def * def = nullptr;
        for (const auto & d : defs) {
            for (const auto & n : d.names) {
                if (n == key) {
                    def = &d;
                    break;
                }
            }
            if (def) break;
        }

        if (!def) {
            const auto & ign = ignored_args();
            auto it = ign.find(key);
            if (it != ign.end()) {
                std::string shown = key;
                for (int k = 0; k < it->second && !has_inline; k++) {
                    if (i + 1 < argc) {
                        shown += " " + std::string(argv[++i]);
                    }
                }
                fprintf(stderr, "note: '%s' is a llama-server option without a meaning on the Ixian Hexagon runtime, ignored\n", shown.c_str());
                continue;
            }
            fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
            return args_result::exit_error;
        }

        std::string value;
        if (def->n_args == 1) {
            if (has_inline) {
                value = inline_value;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                fprintf(stderr, "error: %s needs a value\n", key.c_str());
                return args_result::exit_error;
            }
        }
        try {
            def->apply(params, value);
        } catch (const std::exception & e) {
            fprintf(stderr, "error: %s: %s\n", key.c_str(), e.what());
            return args_result::exit_error;
        }
    }

    if (params.model_path.empty()) {
        fprintf(stderr, "error: a model package is required (-m <dir>); the model packages are on Hugging Face, see the README\n");
        return args_result::exit_error;
    }
    if (params.n_parallel < 1) {
        params.n_parallel = 1;
    }
    if (!params.ssl_file_key.empty() != !params.ssl_file_cert.empty()) {
        fprintf(stderr, "error: --ssl-key-file and --ssl-cert-file go together\n");
        return args_result::exit_error;
    }
    if (params.tts_voice != "female" && params.tts_voice != "male" && !params.tts_voice.empty()) {
        fprintf(stderr, "error: --tts-voice must be female or male\n");
        return args_result::exit_error;
    }
    if (!params.chat_template_kwargs.empty()) {
        try {
            auto j = json::parse(params.chat_template_kwargs);
            if (!j.is_object()) throw std::invalid_argument("not an object");
        } catch (const std::exception & e) {
            fprintf(stderr, "error: --chat-template-kwargs must be a JSON object: %s\n", e.what());
            return args_result::exit_error;
        }
    }
    if (!params.ui_config.empty()) {
        try {
            auto j = json::parse(params.ui_config);
            if (!j.is_object()) throw std::invalid_argument("not an object");
        } catch (const std::exception & e) {
            fprintf(stderr, "error: --ui-config must be a JSON object: %s\n", e.what());
            return args_result::exit_error;
        }
    }
    return args_result::ok;
}

std::string server_params_describe(const server_params & p) {
    json j = {
        {"model", p.model_path}, {"model_type", p.model_type}, {"target", p.target},
        {"n_ctx", p.n_ctx}, {"n_predict", p.n_predict}, {"n_parallel", p.n_parallel},
        {"hostnames", p.hostnames}, {"port", p.port}, {"api_prefix", p.api_prefix},
        {"cors_origins", p.cors_origins}, {"ui", p.ui},
        {"reasoning", p.reasoning}, {"reasoning_format", p.reasoning_format},
        {"image_fit", image_fit_name(p.image_fit)}, {"cache_prompt", p.cache_prompt},
        {"endpoint_slots", p.endpoint_slots}, {"endpoint_props", p.endpoint_props}, {"endpoint_metrics", p.endpoint_metrics},
    };
    return safe_json_to_str(j);
}

}  // namespace ixsrv
