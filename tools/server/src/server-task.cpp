#include "server-task.h"

#include "server-log.h"
#include "server-media.h"

#include <cmath>
#include <ctime>
#include <limits>

namespace ixsrv {

const char * stop_type_to_str(stop_type t) {
    switch (t) {
        case STOP_TYPE_EOS:   return "eos";
        case STOP_TYPE_WORD:  return "word";
        case STOP_TYPE_LIMIT: return "limit";
        default:              return "none";
    }
}

//
// task_params
//

json task_params::to_json(bool only_metrics) const {
    json j = {
        {"seed",                      sampling.seed < 0 ? json(0xFFFFFFFFu) : json(sampling.seed)},
        {"temperature",               sampling.temp},
        {"dynatemp_range",            0.0},
        {"dynatemp_exponent",         1.0},
        {"top_k",                     sampling.top_k},
        {"top_p",                     sampling.top_p},
        {"min_p",                     sampling.min_p},
        {"top_n_sigma",               -1.0},
        {"xtc_probability",           0.0},
        {"xtc_threshold",             0.1},
        {"typical_p",                 1.0},
        {"repeat_last_n",             sampling.penalty_last_n},
        {"repeat_penalty",            sampling.penalty_repeat},
        {"presence_penalty",          0.0},
        {"frequency_penalty",         0.0},
        {"dry_multiplier",            0.0},
        {"dry_base",                  1.75},
        {"dry_allowed_length",        2},
        {"dry_penalty_last_n",        64},
        {"mirostat",                  0},
        {"mirostat_tau",              5.0},
        {"mirostat_eta",              0.1},
        {"adaptive_target",           -1.0},
        {"adaptive_decay",            0.9},
        {"max_tokens",                n_predict},
        {"n_predict",                 n_predict},
        {"n_keep",                    n_keep},
        {"n_discard",                 n_discard},
        {"ignore_eos",                sampling.ignore_eos},
        {"stream",                    stream},
        {"n_probs",                   sampling.n_probs},
        {"min_keep",                  0},
        {"chat_format",               "Content-only"},
        {"reasoning_format",          reasoning_format_name(reasoning_fmt)},
        {"reasoning_in_content",      stream && reasoning_fmt == reasoning_format::deepseek_legacy},
        {"generation_prompt",         ""},
        {"samplers",                  json::array({"penalties", "top_k", "top_p", "min_p", "temperature"})},
        {"speculative.types",         json::array()},
        {"timings_per_token",         timings_per_token},
        {"post_sampling_probs",       post_sampling_probs},
        {"backend_sampling",          sampling.temp == 0.0f},
        {"lora",                      json::array()},
        {"enable_thinking",           enable_thinking},
        {"modalities",                want_audio ? (want_text ? json::array({"text", "audio"}) : json::array({"audio"})) : json::array({"text"})},
    };
    if (!only_metrics) {
        j["stop"] = antiprompt;
        j["dry_sequence_breakers"] = json::array({"\n", ":", "\"", "*"});
        j["logit_bias"] = json::array();
        j["grammar"] = "";
        j["grammar_lazy"] = false;
        j["grammar_triggers"] = json::array();
        j["preserved_tokens"] = json::array();
        j["max_audio_seconds"] = max_audio_seconds;
    }
    return j;
}

zx::GenerationConfig task_params::to_sdk(int32_t max_tokens) const {
    zx::GenerationConfig cfg;
    cfg.max_tokens = max_tokens;
    cfg.temperature = sampling.temp;
    cfg.top_p = sampling.top_p;
    cfg.min_p = sampling.min_p;
    cfg.top_k = sampling.top_k;
    cfg.repetition_penalty = sampling.penalty_repeat;
    cfg.repeat_last_n = sampling.penalty_last_n;
    // the SDK takes 0 as "random": an explicit seed N becomes N + 1 so that seed 0 stays deterministic and distinct
    cfg.seed = sampling.seed < 0 ? 0 : (uint64_t) sampling.seed + 1;
    cfg.stop = antiprompt;
    cfg.logprobs = sampling.n_probs > 0;
    cfg.top_logprobs = std::min(20, std::max(0, sampling.n_probs));
    cfg.enable_thinking = enable_thinking;
    cfg.modalities.clear();
    if (want_text) cfg.modalities.push_back(zx::Modality::Text);
    if (want_audio) cfg.modalities.push_back(zx::Modality::Audio);
    if (cfg.modalities.empty()) cfg.modalities.push_back(zx::Modality::Text);
    if (audio_first_chunk_frames > 0) cfg.audio.first_chunk_frames = audio_first_chunk_frames;
    if (audio_chunk_frames > 0) cfg.audio.chunk_frames = audio_chunk_frames;
    cfg.audio.temperature = audio_temperature;
    cfg.audio.top_k = audio_top_k;
    cfg.audio.repetition_penalty = audio_repetition_penalty;
    cfg.audio.collect = true;
    cfg.max_audio_seconds = max_audio_seconds;
    cfg.cache_prompt = cache_prompt;
    return cfg;
}

task_params task_params_from_json(const json & data, const server_params & defaults, int32_t n_ctx) {
    task_params p;

    p.stream            = json_value(data, "stream", false);
    p.cache_prompt      = json_value(data, "cache_prompt", defaults.cache_prompt);
    p.return_tokens     = json_value(data, "return_tokens", false);
    p.return_progress   = json_value(data, "return_progress", false);
    p.timings_per_token = json_value(data, "timings_per_token", false);
    p.post_sampling_probs = json_value(data, "post_sampling_probs", false);
    p.sse_ping_interval = json_value(data, "sse_ping_interval", defaults.sse_ping_interval);
    p.n_predict         = json_value(data, "n_predict", json_value(data, "max_tokens", json_value(data, "max_completion_tokens", defaults.n_predict)));
    if (data.contains("n_predict") && !data.at("n_predict").is_null()) {
        p.n_predict = json_value(data, "n_predict", p.n_predict);
    }
    if (defaults.n_predict > 0 && (p.n_predict < 0 || p.n_predict > defaults.n_predict)) {
        p.n_predict = defaults.n_predict;  // the server's cap, like llama-server
    }
    p.n_keep            = json_value(data, "n_keep", 0);
    p.n_discard         = json_value(data, "n_discard", 0);
    p.n_cmpl            = json_value(data, "n", json_value(data, "n_cmpl", 1));
    p.t_max_predict_ms  = json_value(data, "t_max_predict_ms", (int64_t) -1);
    p.response_fields   = json_value(data, "response_fields", std::vector<std::string>());

    const json so = json_value(data, "stream_options", json::object());
    p.include_usage = json_value(so, "include_usage", true);

    // sampling
    p.sampling.temp           = json_value(data, "temperature", defaults.temperature);
    p.sampling.top_k          = json_value(data, "top_k", defaults.top_k);
    p.sampling.top_p          = json_value(data, "top_p", defaults.top_p);
    p.sampling.min_p          = json_value(data, "min_p", defaults.min_p);
    p.sampling.penalty_repeat = json_value(data, "repeat_penalty", json_value(data, "repetition_penalty", defaults.repeat_penalty));
    p.sampling.penalty_last_n = json_value(data, "repeat_last_n", defaults.repeat_last_n);
    p.sampling.ignore_eos     = json_value(data, "ignore_eos", false);
    p.sampling.n_probs        = json_value(data, "n_probs", 0);
    // OpenAI: chat "logprobs": true + "top_logprobs": N; completions "logprobs": N
    if (data.contains("logprobs") && !data.at("logprobs").is_null()) {
        const json & lp = data.at("logprobs");
        if (lp.is_boolean()) {
            if (lp.get<bool>()) {
                p.sampling.n_probs = json_value(data, "top_logprobs", 20);
            } else if (data.contains("top_logprobs") && !data.at("top_logprobs").is_null()) {
                throw api_error(ERROR_TYPE_INVALID_REQUEST, "top_logprobs requires logprobs to be set to true");
            }
        } else if (lp.is_number_integer()) {
            p.sampling.n_probs = lp.get<int32_t>();
        } else {
            throw api_error(ERROR_TYPE_INVALID_REQUEST, "logprobs must be a boolean (chat) or an integer (completions)");
        }
    } else if (data.contains("top_logprobs") && !data.at("top_logprobs").is_null()) {
        throw api_error(ERROR_TYPE_INVALID_REQUEST, "top_logprobs requires logprobs to be set to true");
    }
    {
        // "seed": -1 or 0xFFFFFFFF = random (llama.cpp LLAMA_DEFAULT_SEED)
        int64_t seed = defaults.seed;
        if (data.contains("seed") && !data.at("seed").is_null()) {
            const json & s = data.at("seed");
            if (s.is_number_unsigned()) seed = (int64_t) s.get<uint64_t>();
            else if (s.is_number_integer()) seed = s.get<int64_t>();
            else if (s.is_number_float()) seed = (int64_t) s.get<double>();
            else throw api_error(ERROR_TYPE_INVALID_REQUEST, "seed must be a number");
        }
        if (seed == 0xFFFFFFFFLL) seed = -1;
        p.sampling.seed = seed;
    }
    if (p.sampling.n_probs < 0 || p.sampling.n_probs > 20) {
        throw api_error(ERROR_TYPE_INVALID_REQUEST, "n_probs must be in 0..20 on this runtime");
    }
    if (p.n_cmpl < 1 || p.n_cmpl > 16) {
        throw api_error(ERROR_TYPE_INVALID_REQUEST, "n must be in 1..16");
    }
    if (p.n_predict == 0) {
        throw api_error(ERROR_TYPE_INVALID_REQUEST, "max_tokens must not be 0");
    }
    if (n_ctx > 0 && p.n_predict > n_ctx) {
        p.n_predict = n_ctx;
    }

    // stop sequences
    if (data.contains("stop") && !data.at("stop").is_null()) {
        const json & stop = data.at("stop");
        if (stop.is_string()) {
            if (!stop.get<std::string>().empty()) p.antiprompt.push_back(stop.get<std::string>());
        } else if (stop.is_array()) {
            for (const auto & s : stop) {
                if (!s.is_string()) throw api_error(ERROR_TYPE_INVALID_REQUEST, "stop must be a string or an array of strings");
                if (!s.get<std::string>().empty()) p.antiprompt.push_back(s.get<std::string>());
            }
        } else {
            throw api_error(ERROR_TYPE_INVALID_REQUEST, "stop must be a string or an array of strings");
        }
    }

    // llama.cpp-only knobs: told once in the log, never an error
    static const char * ignored_keys[] = {
        "presence_penalty", "frequency_penalty", "logit_bias", "mirostat", "mirostat_tau", "mirostat_eta", "dry_multiplier", "dry_base",
        "dry_allowed_length", "dry_penalty_last_n", "dry_sequence_breakers", "xtc_probability", "xtc_threshold", "typical_p", "typ_p",
        "dynatemp_range", "dynatemp_exponent", "top_n_sigma", "samplers", "lora", "n_cache_reuse", "speculative.n_max", "speculative.n_min",
        "speculative.p_min", "id_slot", "reasoning_budget_tokens", "thinking_budget_tokens", "adaptive_target", "adaptive_decay",
    };
    for (const char * k : ignored_keys) {
        if (data.contains(k) && !data.at(k).is_null()) {
            SRV_DBG("request parameter '%s' has no effect on the Ixian Hexagon runtime\n", k);
        }
    }

    // speech
    if (data.contains("modalities") && !data.at("modalities").is_null()) {
        const json & m = data.at("modalities");
        if (!m.is_array()) throw api_error(ERROR_TYPE_INVALID_REQUEST, "modalities must be an array");
        p.want_text = false;
        p.want_audio = false;
        for (const auto & v : m) {
            const std::string s = v.is_string() ? v.get<std::string>() : "";
            if (s == "text") p.want_text = true;
            else if (s == "audio") p.want_audio = true;
            else throw api_error(ERROR_TYPE_INVALID_REQUEST, "modalities must contain only \"text\" and/or \"audio\"");
        }
        if (!p.want_text && !p.want_audio) {
            throw api_error(ERROR_TYPE_INVALID_REQUEST, "modalities must not be empty");
        }
    }
    const json audio = json_value(data, "audio", json::object());
    if (audio.is_object() && !audio.empty()) {
        p.audio_voice = json_value(audio, "voice", std::string());
        p.audio_format = json_value(audio, "format", std::string("wav"));
        if (p.audio_format == "pcm") p.audio_format = "pcm16";
        if (p.audio_format != "wav" && p.audio_format != "pcm16" && p.audio_format != "pcm32") {
            throw api_error(ERROR_TYPE_INVALID_REQUEST, "audio.format must be wav, pcm16 or pcm32 on this runtime");
        }
        if (!p.audio_voice.empty() && !lfm3_is_voice(p.audio_voice)) {
            throw api_error(ERROR_TYPE_INVALID_REQUEST, "unknown voice \"" + p.audio_voice + "\", see \"voices\" in GET /props");
        }
        p.audio_lang = json_value(audio, "language", json_value(audio, "lang", std::string()));
    }
    p.max_audio_seconds        = json_value(data, "max_audio_seconds", 30.0f);
    p.audio_first_chunk_frames = json_value(data, "audio_first_chunk_frames", -1);
    p.audio_chunk_frames       = json_value(data, "audio_chunk_frames", -1);
    p.audio_temperature        = json_value(data, "audio_temperature", -1.0f);
    p.audio_top_k              = json_value(data, "audio_top_k", -1);
    p.audio_repetition_penalty = json_value(data, "audio_repetition_penalty", -1.0f);
    if (p.want_audio && p.sampling.n_probs > 0) {
        throw api_error(ERROR_TYPE_INVALID_REQUEST, "logprobs is not available with audio output");
    }

    p.verbose = defaults.verbosity >= 5 || json_value(data, "verbose", false);
    return p;
}

//
// token_prob
//

json token_prob::to_json(bool post_sampling_probs) const {
    std::string txt(text);
    txt.resize(validate_utf8(txt));
    json j = {
        {"id",    tok},
        {"token", txt},
        {"bytes", str_to_bytes_json(text)},
        {post_sampling_probs ? "prob" : "logprob", post_sampling_probs ? std::exp(logprob) : logprob},
    };
    return j;
}

json token_prob::vector_to_json(const std::vector<token_prob> & probs, bool post_sampling_probs) {
    json out = json::array();
    for (const auto & p : probs) {
        json j = p.to_json(post_sampling_probs);
        json top = json::array();
        for (const auto & t : p.top) {
            top.push_back(t.to_json(post_sampling_probs));
        }
        j[post_sampling_probs ? "top_probs" : "top_logprobs"] = top;
        out.push_back(j);
    }
    return out;
}

//
// task_timings
//

json task_timings::to_json() const {
    const double prompt_per_token_ms = prompt_n > 0 ? prompt_ms / prompt_n : 0.0;
    const double prompt_per_second   = prompt_ms > 0.0 ? 1e3 / prompt_ms * prompt_n : 0.0;
    const double predicted_per_token_ms = predicted_steps > 0 ? predicted_ms / predicted_steps : 0.0;
    double predicted_per_second = predicted_per_second_sdk;
    if (predicted_per_second <= 0.0) {
        predicted_per_second = predicted_ms > 0.0 ? 1e3 / predicted_ms * predicted_steps : 0.0;
    }
    auto finite = [](double v) { return std::isfinite(v) ? v : 0.0; };
    json j = {
        {"cache_n",                cache_n},
        {"prompt_n",               prompt_n},
        {"prompt_ms",              finite(prompt_ms)},
        {"prompt_per_token_ms",    finite(prompt_per_token_ms)},
        {"prompt_per_second",      finite(prompt_per_second)},
        {"predicted_n",            predicted_n},
        {"predicted_ms",           finite(predicted_ms)},
        {"predicted_per_token_ms", finite(predicted_per_token_ms)},
        {"predicted_per_second",   finite(predicted_per_second)},
        {"ttft_ms",                finite(ttft_ms)},
    };
    if (encode_ms > 0) j["encode_ms"] = finite(encode_ms);
    if (audio_frames > 0) {
        j["audio_frames"] = audio_frames;
        j["audio_seconds"] = finite(audio_seconds);
        j["realtime_factor"] = finite(realtime_factor);
    }
    return j;
}

//
// completion_result
//

completion_result::completion_result()
    : oai_resp_id(gen_resp_id("resp_")), oai_resp_reasoning_id(gen_resp_id("rs_")), oai_resp_message_id(gen_resp_id("msg_")) {
    audio_id = gen_resp_id("audio_");
}

json completion_result::usage_json_oaicompat() const {
    return json {
        {"completion_tokens", n_decoded},
        {"prompt_tokens",     n_prompt_tokens},
        {"total_tokens",      n_decoded + n_prompt_tokens},
        {"prompt_tokens_details", json {{"cached_tokens", n_prompt_tokens_cache}}},
    };
}

std::string completion_result::finish_reason_oaicompat() const {
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        return tool_calls.empty() ? "stop" : "tool_calls";
    }
    return "length";
}

static std::string content_with_reasoning(const completion_result & r) {
    // reasoning_format none / deepseek-legacy: the thinking stays inline in the content
    if (r.params.reasoning_fmt == reasoning_format::deepseek) {
        return r.content;
    }
    if (r.reasoning.empty()) {
        return r.content;
    }
    return "<think>" + r.reasoning + "</think>" + r.content;
}

json completion_result::message_json_oaicompat(bool) const {
    json msg = {{"role", "assistant"}};
    const std::string c = content_with_reasoning(*this);
    msg["content"] = c.empty() && !tool_calls.empty() ? json(nullptr) : json(c);
    if (!reasoning.empty() && params.reasoning_fmt != reasoning_format::none) {
        msg["reasoning_content"] = reasoning;
    }
    if (!tool_calls.empty()) {
        json calls = json::array();
        for (const auto & tc : tool_calls) {
            calls.push_back(json {
                {"type", "function"},
                {"function", {{"name", tc.name}, {"arguments", tc.arguments}}},
                {"id", tc.id},
            });
        }
        msg["tool_calls"] = calls;
    }
    if (params.want_audio && !audio.empty()) {
        std::string bytes;
        if (params.audio_format == "pcm16") {
            bytes = pcm_f32_to_s16(audio.data(), audio.size());
        } else if (params.audio_format == "pcm32") {
            bytes = pcm_f32_bytes(audio.data(), audio.size());
        } else {
            bytes = encode_wav_s16(audio.data(), audio.size(), (uint32_t) audio_sample_rate);
        }
        msg["audio"] = json {
            {"id",         audio_id},
            {"data",       base64_encode(bytes)},
            {"expires_at", (int64_t) std::time(nullptr) + 3600},
            {"transcript", content},
        };
    }
    return msg;
}

json completion_result::to_json_non_oaicompat() const {
    // in a stream the pieces were already sent: the final chunk repeats neither the text nor the tokens (llama-server)
    json res = json {
        {"index",               index},
        {"content",             params.stream ? std::string() : content_with_reasoning(*this)},
        {"tokens",              params.return_tokens && !params.stream ? json(tokens) : json::array()},
        {"id_slot",             0},
        {"stop",                true},
        {"model",               params.oaicompat_model},
        {"tokens_predicted",    n_decoded},
        {"tokens_evaluated",    n_prompt_tokens},
        {"generation_settings", params.to_json()},
        {"prompt",              prompt},
        {"has_new_line",        has_new_line},
        {"truncated",           truncated},
        {"stop_type",           stop_type_to_str(stop)},
        {"stopping_word",       stopping_word},
        {"tokens_cached",       n_prompt_tokens_cache},
        {"timings",             timings.to_json()},
    };
    if (!reasoning.empty()) {
        res["reasoning_content"] = reasoning;
    }
    if (!tool_calls.empty()) {
        res["tool_calls"] = message_json_oaicompat().at("tool_calls");
    }
    if (!params.stream && !probs.empty()) {
        res["completion_probabilities"] = token_prob::vector_to_json(probs, params.post_sampling_probs);
    }
    if (params.want_audio && !audio.empty()) {
        res["audio"] = message_json_oaicompat().at("audio");
    }
    return params.response_fields.empty() ? res : json_get_nested_values(params.response_fields, res);
}

json completion_result::to_json_oaicompat_cmpl() const {
    json logprobs = json(nullptr);
    if (!params.stream && !probs.empty()) {
        logprobs = json {{"content", token_prob::vector_to_json(probs, params.post_sampling_probs)}};
    }
    json finish_reason = "length";
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        finish_reason = "stop";
    }
    json res = json {
        {"choices", json::array({json {
            {"text",          params.stream ? std::string() : content_with_reasoning(*this)},
            {"index",         index},
            {"logprobs",      logprobs},
            {"finish_reason", finish_reason},
        }})},
        {"created",            (int64_t) std::time(nullptr)},
        {"model",              params.oaicompat_model},
        {"system_fingerprint", std::string(build_info())},
        {"object",             "text_completion"},
        {"usage",              usage_json_oaicompat()},
        {"id",                 params.oaicompat_cmpl_id},
    };
    if (params.verbose) {
        res["__verbose"] = to_json_non_oaicompat();
    }
    if (timings.is_set) {
        res["timings"] = timings.to_json();
    }
    return res;
}

json completion_result::to_json_oaicompat_chat() const {
    json choice {
        {"finish_reason", finish_reason_oaicompat()},
        {"index",         index},
        {"message",       message_json_oaicompat()},
    };
    if (!params.stream && !probs.empty()) {
        choice["logprobs"] = json {{"content", token_prob::vector_to_json(probs, params.post_sampling_probs)}};
    }
    json res = json {
        {"choices",            json::array({choice})},
        {"created",            (int64_t) std::time(nullptr)},
        {"model",              params.oaicompat_model},
        {"system_fingerprint", std::string(build_info())},
        {"object",             "chat.completion"},
        {"usage",              usage_json_oaicompat()},
        {"id",                 params.oaicompat_cmpl_id},
    };
    if (params.verbose) {
        res["__verbose"] = to_json_non_oaicompat();
    }
    if (timings.is_set) {
        res["timings"] = timings.to_json();
    }
    return res;
}

static json chat_chunk(const completion_result & r, const json & delta, const json & finish_reason, int64_t t) {
    return json {
        {"choices", json::array({json {
            {"finish_reason", finish_reason},
            {"index",         r.index},
            {"delta",         delta},
        }})},
        {"created",            t},
        {"id",                 r.params.oaicompat_cmpl_id},
        {"model",              r.params.oaicompat_model},
        {"system_fingerprint", std::string(build_info())},
        {"object",             "chat.completion.chunk"},
    };
}

json completion_result::to_json_oaicompat_chat_stream() const {
    const int64_t t = (int64_t) std::time(nullptr);
    json deltas = json::array();
    if (legacy_think_open && params.reasoning_fmt != reasoning_format::deepseek) {
        // the reply ended inside the thinking: close the tag in the content
        deltas.push_back(chat_chunk(*this, json {{"content", "</think>"}}, nullptr, t));
    }
    deltas.push_back(chat_chunk(*this, json::object(), finish_reason_oaicompat(), t));
    if (params.include_usage) {
        deltas.push_back(json {
            {"choices",            json::array()},
            {"created",            t},
            {"id",                 params.oaicompat_cmpl_id},
            {"model",              params.oaicompat_model},
            {"system_fingerprint", std::string(build_info())},
            {"object",             "chat.completion.chunk"},
            {"usage",              usage_json_oaicompat()},
        });
    }
    if (timings.is_set) {
        deltas.back()["timings"] = timings.to_json();
    }
    if (params.verbose && !deltas.empty()) {
        deltas.front()["__verbose"] = to_json_non_oaicompat();
    }
    return deltas;
}

static json resp_usage(const completion_result & r) {
    return json {
        {"input_tokens",  r.n_prompt_tokens},
        {"output_tokens", r.n_decoded},
        {"total_tokens",  r.n_decoded + r.n_prompt_tokens},
        {"input_tokens_details", json {{"cached_tokens", r.n_prompt_tokens_cache}}},
    };
}

json completion_result::to_json_oaicompat_resp() const {
    std::vector<json> output;
    if (!reasoning.empty()) {
        output.push_back(json {
            {"id",      oai_resp_reasoning_id},
            {"summary", json::array()},
            {"type",    "reasoning"},
            {"content", json::array({json {{"text", reasoning}, {"type", "reasoning_text"}}})},
            {"encrypted_content", ""},
            {"status",  "completed"},
        });
    }
    if (!content.empty()) {
        output.push_back(json {
            {"content", json::array({json {{"type", "output_text"}, {"annotations", json::array()}, {"logprobs", json::array()}, {"text", content}}})},
            {"id",     oai_resp_message_id},
            {"role",   "assistant"},
            {"status", "completed"},
            {"type",   "message"},
        });
    }
    for (const auto & tc : tool_calls) {
        output.push_back(json {
            {"id",        "fc_" + tc.id},
            {"type",      "function_call"},
            {"status",    "completed"},
            {"arguments", tc.arguments},
            {"call_id",   "call_" + tc.id},
            {"name",      tc.name},
        });
    }
    const int64_t t = (int64_t) std::time(nullptr);
    json res = {
        {"completed_at", t},
        {"created_at",   t},
        {"id",           oai_resp_id},
        {"model",        params.oaicompat_model},
        {"object",       "response"},
        {"output",       output},
        {"status",       "completed"},
        {"usage",        resp_usage(*this)},
    };
    if (timings.is_set) {
        res["timings"] = timings.to_json();
    }
    return res;
}

json completion_result::to_json_oaicompat_resp_stream() const {
    std::vector<json> events;
    std::vector<json> output;
    size_t output_index = 0;

    if (!reasoning.empty()) {
        const size_t idx = output_index++;
        const json item = json {
            {"id",      oai_resp_reasoning_id},
            {"summary", json::array()},
            {"type",    "reasoning"},
            {"content", json::array({json {{"text", reasoning}, {"type", "reasoning_text"}}})},
            {"encrypted_content", ""},
        };
        events.push_back(json {{"event", "response.output_item.done"}, {"data", json {{"type", "response.output_item.done"}, {"output_index", idx}, {"item", item}}}});
        output.push_back(item);
    }
    if (!content.empty()) {
        const size_t idx = output_index++;
        events.push_back(json {{"event", "response.output_text.done"}, {"data", json {
            {"type", "response.output_text.done"}, {"item_id", oai_resp_message_id}, {"output_index", idx}, {"content_index", 0}, {"text", content}}}});
        const json part = {{"type", "output_text"}, {"annotations", json::array()}, {"logprobs", json::array()}, {"text", content}};
        events.push_back(json {{"event", "response.content_part.done"}, {"data", json {
            {"type", "response.content_part.done"}, {"item_id", oai_resp_message_id}, {"output_index", idx}, {"content_index", 0}, {"part", part}}}});
        const json item = {{"type", "message"}, {"status", "completed"}, {"id", oai_resp_message_id}, {"content", json::array({part})}, {"role", "assistant"}};
        events.push_back(json {{"event", "response.output_item.done"}, {"data", json {{"type", "response.output_item.done"}, {"output_index", idx}, {"item", item}}}});
        output.push_back(item);
    }
    for (const auto & tc : tool_calls) {
        const size_t idx = output_index++;
        const json item = {{"id", "fc_" + tc.id}, {"type", "function_call"}, {"status", "completed"}, {"arguments", tc.arguments}, {"call_id", "call_" + tc.id}, {"name", tc.name}};
        events.push_back(json {{"event", "response.output_item.done"}, {"data", json {{"type", "response.output_item.done"}, {"output_index", idx}, {"item", item}}}});
        output.push_back(item);
    }
    const int64_t t = (int64_t) std::time(nullptr);
    events.push_back(json {{"event", "response.completed"}, {"data", json {
        {"type", "response.completed"},
        {"response", json {
            {"id", oai_resp_id}, {"object", "response"}, {"created_at", t}, {"status", "completed"},
            {"model", params.oaicompat_model}, {"output", output}, {"usage", resp_usage(*this)},
        }},
    }}});
    if (timings.is_set) {
        events.back().at("data")["timings"] = timings.to_json();
    }
    return events;
}

json completion_result::to_json_oaicompat_asr() const {
    if (params.transcription_format == "verbose_json") {
        return json {
            {"task",     "transcribe"},
            {"language", params.audio_lang.empty() ? "unknown" : params.audio_lang},
            {"duration", timings.audio_seconds},
            {"text",     content},
            {"usage", json {{"type", "tokens"}, {"input_tokens", n_prompt_tokens}, {"output_tokens", n_decoded}, {"total_tokens", n_decoded + n_prompt_tokens}}},
        };
    }
    return json {
        {"type",  "transcript.text.done"},
        {"text",  content},
        {"usage", json {
            {"type",          "tokens"},
            {"input_tokens",  n_prompt_tokens},
            {"output_tokens", n_decoded},
            {"total_tokens",  n_decoded + n_prompt_tokens},
            {"input_tokens_details", json {{"cached_tokens", n_prompt_tokens_cache}}},
        }},
    };
}

json completion_result::to_json_anthropic() const {
    std::string stop_reason = "max_tokens";
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        stop_reason = tool_calls.empty() ? "end_turn" : "tool_use";
    }
    json blocks = json::array();
    if (!reasoning.empty()) {
        blocks.push_back({{"type", "thinking"}, {"thinking", reasoning}, {"signature", ""}});
    }
    if (!content.empty()) {
        blocks.push_back({{"type", "text"}, {"text", content}});
    }
    for (const auto & tc : tool_calls) {
        json block = {{"type", "tool_use"}, {"id", tc.id}, {"name", tc.name}};
        try {
            block["input"] = json::parse(tc.arguments);
        } catch (...) {
            block["input"] = json::object();
        }
        blocks.push_back(block);
    }
    return json {
        {"id",            params.oaicompat_cmpl_id},
        {"type",          "message"},
        {"role",          "assistant"},
        {"content",       blocks},
        {"model",         params.oaicompat_model},
        {"stop_reason",   stop_reason},
        {"stop_sequence", stopping_word.empty() ? json(nullptr) : json(stopping_word)},
        {"usage", {
            {"cache_read_input_tokens", n_prompt_tokens_cache},
            {"input_tokens",  n_prompt_tokens - n_prompt_tokens_cache},
            {"output_tokens", n_decoded},
        }},
    };
}

json completion_result::to_json_anthropic_stream() const {
    json events = json::array();
    std::string stop_reason = "max_tokens";
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        stop_reason = tool_calls.empty() ? "end_turn" : "tool_use";
    }
    const bool has_thinking = !reasoning.empty();
    const bool has_text = !content.empty();
    const size_t thinking_block_index = 0;
    const size_t text_block_index = has_thinking ? 1 : 0;

    if (has_thinking) {
        events.push_back({{"event", "content_block_delta"}, {"data", {
            {"type", "content_block_delta"}, {"index", thinking_block_index}, {"delta", {{"type", "signature_delta"}, {"signature", ""}}}}}});
        events.push_back({{"event", "content_block_stop"}, {"data", {{"type", "content_block_stop"}, {"index", thinking_block_index}}}});
    }
    if (has_text) {
        events.push_back({{"event", "content_block_stop"}, {"data", {{"type", "content_block_stop"}, {"index", text_block_index}}}});
    }
    for (size_t i = 0; i < tool_calls.size(); i++) {
        const size_t idx = (has_thinking ? 1 : 0) + (has_text ? 1 : 0) + i;
        events.push_back({{"event", "content_block_stop"}, {"data", {{"type", "content_block_stop"}, {"index", idx}}}});
    }
    events.push_back({{"event", "message_delta"}, {"data", {
        {"type", "message_delta"},
        {"delta", {{"stop_reason", stop_reason}, {"stop_sequence", stopping_word.empty() ? json(nullptr) : json(stopping_word)}}},
        {"usage", {{"output_tokens", n_decoded}}},
    }}});
    events.push_back({{"event", "message_stop"}, {"data", {{"type", "message_stop"}}}});
    return events;
}

json completion_result::to_json_final() const {
    switch (params.res_type) {
        case TASK_RESPONSE_TYPE_NONE:      return to_json_non_oaicompat();
        case TASK_RESPONSE_TYPE_OAI_CMPL:  return to_json_oaicompat_cmpl();
        case TASK_RESPONSE_TYPE_OAI_CHAT:  return params.stream ? to_json_oaicompat_chat_stream() : to_json_oaicompat_chat();
        case TASK_RESPONSE_TYPE_OAI_RESP:  return params.stream ? to_json_oaicompat_resp_stream() : to_json_oaicompat_resp();
        case TASK_RESPONSE_TYPE_OAI_ASR:   return to_json_oaicompat_asr();
        case TASK_RESPONSE_TYPE_ANTHROPIC: return params.stream ? to_json_anthropic_stream() : to_json_anthropic();
    }
    return json::object();
}

//
// partial results
//

static json tool_call_delta_json(size_t index, const tool_call_out & tc) {
    return json {
        {"index", index},
        {"id",    tc.id},
        {"type",  "function"},
        {"function", {{"name", tc.name}, {"arguments", tc.arguments}}},
    };
}

static json partial_non_oaicompat(completion_result & r, const completion_delta & d) {
    json res = json {
        {"index",            r.index},
        {"content",          d.content},
        {"tokens",           r.params.return_tokens && !r.tokens.empty() ? json::array({r.tokens.back()}) : json::array()},
        {"stop",             false},
        {"id_slot",          0},
        {"tokens_predicted", r.n_decoded},
        {"tokens_evaluated", r.n_prompt_tokens},
    };
    if (!d.reasoning.empty()) {
        res["reasoning_content"] = d.reasoning;
    }
    if (!d.tool_calls.empty()) {
        json calls = json::array();
        for (const auto & [idx, tc] : d.tool_calls) calls.push_back(tool_call_delta_json(idx, tc));
        res["tool_calls"] = calls;
    }
    if (d.with_timings || r.timings.is_set) {
        if (d.with_timings) res["timings"] = r.timings.to_json();
    }
    if (d.is_progress) {
        res["prompt_progress"] = d.progress;
    }
    if (!d.probs.empty()) {
        res["completion_probabilities"] = token_prob::vector_to_json(d.probs, r.params.post_sampling_probs);
    }
    if (!d.audio_bytes.empty()) {
        res["audio"] = json {{"data", base64_encode(d.audio_bytes)}, {"format", r.params.audio_format}};
    }
    return res;
}

static json partial_oaicompat_cmpl(completion_result & r, const completion_delta & d) {
    json logprobs = json(nullptr);
    if (!d.probs.empty()) {
        logprobs = json {{"content", token_prob::vector_to_json(d.probs, r.params.post_sampling_probs)}};
    }
    json res = json {
        {"choices", json::array({json {
            {"text",          d.content},
            {"index",         r.index},
            {"logprobs",      logprobs},
            {"finish_reason", nullptr},
        }})},
        {"created",            (int64_t) std::time(nullptr)},
        {"model",              r.params.oaicompat_model},
        {"system_fingerprint", std::string(build_info())},
        {"object",             "text_completion"},
        {"id",                 r.params.oaicompat_cmpl_id},
    };
    if (r.params.verbose) {
        res["__verbose"] = partial_non_oaicompat(r, d);
    }
    if (d.with_timings) {
        res["timings"] = r.timings.to_json();
    }
    if (d.is_progress) {
        res["prompt_progress"] = d.progress;
    }
    return res;
}

static json partial_oaicompat_chat(completion_result & r, const completion_delta & d) {
    const int64_t t = (int64_t) std::time(nullptr);
    std::vector<json> deltas;
    auto add_delta = [&](const json & delta) { deltas.push_back(chat_chunk(r, delta, nullptr, t)); };

    if (d.first || d.is_progress) {
        add_delta(json {{"role", "assistant"}, {"content", nullptr}});
    }
    if (!d.reasoning.empty()) {
        if (r.params.reasoning_fmt == reasoning_format::deepseek) {
            add_delta(json {{"reasoning_content", d.reasoning}});
        } else {
            std::string c = d.reasoning;
            if (!r.legacy_think_open) {
                c = "<think>" + c;
                r.legacy_think_open = true;
            }
            json delta = {{"content", c}};
            if (r.params.reasoning_fmt == reasoning_format::deepseek_legacy) {
                delta["reasoning_content"] = d.reasoning;
            }
            add_delta(delta);
        }
    }
    if (!d.content.empty()) {
        std::string c = d.content;
        if (r.legacy_think_open) {
            c = "</think>" + c;
            r.legacy_think_open = false;
        }
        add_delta(json {{"content", c}});
    }
    for (const auto & [idx, tc] : d.tool_calls) {
        add_delta(json {{"tool_calls", json::array({tool_call_delta_json(idx, tc)})}});
    }
    if (!d.audio_bytes.empty()) {
        json audio = {{"id", r.audio_id}, {"data", base64_encode(d.audio_bytes)}};
        if (!d.content.empty()) {
            audio["transcript"] = d.content;
        }
        add_delta(json {{"audio", audio}});
    }
    if (!deltas.empty()) {
        auto & last = deltas.back();
        if (!d.probs.empty()) {
            last.at("choices").at(0)["logprobs"] = json {{"content", token_prob::vector_to_json(d.probs, r.params.post_sampling_probs)}};
        }
        if (d.with_timings) {
            last["timings"] = r.timings.to_json();
        }
        if (d.is_progress) {
            last["prompt_progress"] = d.progress;
        }
    }
    return deltas;
}

static json partial_oaicompat_resp(completion_result & r, const completion_delta & d) {
    std::vector<json> events;
    auto in_progress = [&]() {
        return json {{"event", "response.in_progress"}, {"data", json {{"type", "response.in_progress"}, {"response", json {{"id", r.oai_resp_id}, {"object", "response"}, {"status", "in_progress"}}}}}};
    };
    if (!r.oai_resp_created) {
        events.push_back(json {{"event", "response.created"}, {"data", json {{"type", "response.created"}, {"response", json {{"id", r.oai_resp_id}, {"object", "response"}, {"status", "in_progress"}}}}}});
        events.push_back(in_progress());
        r.oai_resp_created = true;
    } else if (d.is_progress) {
        events.push_back(in_progress());
    }
    const bool has_reasoning = !r.reasoning.empty();
    if (!d.reasoning.empty()) {
        if (!r.thinking_block_started) {
            events.push_back(json {{"event", "response.output_item.added"}, {"data", json {
                {"type", "response.output_item.added"}, {"output_index", 0},
                {"item", json {{"id", r.oai_resp_reasoning_id}, {"summary", json::array()}, {"type", "reasoning"}, {"content", json::array()}, {"encrypted_content", ""}, {"status", "in_progress"}}}}}});
            r.thinking_block_started = true;
        }
        events.push_back(json {{"event", "response.reasoning_text.delta"}, {"data", json {
            {"type", "response.reasoning_text.delta"}, {"output_index", 0}, {"content_index", 0}, {"delta", d.reasoning}, {"item_id", r.oai_resp_reasoning_id}}}});
    }
    if (!d.content.empty()) {
        const size_t idx = has_reasoning ? 1 : 0;
        if (!r.text_block_started) {
            events.push_back(json {{"event", "response.output_item.added"}, {"data", json {
                {"type", "response.output_item.added"}, {"output_index", idx},
                {"item", json {{"content", json::array()}, {"id", r.oai_resp_message_id}, {"role", "assistant"}, {"status", "in_progress"}, {"type", "message"}}}}}});
            events.push_back(json {{"event", "response.content_part.added"}, {"data", json {
                {"type", "response.content_part.added"}, {"item_id", r.oai_resp_message_id}, {"output_index", idx}, {"content_index", 0},
                {"part", json {{"type", "output_text"}, {"text", ""}}}}}});
            r.text_block_started = true;
        }
        events.push_back(json {{"event", "response.output_text.delta"}, {"data", json {
            {"type", "response.output_text.delta"}, {"item_id", r.oai_resp_message_id}, {"output_index", idx}, {"content_index", 0}, {"delta", d.content}}}});
    }
    for (const auto & [tc_index, tc] : d.tool_calls) {
        const size_t idx = (has_reasoning ? 1 : 0) + (r.text_block_started ? 1 : 0) + tc_index;
        events.push_back(json {{"event", "response.output_item.added"}, {"data", json {
            {"type", "response.output_item.added"}, {"output_index", idx},
            {"item", json {{"id", "fc_" + tc.id}, {"arguments", ""}, {"call_id", "call_" + tc.id}, {"name", tc.name}, {"type", "function_call"}, {"status", "in_progress"}}}}}});
        events.push_back(json {{"event", "response.function_call_arguments.delta"}, {"data", json {
            {"type", "response.function_call_arguments.delta"}, {"delta", tc.arguments}, {"item_id", "fc_" + tc.id}, {"output_index", idx}}}});
        r.oai_resp_fc_id = tc.id;
    }
    if (!events.empty()) {
        json & data = events.back().at("data");
        if (d.with_timings) data["timings"] = r.timings.to_json();
        if (d.is_progress) data["prompt_progress"] = d.progress;
    }
    return events;
}

static json partial_oaicompat_asr(completion_result &, const completion_delta & d) {
    return json {{"type", "transcript.text.delta"}, {"delta", d.content}};
}

static json partial_anthropic(completion_result & r, const completion_delta & d) {
    json events = json::array();
    if (d.first) {
        events.push_back({{"event", "message_start"}, {"data", {
            {"type", "message_start"},
            {"message", {
                {"id", r.params.oaicompat_cmpl_id}, {"type", "message"}, {"role", "assistant"}, {"content", json::array()},
                {"model", r.params.oaicompat_model}, {"stop_reason", nullptr}, {"stop_sequence", nullptr},
                {"usage", {{"cache_read_input_tokens", r.n_prompt_tokens_cache}, {"input_tokens", r.n_prompt_tokens - r.n_prompt_tokens_cache}, {"output_tokens", 0}}},
            }},
        }}});
    }
    const size_t thinking_block_index = 0;
    const bool has_reasoning = !r.reasoning.empty();
    const size_t text_block_index = has_reasoning ? 1 : 0;

    if (!d.reasoning.empty()) {
        if (!r.thinking_block_started) {
            events.push_back({{"event", "content_block_start"}, {"data", {{"type", "content_block_start"}, {"index", thinking_block_index}, {"content_block", {{"type", "thinking"}, {"thinking", ""}}}}}});
            r.thinking_block_started = true;
        }
        events.push_back({{"event", "content_block_delta"}, {"data", {{"type", "content_block_delta"}, {"index", thinking_block_index}, {"delta", {{"type", "thinking_delta"}, {"thinking", d.reasoning}}}}}});
    }
    if (!d.content.empty()) {
        if (!r.text_block_started) {
            events.push_back({{"event", "content_block_start"}, {"data", {{"type", "content_block_start"}, {"index", text_block_index}, {"content_block", {{"type", "text"}, {"text", ""}}}}}});
            r.text_block_started = true;
        }
        events.push_back({{"event", "content_block_delta"}, {"data", {{"type", "content_block_delta"}, {"index", text_block_index}, {"delta", {{"type", "text_delta"}, {"text", d.content}}}}}});
    }
    for (const auto & [tc_index, tc] : d.tool_calls) {
        const size_t idx = (has_reasoning ? 1 : 0) + (r.text_block_started ? 1 : 0) + tc_index;
        if (!r.tool_calls_started.count(tc_index)) {
            events.push_back({{"event", "content_block_start"}, {"data", {{"type", "content_block_start"}, {"index", idx}, {"content_block", {{"type", "tool_use"}, {"id", tc.id}, {"name", tc.name}}}}}});
            r.tool_calls_started.insert(tc_index);
        }
        events.push_back({{"event", "content_block_delta"}, {"data", {{"type", "content_block_delta"}, {"index", idx}, {"delta", {{"type", "input_json_delta"}, {"partial_json", tc.arguments}}}}}});
    }
    return events;
}

json to_json_partial(completion_result & r, const completion_delta & d) {
    switch (r.params.res_type) {
        case TASK_RESPONSE_TYPE_NONE:      return partial_non_oaicompat(r, d);
        case TASK_RESPONSE_TYPE_OAI_CMPL:  return partial_oaicompat_cmpl(r, d);
        case TASK_RESPONSE_TYPE_OAI_CHAT:  return partial_oaicompat_chat(r, d);
        case TASK_RESPONSE_TYPE_OAI_RESP:  return partial_oaicompat_resp(r, d);
        case TASK_RESPONSE_TYPE_OAI_ASR:   return partial_oaicompat_asr(r, d);
        case TASK_RESPONSE_TYPE_ANTHROPIC: return partial_anthropic(r, d);
    }
    return json::object();
}

std::string format_sse(task_response_type type, const json & data) {
    if (data.is_null() || (data.is_array() && data.empty())) {
        return "";
    }
    switch (type) {
        case TASK_RESPONSE_TYPE_ANTHROPIC: return format_anthropic_sse(data);
        case TASK_RESPONSE_TYPE_OAI_RESP:  return format_oai_resp_sse(data);
        default:                           return format_oai_sse(data);
    }
}

std::string format_sse_error(task_response_type type, const json & error_obj) {
    if (type == TASK_RESPONSE_TYPE_ANTHROPIC) {
        return format_anthropic_sse(json {{"event", "error"}, {"data", error_obj}});
    }
    return format_oai_sse(json {{"error", error_obj}});
}

}  // namespace ixsrv
