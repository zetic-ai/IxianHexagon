#include "server-routes.h"

#include "server-decision.h"
#include "server-log.h"
#include "server-media.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

extern const unsigned char ixsrv_asset_lfm3_jinja[];
extern const size_t ixsrv_asset_lfm3_jinja_size;

namespace ixsrv {

namespace {

json parse_body(const http_req & req) {
    if (string_strip(req.body).empty()) {
        return json::object();
    }
    try {
        return json::parse(req.body);
    } catch (const std::exception & e) {
        throw api_error(ERROR_TYPE_INVALID_REQUEST, std::string("invalid JSON body: ") + e.what());
    }
}

http_res_ptr make_res() {
    return std::make_unique<http_res>();
}

// multipart form fields arrive as strings: give the numeric and boolean ones their types back
json fix_form_types(json body) {
    for (const char * k : {"top_k", "n_predict", "max_tokens", "seed", "audio_top_k"}) {
        if (body.contains(k) && body[k].is_string()) {
            body[k] = std::stoll(body[k].get<std::string>());
        }
    }
    for (const char * k : {"top_p", "temperature", "repeat_penalty", "speed", "max_audio_seconds", "audio_temperature"}) {
        if (body.contains(k) && body[k].is_string()) {
            body[k] = std::stof(body[k].get<std::string>());
        }
    }
    if (body.contains("stream") && body["stream"].is_string()) {
        body["stream"] = body["stream"] == "true";
    }
    return body;
}

stop_type stop_type_of(zx::FinishReason r, bool has_stop_word) {
    switch (r) {
        case zx::FinishReason::Stop:         return STOP_TYPE_EOS;
        case zx::FinishReason::StopSequence: return has_stop_word ? STOP_TYPE_WORD : STOP_TYPE_EOS;
        case zx::FinishReason::Length:       return STOP_TYPE_LIMIT;
        case zx::FinishReason::ContextFull:  return STOP_TYPE_LIMIT;
        case zx::FinishReason::Cancelled:    return STOP_TYPE_LIMIT;
        case zx::FinishReason::Error:        return STOP_TYPE_NONE;
    }
    return STOP_TYPE_NONE;
}

std::string audio_bytes(const float * pcm, size_t n, const std::string & format) {
    if (format == "pcm32" || format == "pcm") {
        return pcm_f32_bytes(pcm, n);
    }
    return pcm_f32_to_s16(pcm, n);  // wav (streamed body) and pcm16
}

}  // namespace

server_routes::server_routes(engine & eng, const server_params & params) : eng_(eng), params_(params) {}

media_options server_routes::media_opts() const {
    media_options m;
    m.allow_image = eng_.caps().image;
    m.allow_audio = eng_.caps().audio;
    m.allow_video = eng_.caps().video;
    m.media_path = params_.media_path;
    m.allow_private_urls = params_.media_allow_private_urls;
    m.image_fit = params_.image_fit;
    m.video_fps = params_.video_fps;
    m.ffmpeg_dir = params_.video_ffmpeg_dir;
    return m;
}

chat_convert_options server_routes::convert_options() const {
    chat_convert_options o;
    o.media = media_opts();
    o.enable_thinking = params_.reasoning == "on";
    o.reasoning_fmt = reasoning_format_from_name(params_.reasoning_format);
    if (!params_.chat_template_kwargs.empty()) {
        for (const auto & [k, v] : json::parse(params_.chat_template_kwargs).items()) {
            o.template_kwargs[k] = v.dump();
        }
    }
    o.prefill_assistant = params_.prefill_assistant;
    o.eng = &eng_;
    return o;
}

void server_routes::register_routes(http_server & http) {
    auto G = [](auto fn) { return http_server::guard(fn); };
    http.get ("/health",                    G([this](const http_req & r) { return get_health(r); }));
    http.get ("/v1/health",                 G([this](const http_req & r) { return get_health(r); }));
    http.get ("/metrics",                   G([this](const http_req & r) { return get_metrics(r); }));
    http.get ("/props",                     G([this](const http_req & r) { return get_props(r); }));
    http.post("/props",                     G([this](const http_req & r) { return post_props(r); }));
    http.get ("/models",                    G([this](const http_req & r) { return get_models(r); }));
    http.get ("/v1/models",                 G([this](const http_req & r) { return get_models(r); }));
    http.post("/completion",                G([this](const http_req & r) { return post_completions(r); }));
    http.post("/completions",               G([this](const http_req & r) { return post_completions(r); }));
    http.post("/v1/completions",            G([this](const http_req & r) { return post_completions_oai(r); }));
    http.post("/chat/completions",          G([this](const http_req & r) { return post_chat_completions(r); }));
    http.post("/v1/chat/completions",       G([this](const http_req & r) { return post_chat_completions(r); }));
    http.post("/v1/chat/completions/control", G([this](const http_req & r) { return post_control(r); }));
    http.post("/v1/responses",              G([this](const http_req & r) { return post_responses_oai(r); }));
    http.post("/responses",                 G([this](const http_req & r) { return post_responses_oai(r); }));
    http.post("/v1/audio/transcriptions",   G([this](const http_req & r) { return post_transcriptions_oai(r); }));
    http.post("/audio/transcriptions",      G([this](const http_req & r) { return post_transcriptions_oai(r); }));
    http.post("/tts",                       G([this](const http_req & r) { return post_tts(r); }));
    http.post("/v1/audio/speech",           G([this](const http_req & r) { return post_speech_oai(r); }));
    http.post("/audio/speech",              G([this](const http_req & r) { return post_speech_oai(r); }));
    http.get ("/v1/realtime",               G([this](const http_req & r) { return not_supported(r, "the realtime WebSocket API is not available yet; use /v1/chat/completions with input_audio and modalities"); }));
    http.post("/v1/messages",               G([this](const http_req & r) { return post_anthropic_messages(r); }));
    http.post("/v1/messages/count_tokens",  G([this](const http_req & r) { return post_anthropic_count_tokens(r); }));
    http.post("/infill",                    G([this](const http_req & r) { return not_supported(r, "Infill is not supported by this model: the LFM3 packages have no fill-in-the-middle tokens"); }));
    http.post("/embedding",                 G([this](const http_req & r) { return not_supported(r, "embeddings are not available on the Ixian Hexagon runtime (the SDK has no embedding API)"); }));
    http.post("/embeddings",                G([this](const http_req & r) { return not_supported(r, "embeddings are not available on the Ixian Hexagon runtime (the SDK has no embedding API)"); }));
    http.post("/v1/embeddings",             G([this](const http_req & r) { return not_supported(r, "embeddings are not available on the Ixian Hexagon runtime (the SDK has no embedding API)"); }));
    http.post("/rerank",                    G([this](const http_req & r) { return not_supported(r, "reranking is not available on the Ixian Hexagon runtime"); }));
    http.post("/reranking",                 G([this](const http_req & r) { return not_supported(r, "reranking is not available on the Ixian Hexagon runtime"); }));
    http.post("/v1/rerank",                 G([this](const http_req & r) { return not_supported(r, "reranking is not available on the Ixian Hexagon runtime"); }));
    http.post("/v1/reranking",              G([this](const http_req & r) { return not_supported(r, "reranking is not available on the Ixian Hexagon runtime"); }));
    http.post("/v1/systemone",              G([this](const http_req & r) { return post_systemone(r); }));
    http.post("/tokenize",                  G([this](const http_req & r) { return post_tokenize(r); }));
    http.post("/detokenize",                G([this](const http_req & r) { return post_detokenize(r); }));
    http.post("/apply-template",            G([this](const http_req & r) { return post_apply_template(r); }));
    http.post("/chat/completions/input_tokens",    G([this](const http_req & r) { return post_chat_completions_tok(r); }));
    http.post("/v1/chat/completions/input_tokens", G([this](const http_req & r) { return post_chat_completions_tok(r); }));
    http.post("/responses/input_tokens",           G([this](const http_req & r) { return post_responses_tok_oai(r); }));
    http.post("/v1/responses/input_tokens",        G([this](const http_req & r) { return post_responses_tok_oai(r); }));
    http.get ("/lora-adapters",             G([this](const http_req & r) { return get_lora_adapters(r); }));
    http.post("/lora-adapters",             G([this](const http_req & r) { return not_supported(r, "LoRA adapters are not available on the Ixian Hexagon runtime"); }));
    http.get ("/slots",                     G([this](const http_req & r) { return get_slots(r); }));
    http.post("/slots/:id_slot",            G([this](const http_req & r) { return post_slots(r); }));
}

//
// simple endpoints
//

http_res_ptr server_routes::not_supported(const http_req &, const std::string & what) {
    auto res = make_res();
    res->error(what, ERROR_TYPE_NOT_SUPPORTED);
    return res;
}

http_res_ptr server_routes::get_health(const http_req &) {
    auto res = make_res();
    res->ok(json {{"status", "ok"}});
    return res;
}

http_res_ptr server_routes::get_metrics(const http_req &) {
    auto res = make_res();
    if (!params_.endpoint_metrics) {
        res->error("This server does not support metrics endpoint. Start it with `--metrics`", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    const engine_metrics m = eng_.metrics(true);
    struct item { const char * name; const char * help; double value; };
    const std::vector<item> counters = {
        {"prompt_tokens_total",            "Number of prompt tokens processed, excluding cached tokens", (double) m.prompt.count},
        {"prompt_tokens_cached_total",     "Number of prompt tokens reused from the cache", (double) m.n_prompt_cached},
        {"prompt_seconds_total",           "Total time spent processing prompts", m.prompt.time / 1.e6},
        {"tokens_predicted_total",         "Number of generation tokens processed", (double) m.predict.count},
        {"tokens_predicted_seconds_total", "Total time spent generating tokens", m.predict.time / 1.e6},
        {"n_decode_total",                 "Total number of decode calls (prefills and generation steps)", (double) m.n_decode},
        {"n_tokens_max",                   "Largest observed sequence length (prompt + generation)", (double) m.n_tokens_max},
        {"spec_decode_num_draft_tokens_total",    "Speculative: Total draft tokens generated", 0.0},
        {"spec_decode_num_accepted_tokens_total", "Speculative: Total draft tokens accepted by the target model", 0.0},
        {"spec_decode_num_drafts_total",          "Speculative: Total speculative decoding verification steps", 0.0},
    };
    const std::vector<item> gauges = {
        {"prompt_tokens_seconds",    "Average prompt throughput in tokens/s", m.prompt_bucket.n_per_second()},
        {"predicted_tokens_seconds", "Average generation throughput in tokens/s", m.predict_bucket.n_per_second()},
        {"requests_processing",      "Number of requests processing", eng_.is_processing() ? 1.0 : 0.0},
        {"requests_deferred",        "Number of requests deferred", (double) eng_.n_queued()},
        {"n_busy_slots_per_decode",  "Average number of busy slots per decode call", (double) m.n_busy_slots / std::max((double) m.n_decode, 1.0)},
    };
    const std::vector<item> ixian_counters = {
        {"requests_total",      "Requests served by the inference thread", (double) m.n_requests},
        {"audio_frames_total",  "Speech frames (80 ms) produced", (double) m.n_audio_frames},
        {"decisions_total",     "d1 questions answered", (double) m.n_decisions},
    };
    std::stringstream out;
    auto add = [&](const char * prefix, const char * type, const std::vector<item> & items) {
        for (const auto & it : items) {
            out << "# HELP " << prefix << it.name << " " << it.help << "\n"
                << "# TYPE " << prefix << it.name << " " << type << "\n"
                << prefix << it.name << " " << it.value << "\n";
        }
    };
    add("llamacpp:", "counter", counters);
    add("llamacpp:", "gauge", gauges);
    add("ixian:", "counter", ixian_counters);
    res->headers["Process-Start-Time-Unix"] = std::to_string(m.t_start / 1000000);
    res->content_type = "text/plain; version=0.0.4";
    res->status = 200;
    res->data = out.str();
    return res;
}

http_res_ptr server_routes::get_slots(const http_req & req) {
    auto res = make_res();
    if (!params_.endpoint_slots) {
        res->error("This server does not support slots endpoint. Start it with `--slots`", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    if (!req.get_param("fail_on_no_slot").empty() && eng_.is_processing()) {
        res->error("no slot available", ERROR_TYPE_UNAVAILABLE);
        return res;
    }
    res->ok(json::array({eng_.slot_json(true)}));
    return res;
}

http_res_ptr server_routes::post_slots(const http_req & req) {
    auto res = make_res();
    if (params_.slot_save_path.empty()) {
        res->error("This server does not support slots action. Start it with `--slot-save-path`", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    int id_slot;
    try {
        id_slot = std::stoi(req.get_param("id_slot"));
    } catch (const std::exception &) {
        res->error("Invalid slot ID", ERROR_TYPE_INVALID_REQUEST);
        return res;
    }
    if (id_slot != 0) {
        res->error("Invalid slot ID", ERROR_TYPE_INVALID_REQUEST);
        return res;
    }
    const std::string action = req.get_param("action");
    if (action == "erase") {
        return handle_slots_erase(req);
    }
    if (action == "save" || action == "restore") {
        res->error("saving and restoring the NPU KV cache is not available on the Ixian Hexagon runtime; action=erase works", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    res->error("Invalid action", ERROR_TYPE_INVALID_REQUEST);
    return res;
}

http_res_ptr server_routes::handle_slots_erase(const http_req &) {
    auto res = make_res();
    const json slot = eng_.slot_json(true);
    const int n_erased = json_value(slot, "n_prompt_tokens", 0);
    eng_.erase_cache();
    res->ok(json {{"id_slot", 0}, {"n_erased", n_erased}});
    return res;
}

json server_routes::props_json() const {
    task_params defaults;
    defaults.sampling.temp = params_.temperature;
    defaults.sampling.top_k = params_.top_k;
    defaults.sampling.top_p = params_.top_p;
    defaults.sampling.min_p = params_.min_p;
    defaults.sampling.penalty_repeat = params_.repeat_penalty;
    defaults.sampling.penalty_last_n = params_.repeat_last_n;
    defaults.sampling.seed = params_.seed;
    defaults.n_predict = params_.n_predict;
    defaults.cache_prompt = params_.cache_prompt;
    defaults.enable_thinking = params_.reasoning == "on";
    defaults.reasoning_fmt = reasoning_format_from_name(params_.reasoning_format);

    const auto & caps = eng_.caps();
    const auto & info = eng_.info();

    json ui_settings = json::object();
    if (!params_.ui_config.empty()) {
        try { ui_settings = json::parse(params_.ui_config); } catch (...) {}
    }
    std::string chat_template;
    if (eng_.has_llm()) {
        chat_template.assign((const char *) ixsrv_asset_lfm3_jinja, ixsrv_asset_lfm3_jinja_size);
    }

    json props = {
        {"default_generation_settings", json {{"params", defaults.to_json(true)}, {"n_ctx", caps.max_context}}},
        {"total_slots",                 1},
        {"model_alias",                 eng_.model_name()},
        {"model_ftype",                 "hexagon-" + info.target},
        {"model_path",                  params_.model_path},
        {"modalities",                  json {{"vision", caps.image}, {"video", caps.video}, {"audio", caps.audio}}},
        {"media_marker",                get_media_marker()},
        {"endpoint_slots",              params_.endpoint_slots},
        {"endpoint_props",              params_.endpoint_props},
        {"endpoint_metrics",            params_.endpoint_metrics},
        {"ui",                          params_.ui},
        {"ui_settings",                 ui_settings},
        {"chat_template",               chat_template},
        {"chat_template_caps",          json {
            {"supports_tools", true}, {"supports_tool_calls", true}, {"supports_tool_responses", true},
            {"supports_system_role", true}, {"supports_parallel_tool_calls", true}, {"supports_preserve_reasoning", false},
            {"supports_string_content", true}, {"supports_typed_content", true}, {"requires_typed_content", false},
            {"supports_enable_thinking", eng_.has_llm()},
        }},
        {"bos_token",                   eng_.bos_str()},
        {"eos_token",                   eng_.eos_str()},
        {"build_info",                  std::string(build_info())},
        {"is_sleeping",                 false},
        {"cors_proxy_enabled",          false},
        {"ixian",                       json {
            {"server_version",   server_version()},
            {"sdk_version",      info.sdk_version.empty() ? std::string(sdk_version()) : info.sdk_version},
            {"engine",           IXSRV_ENGINE_NAME},
            {"model_id",         info.model_id},
            {"revision",         info.revision},
            {"target",           info.target},
            {"hexagon_arch",     info.hexagon_arch},
            {"nsp_count",        info.nsp_count},
            {"soc",              info.soc},
            {"package_version",  info.package_version},
            {"runtime_commit",   info.runtime_commit},
            {"capabilities",     json {
                {"text", caps.text}, {"image", caps.image}, {"audio", caps.audio}, {"video", caps.video},
                {"speech_out", caps.speech_out}, {"decision", caps.decision}, {"max_context", caps.max_context},
                {"input_sample_rate", caps.input_sample_rate}, {"speech_sample_rate", caps.speech_sample_rate},
            }},
            {"image_fit",        image_fit_name(params_.image_fit)},
            {"image_size",       512},
        }},
    };
    if (caps.speech_out) {
        props["voices"] = json::array({"female", "male"});
    }
    return props;
}

http_res_ptr server_routes::get_props(const http_req &) {
    auto res = make_res();
    res->ok(props_json());
    return res;
}

http_res_ptr server_routes::post_props(const http_req &) {
    auto res = make_res();
    if (!params_.endpoint_props) {
        res->error("This server does not support changing global properties. Start it with `--props`", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    res->ok(json {{"success", true}});
    return res;
}

json server_routes::models_json() const {
    const auto & caps = eng_.caps();
    const auto & info = eng_.info();
    std::vector<std::string> input_modalities = {"text"};
    if (caps.image) input_modalities.push_back("image");
    if (caps.audio) input_modalities.push_back("audio");
    if (caps.video) input_modalities.push_back("video");
    std::vector<std::string> output_modalities;
    if (caps.decision) {
        output_modalities.push_back("decisions");
    } else {
        output_modalities.push_back("text");
        if (caps.speech_out) output_modalities.push_back("audio");
    }
    json capabilities = json::array({"completion"});
    if (caps.image || caps.audio || caps.video) {
        capabilities.push_back("multimodal");
    }
    if (caps.decision) {
        capabilities = json::array({"decision"});
        if (caps.image || caps.audio) capabilities.push_back("multimodal");
    }
    std::string family = string_lower(info.model_id);
    const size_t slash = family.find('/');
    if (slash != std::string::npos) family = family.substr(slash + 1);
    const size_t dash = family.find('-');
    if (dash != std::string::npos) family = family.substr(0, dash);

    json data_entry = {
        {"id",       eng_.model_name()},
        {"aliases",  params_.model_alias},
        {"tags",     params_.model_tags},
        {"object",   "model"},
        {"architecture", json {{"input_modalities", input_modalities}, {"output_modalities", output_modalities}}},
        {"created",  (int64_t) std::time(nullptr)},
        {"owned_by", "ixian"},
        {"meta", json {
            {"vocab_type",  2},
            {"n_vocab",     0},
            {"n_ctx",       caps.max_context},
            {"n_ctx_train", 32768},
            {"n_embd",      0},
            {"n_params",    0},
            {"size",        0},
            {"ftype",       "hexagon-" + info.target},
            {"model_id",    info.model_id},
            {"revision",    info.revision},
            {"target",      info.target},
            {"nsp_count",   info.nsp_count},
        }},
    };
    return json {
        {"models", json::array({json {
            {"name",  eng_.model_name()},
            {"model", eng_.model_name()},
            {"modified_at", ""},
            {"size", ""},
            {"digest", ""},
            {"type", "model"},
            {"description", info.model_id},
            {"tags", json::array({""})},
            {"capabilities", capabilities},
            {"parameters", ""},
            {"details", json {
                {"parent_model", info.model_id},
                {"format", "ixian-hexagon"},
                {"family", family},
                {"families", json::array({family})},
                {"parameter_size", ""},
                {"quantization_level", info.target},
            }},
        }})},
        {"object", "list"},
        {"data", json::array({data_entry})},
    };
}

http_res_ptr server_routes::get_models(const http_req &) {
    auto res = make_res();
    res->ok(models_json());
    return res;
}

http_res_ptr server_routes::get_lora_adapters(const http_req &) {
    auto res = make_res();
    res->ok(json::array());
    return res;
}

http_res_ptr server_routes::post_control(const http_req & req) {
    auto res = make_res();
    const json body = parse_body(req);
    if (json_value(body, "id", std::string()).empty()) {
        res->error("missing completion id", ERROR_TYPE_INVALID_REQUEST);
        return res;
    }
    if (json_value(body, "action", std::string()) != "reasoning_end") {
        res->error("unknown control action", ERROR_TYPE_INVALID_REQUEST);
        return res;
    }
    res->error("reasoning control is not available on the Ixian Hexagon runtime", ERROR_TYPE_NOT_SUPPORTED);
    return res;
}

//
// tokenizer endpoints
//

http_res_ptr server_routes::post_tokenize(const http_req & req) {
    auto res = make_res();
    const json body = parse_body(req);
    json tokens_response = json::array();
    if (body.contains("content")) {
        const bool add_special   = json_value(body, "add_special", false);
        const bool parse_special = json_value(body, "parse_special", true);
        const bool with_pieces   = json_value(body, "with_pieces", false);
        const json & content = body.at("content");

        std::vector<int32_t> tokens;
        if (content.is_string()) {
            tokens = eng_.tokenize(content.get<std::string>(), parse_special);
        } else if (content.is_array()) {
            for (const auto & p : content) {
                if (p.is_string()) {
                    const auto t = eng_.tokenize(p.get<std::string>(), parse_special);
                    tokens.insert(tokens.end(), t.begin(), t.end());
                } else if (p.is_number_integer()) {
                    tokens.push_back(p.get<int32_t>());
                } else {
                    throw std::invalid_argument("\"content\" must be a string or an array of strings and token ids");
                }
            }
        } else {
            throw std::invalid_argument("\"content\" must be a string or an array of strings and token ids");
        }
        if (add_special) {
            const int32_t bos = eng_.bos_token();
            if (bos >= 0 && (tokens.empty() || tokens[0] != bos)) {
                tokens.insert(tokens.begin(), bos);
            }
        }
        if (with_pieces) {
            for (const auto token : tokens) {
                const std::string piece = eng_.token_piece(token);
                json piece_json;
                if (is_valid_utf8(piece)) {
                    piece_json = piece;
                } else {
                    piece_json = str_to_bytes_json(piece);
                }
                tokens_response.push_back(json {{"id", token}, {"piece", piece_json}});
            }
        } else {
            tokens_response = tokens;
        }
    }
    res->ok(json {{"tokens", tokens_response}});
    return res;
}

http_res_ptr server_routes::post_detokenize(const http_req & req) {
    auto res = make_res();
    const json body = parse_body(req);
    std::string content;
    if (body.contains("tokens")) {
        if (!body.at("tokens").is_array()) {
            throw std::invalid_argument("\"tokens\" must be an array of token ids");
        }
        content = eng_.detokenize(body.at("tokens").get<std::vector<int32_t>>());
    }
    res->ok(json {{"content", content}});
    return res;
}

http_res_ptr server_routes::post_apply_template(const http_req & req) {
    auto res = make_res();
    const json body = parse_body(req);
    const chat_inputs in = oai_chat_to_sdk(body, convert_options());
    const bool add_generation_prompt = json_value(body, "add_generation_prompt", true);
    res->ok(json {{"prompt", eng_.apply_template(in.messages, add_generation_prompt, in.enable_thinking)}});
    return res;
}

http_res_ptr server_routes::handle_count_tokens(const http_req & req, task_response_type res_type) {
    auto res = make_res();
    json body = parse_body(req);
    bool is_oai = false;
    switch (res_type) {
        case TASK_RESPONSE_TYPE_OAI_CHAT:  is_oai = true; break;
        case TASK_RESPONSE_TYPE_OAI_RESP:  is_oai = true; body = convert_responses_to_chatcmpl(body); break;
        case TASK_RESPONSE_TYPE_ANTHROPIC: body = convert_anthropic_to_oai(body); break;
        default:
            res->error("invalid res_type", ERROR_TYPE_INVALID_REQUEST);
            return res;
    }
    const chat_inputs in = oai_chat_to_sdk(body, convert_options());
    task_params p = task_params_from_json(body, params_, eng_.n_ctx());
    p.enable_thinking = in.enable_thinking;
    const zx::PromptInfo info = eng_.inspect(in.messages, p.to_sdk(1));
    json response = {{"input_tokens", (int64_t) info.rows}};
    if (is_oai) {
        response["object"] = "response.input_tokens";
    }
    res->ok(response);
    return res;
}

http_res_ptr server_routes::post_chat_completions_tok(const http_req & req) {
    return handle_count_tokens(req, TASK_RESPONSE_TYPE_OAI_CHAT);
}

http_res_ptr server_routes::post_responses_tok_oai(const http_req & req) {
    return handle_count_tokens(req, TASK_RESPONSE_TYPE_OAI_RESP);
}

http_res_ptr server_routes::post_anthropic_count_tokens(const http_req & req) {
    return handle_count_tokens(req, TASK_RESPONSE_TYPE_ANTHROPIC);
}

//
// completions
//

void server_routes::log_prompt(const std::string & prompt) const {
    if (params_.prompts_log_dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(params_.prompts_log_dir, ec);
    const auto file_path = std::filesystem::path(params_.prompts_log_dir) / string_format("%012lld.txt", (long long) time_ms());
    std::ofstream f(file_path);
    if (f) {
        f << prompt;
    }
}

void server_routes::prompts_from_json(const json & prompt, std::vector<std::vector<zx::Message>> & prompts, std::vector<std::string> & texts) {
    const media_options mopts = media_opts();

    auto one = [&](const json & p) {
        std::vector<zx::Part> parts;
        std::string text;
        if (p.is_string()) {
            text = p.get<std::string>();
            parts.push_back(zx::Part::Text(text));
        } else if (json_is_array_of_numbers(p)) {
            text = eng_.detokenize(p.get<std::vector<int32_t>>());
            parts.push_back(zx::Part::Text(text));
        } else if (json_is_array_of_mixed_numbers_strings(p)) {
            for (const auto & e : p) {
                if (e.is_string()) text += e.get<std::string>();
                else text += eng_.detokenize({e.get<int32_t>()});
            }
            parts.push_back(zx::Part::Text(text));
        } else if (p.is_object() && p.contains("prompt_string")) {
            const json & ps = p.at("prompt_string");
            if (ps.is_string()) {
                text = ps.get<std::string>();
            } else if (json_is_array_of_numbers(ps)) {
                text = eng_.detokenize(ps.get<std::vector<int32_t>>());
            } else {
                throw std::invalid_argument("\"prompt_string\" must be a string or a list of tokens");
            }
            std::vector<raw_buffer> files;
            if (p.contains("multimodal_data")) {
                const json & md = p.at("multimodal_data");
                if (!mopts.allow_image && !mopts.allow_audio && !mopts.allow_video) {
                    throw std::runtime_error("Multimodal data provided, but model does not support multimodal requests.");
                }
                if (md.is_array()) {
                    for (const auto & entry : md) files.push_back(load_media_url(entry.is_string() ? entry.get<std::string>() : entry.dump(), mopts));
                } else if (md.is_string()) {
                    files.push_back(load_media_url(md.get<std::string>(), mopts));
                } else {
                    throw std::invalid_argument("\"multimodal_data\" must be an array of base64 strings");
                }
            }
            // the media go where the markers are, extra media at the end
            const std::string marker = get_media_marker();
            size_t pos = 0;
            size_t file_idx = 0;
            while (true) {
                const size_t m = text.find(marker, pos);
                if (m == std::string::npos) {
                    const std::string tail = text.substr(pos);
                    if (!tail.empty()) parts.push_back(zx::Part::Text(tail));
                    break;
                }
                const std::string before = text.substr(pos, m - pos);
                if (!before.empty()) parts.push_back(zx::Part::Text(before));
                if (file_idx < files.size()) {
                    for (auto & part : media_to_parts(files[file_idx], "", mopts)) parts.push_back(std::move(part));
                    file_idx++;
                }
                pos = m + marker.size();
            }
            for (; file_idx < files.size(); file_idx++) {
                for (auto & part : media_to_parts(files[file_idx], "", mopts)) parts.push_back(std::move(part));
            }
            if (parts.empty()) parts.push_back(zx::Part::Text(""));
        } else {
            throw std::invalid_argument("\"prompt\" elements must be a string, a list of tokens, a JSON object containing a prompt string, or a list of mixed strings & tokens.");
        }
        prompts.push_back({zx::Message::User(std::move(parts))});
        texts.push_back(text);
    };

    if (prompt.is_array() && !json_is_array_and_contains_numbers(prompt)) {
        for (const auto & p : prompt) one(p);
    } else {
        one(prompt);
    }
    if (prompts.empty()) {
        throw std::invalid_argument("\"prompt\" must not be empty");
    }
}

http_res_ptr server_routes::handle_completions_impl(const http_req & req, json body,
                                                    std::vector<std::vector<zx::Message>> prompts, std::vector<std::string> prompt_texts,
                                                    task_response_type res_type, const chat_inputs * chat) {
    auto res = make_res();
    if (!eng_.has_llm()) {
        res->error("this server does not support chat/completions (the loaded package is a d1 decision model: use /v1/systemone)", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }

    task_params params = task_params_from_json(body, params_, eng_.n_ctx());
    params.res_type = res_type;
    params.oaicompat_model = eng_.model_name();
    params.oaicompat_cmpl_id = gen_chatcmplid();
    params.transcription_format = json_value(body, "__transcription_format", std::string("json"));
    if (chat) {
        params.enable_thinking = chat->enable_thinking;
        params.reasoning_fmt = chat->reasoning_fmt;
    } else {
        params.enable_thinking = false;
        params.reasoning_fmt = reasoning_format_from_name(params_.reasoning_format);
    }
    if (params.want_audio) {
        if (!eng_.caps().speech_out) {
            res->error("this package has no speech output (capabilities.speech_out false)", ERROR_TYPE_NOT_SUPPORTED);
            return res;
        }
        // the voice and the language go to the system prompt, as llama-server's speech prompt
        const std::string voice = params.audio_voice.empty() ? params_.tts_voice : params.audio_voice;
        const std::string lang = params.audio_lang.empty() ? params_.tts_lang : params.audio_lang;
        const std::string speak = lfm3_speech_system_prompt(lang, voice);
        if (!speak.empty() && params.want_text) {
            for (auto & msgs : prompts) {
                if (!msgs.empty() && msgs.front().role == zx::Role::System) {
                    std::string text = msgs.front().text();
                    msgs.front() = zx::Message::System(text.empty() ? speak : text + "\n" + speak);
                } else {
                    msgs.insert(msgs.begin(), zx::Message::System(speak));
                }
            }
        } else if (!params.want_text) {
            // {Audio} only: the SDK speaks the user text (TTS); the prompt names the language and the voice
            for (auto & msgs : prompts) {
                const std::string tts = lfm3_tts_system_prompt(lang, voice);
                if (!msgs.empty() && msgs.front().role == zx::Role::System) {
                    msgs.front() = zx::Message::System(tts);
                } else {
                    msgs.insert(msgs.begin(), zx::Message::System(tts));
                }
            }
        }
    }

    // the rendered prompt, for GET /slots, --log-prompts-dir and the native "prompt" field
    std::vector<std::string> rendered;
    rendered.reserve(prompts.size());
    for (size_t i = 0; i < prompts.size(); i++) {
        std::string r;
        try {
            r = eng_.apply_template(prompts[i], true, params.enable_thinking);
        } catch (const std::exception & e) {
            SRV_WRN("cannot render the prompt: %s\n", e.what());
            r = i < prompt_texts.size() ? prompt_texts[i] : "";
        }
        rendered.push_back(r);
        log_prompt(r);
    }

    const bool stream = params.stream;
    const int32_t n_ctx = eng_.n_ctx();
    const std::vector<chat_tool> tools = chat ? chat->tools : std::vector<chat_tool>{};
    const bool tools_active = chat && !chat->tools.empty() && chat->choice.kind != tool_choice_kind::none;
    const std::string first_prompt = rendered.empty() ? "" : rendered[0];

    auto channel = std::make_shared<task_channel>();
    const int task_id = eng_.post([=, prompts = std::move(prompts), rendered = std::move(rendered)](engine & eng, engine::task & task) mutable {
        json results = json::array();
        bool any_streamed = false;
        size_t result_index = 0;

        for (size_t pi = 0; pi < prompts.size(); pi++) {
            for (int32_t ci = 0; ci < params.n_cmpl; ci++) {
                if (task.cancelled.load() || channel->consumer_gone()) {
                    return;
                }
                completion_result r;
                r.index = result_index++;
                r.params = params;
                r.prompt = rendered[pi];
                if (params.n_cmpl > 1 && params.sampling.seed >= 0) {
                    r.params.sampling.seed = params.sampling.seed + ci;  // one seed per choice
                }

                // how many tokens fit: the SDK reserves prompt rows + max_tokens (+ speech frames) up front
                zx::PromptInfo info;
                try {
                    info = eng.llm().inspect(prompts[pi], r.params.to_sdk(1));
                } catch (const zx::Error & e) {
                    throw status_to_api_error(e.status(), e.what());
                }
                const int32_t audio_reserve = r.params.want_audio ? (int32_t) std::ceil(r.params.max_audio_seconds * 12.5) + 1 : 0;
                int32_t room = n_ctx - info.rows - audio_reserve;
                int32_t max_tokens = r.params.n_predict > 0 ? std::min(r.params.n_predict, room) : room;
                if (room <= 0 || max_tokens <= 0) {
                    const api_error err(ERROR_TYPE_EXCEED_CONTEXT_SIZE,
                        string_format("the request needs %d prompt rows + %d reply rows, the context holds %d (%s)",
                                      info.rows, std::max(1, r.params.n_predict > 0 ? r.params.n_predict : 1) + audio_reserve, n_ctx,
                                      "the context size has been exceeded"),
                        json {{"n_prompt_tokens", info.rows}, {"n_ctx", n_ctx}});
                    if (any_streamed) {
                        channel->push(format_sse_error(res_type, err.to_json()));
                        channel->close();
                        return;
                    }
                    channel->set_error(err);
                    return;
                }
                r.params.n_predict = max_tokens;
                r.n_prompt_tokens = info.rows;
                r.n_prompt_tokens_cache = info.cached_rows;
                task.params = r.params.to_json(true);
                task.prompt = r.prompt;
                eng.slot_begin(task, info.rows);

                zx::GenerationConfig cfg = r.params.to_sdk(max_tokens);
                lfm3_tool_parser parser(tools_active, tools);
                const int64_t t_start = time_us();
                int64_t t_first = 0;
                bool first = true;

                // the request is valid: a stream sends its headers now (an empty chunk), so the client sees the 200
                // and the keep-alive pings while the prompt is processed; errors from here on are stream events
                if (stream && !any_streamed) {
                    any_streamed = true;
                    channel->push("");
                }

                auto emit = [&](completion_delta & d) -> bool {
                    if (!stream) return !channel->consumer_gone();
                    const json j = to_json_partial(r, d);
                    const std::string text = format_sse(res_type, j);
                    if (text.empty()) return !channel->consumer_gone();
                    any_streamed = true;
                    return channel->push(text);
                };

                if (r.params.return_progress) {
                    completion_delta d;
                    d.is_progress = true;
                    d.progress = json {{"total", info.rows}, {"cache", info.cached_rows}, {"processed", info.rows}, {"time_ms", 0}};
                    d.with_timings = r.params.timings_per_token;
                    emit(d);
                }

                auto on_event = [&](const zx::StreamEvent & ev) -> bool {
                    if (task.cancelled.load() || channel->consumer_gone()) {
                        return false;
                    }
                    if (ev.type == zx::StreamEvent::Type::Done) {
                        return true;
                    }
                    completion_delta d;
                    if (ev.type == zx::StreamEvent::Type::Text) {
                        if (first) {
                            t_first = time_us();
                            first = false;
                            d.first = true;
                        }
                        r.n_decoded++;
                        r.tokens.push_back(ev.token);
                        const std::string piece(ev.text);
                        if (ev.reasoning) {
                            d.reasoning = piece;
                            r.reasoning += piece;
                        } else {
                            std::vector<tool_call_out> completed;
                            parser.feed(piece, d.content, completed);
                            for (auto & tc : completed) {
                                r.tool_calls.push_back(tc);
                                d.tool_calls.emplace_back(r.tool_calls.size() - 1, tc);
                            }
                            r.content += d.content;
                        }
                        if (piece.find('\n') != std::string::npos) {
                            r.has_new_line = true;
                        }
                        if (ev.logprob) {
                            token_prob tp;
                            tp.tok = ev.logprob->token;
                            tp.logprob = ev.logprob->logprob;
                            tp.text = ev.logprob->piece;
                            for (const auto & [id, lp] : ev.logprob->top) {
                                token_prob t;
                                t.tok = id;
                                t.logprob = lp;
                                t.text = eng.token_piece(id);
                                tp.top.push_back(std::move(t));
                            }
                            r.probs.push_back(tp);
                            d.probs.push_back(tp);
                        }
                        eng.slot_progress(r.n_decoded, max_tokens - r.n_decoded, r.has_new_line, piece);
                        d.with_timings = r.params.timings_per_token;
                        if (d.with_timings) {
                            r.timings.is_set = true;
                            r.timings.cache_n = info.cached_rows;
                            r.timings.prompt_n = info.rows - info.cached_rows;
                            r.timings.prompt_ms = (t_first - t_start) / 1000.0;
                            r.timings.predicted_n = r.n_decoded;
                            r.timings.predicted_ms = (time_us() - t_first) / 1000.0;
                            r.timings.predicted_steps = std::max(0, r.n_decoded - 1);
                        }
                    } else if (ev.type == zx::StreamEvent::Type::Audio && ev.audio) {
                        if (first) {
                            t_first = time_us();
                            first = false;
                            d.first = true;
                        }
                        r.audio_sample_rate = ev.audio->sample_rate;
                        if (stream) {
                            d.audio_bytes = audio_bytes(ev.audio->pcm, ev.audio->samples, r.params.audio_format == "wav" ? "pcm16" : r.params.audio_format);
                        }
                    }
                    return emit(d);
                };

                zx::Response resp = eng.llm().chat(prompts[pi], cfg, on_event);
                eng.slot_end(task);

                if (resp.status != zx::Status::Ok) {
                    const api_error err = status_to_api_error(resp.status, resp.error, info.rows, n_ctx);
                    SRV_WRN("task %d: %s\n", task.id, err.what());
                    if (any_streamed) {
                        channel->push(format_sse_error(res_type, err.to_json()));
                        channel->close();
                    } else {
                        channel->set_error(err);
                    }
                    return;
                }

                // flush the tool parser, fill the result from the response
                {
                    completion_delta d;
                    std::vector<tool_call_out> completed;
                    parser.finish(d.content, completed);
                    for (auto & tc : completed) {
                        r.tool_calls.push_back(tc);
                        d.tool_calls.emplace_back(r.tool_calls.size() - 1, tc);
                    }
                    r.content += d.content;
                    if (!d.content.empty() || !d.tool_calls.empty()) {
                        emit(d);
                    }
                }
                if (!tools_active) {
                    r.content = resp.text;
                }
                if (!resp.reasoning_content.empty()) {
                    r.reasoning = resp.reasoning_content;
                }
                if (!resp.tokens.empty()) {
                    r.tokens = resp.tokens;
                }
                r.n_decoded = resp.usage.completion_tokens > 0 ? resp.usage.completion_tokens : r.n_decoded;
                r.n_prompt_tokens = resp.usage.prompt_rows > 0 ? resp.usage.prompt_rows : info.rows;
                r.n_prompt_tokens_cache = resp.usage.cached_rows;
                r.stopping_word = (resp.finish_reason == zx::FinishReason::StopSequence && r.params.antiprompt.size() == 1) ? r.params.antiprompt[0] : "";
                if (resp.finish_reason == zx::FinishReason::StopSequence && r.params.antiprompt.size() > 1) {
                    r.stopping_word = r.params.antiprompt[0];
                }
                r.stop = stop_type_of(resp.finish_reason, !r.stopping_word.empty());
                r.truncated = false;
                r.audio = std::move(resp.audio);
                if (!r.params.stream) {
                    // every token's probabilities were collected on the way (post_sampling: the same values)
                }
                r.timings.is_set = true;
                r.timings.cache_n = r.n_prompt_tokens_cache;
                r.timings.prompt_n = r.n_prompt_tokens - r.n_prompt_tokens_cache;
                r.timings.prompt_ms = resp.timings.ttft > 0 ? resp.timings.ttft : (t_first > 0 ? (t_first - t_start) / 1000.0 : 0.0);
                r.timings.predicted_n = r.n_decoded;
                r.timings.predicted_ms = resp.timings.decode > 0 ? resp.timings.decode : (t_first > 0 ? (time_us() - t_first) / 1000.0 : 0.0);
                r.timings.predicted_steps = resp.timings.decode_steps > 0 ? resp.timings.decode_steps : std::max(0, r.n_decoded - 1);
                r.timings.predicted_per_second_sdk = resp.timings.tokens_per_second;
                r.timings.ttft_ms = resp.timings.ttft;
                r.timings.encode_ms = resp.timings.encode;
                r.timings.audio_seconds = resp.timings.audio_seconds;
                r.timings.realtime_factor = resp.timings.realtime_factor;
                r.timings.audio_frames = resp.usage.audio_frames;

                task.stats.n_prompt_tokens = r.n_prompt_tokens;
                task.stats.n_prompt_tokens_cache = r.n_prompt_tokens_cache;
                task.stats.n_decoded = r.n_decoded;
                task.stats.n_decode_steps = r.timings.predicted_steps;
                task.stats.t_prompt_us = (int64_t) (r.timings.prompt_ms * 1000.0);
                task.stats.t_gen_us = (int64_t) (r.timings.predicted_ms * 1000.0);
                task.stats.n_audio_frames = resp.usage.audio_frames;
                eng.record_stats(task.stats);

                if (resp.finish_reason != zx::FinishReason::Cancelled && r.tool_calls.empty() && !resp.text.empty()) {
                    eng.remember_reply(resp.message);
                }

                SRV_INF("task %d done: prompt %d rows (%d cached), %d tokens, %s, %.1f t/s\n", task.id, r.n_prompt_tokens,
                        r.n_prompt_tokens_cache, r.n_decoded, zx::finishReasonName(resp.finish_reason), r.timings.predicted_per_second_sdk);

                if (stream) {
                    const std::string text = format_sse(res_type, r.to_json_final());
                    if (!text.empty()) {
                        any_streamed = true;
                        channel->push(text);
                    }
                } else {
                    results.push_back(r.to_json_final());
                }
            }
        }

        if (stream) {
            if (res_type == TASK_RESPONSE_TYPE_OAI_CHAT || res_type == TASK_RESPONSE_TYPE_OAI_CMPL || res_type == TASK_RESPONSE_TYPE_OAI_ASR) {
                channel->push("data: [DONE]\n\n");
            }
            channel->close();
            return;
        }
        if (results.size() == 1) {
            channel->set_final(results[0]);
        } else if (res_type == TASK_RESPONSE_TYPE_OAI_CHAT || res_type == TASK_RESPONSE_TYPE_OAI_CMPL) {
            json merged = results[0];
            json & usage = merged["usage"];
            json & choices = merged["choices"];
            int64_t n_completion_tokens = 0;
            for (auto & item : results) {
                n_completion_tokens += item["usage"]["completion_tokens"].get<int64_t>();
            }
            for (size_t i = 1; i < results.size(); i++) {
                choices.push_back(results[i]["choices"][0]);
            }
            usage["completion_tokens"] = n_completion_tokens;
            usage["total_tokens"] = usage["prompt_tokens"].get<int64_t>() + n_completion_tokens;
            channel->set_final(merged);
        } else {
            channel->set_final(results);
        }
    }, channel, params.to_json(true), first_prompt);

    // the first result decides the status: an error before any data is a plain JSON error, like llama-server
    const auto state = channel->wait_started(req.should_stop);
    if (channel->consumer_gone() && state == task_channel::state::pending) {
        eng_.cancel_task(task_id);
        return res;  // the client left
    }
    if (state == task_channel::state::error) {
        res->error(channel->error());
        return res;
    }
    if (!stream) {
        if (state == task_channel::state::final) {
            res->ok(channel->final());
            return res;
        }
        // closed without a result (cancelled)
        res->error("the request was cancelled", ERROR_TYPE_SERVER);
        return res;
    }

    res->status = 200;
    res->content_type = "text/event-stream";
    const int ping_ms = params.sse_ping_interval > 0 ? params.sse_ping_interval * 1000 : 60 * 60 * 1000;
    const bool ping_enabled = params.sse_ping_interval > 0;
    struct stream_state { int64_t last_activity = time_ms(); };
    auto st = std::make_shared<stream_state>();
    res->next = [channel, ping_ms, ping_enabled, st](std::string & output) -> bool {
        while (true) {
            if (channel->next_chunk(output, 250)) {
                st->last_activity = time_ms();
                return true;
            }
            if (channel->closed_and_drained() || channel->consumer_gone()) {
                output.clear();
                return false;
            }
            // nothing for a while: a comment keeps the connection alive (some clients time out on silence)
            if (ping_enabled && time_ms() - st->last_activity >= ping_ms) {
                st->last_activity = time_ms();
                output = ":\n\n";
                return true;
            }
        }
    };
    res->on_complete = [channel, task_id, this]() {
        if (!channel->closed_and_drained()) {
            channel->mark_consumer_gone();
            eng_.cancel_task(task_id);
        }
    };
    return res;
}

http_res_ptr server_routes::post_completions(const http_req & req) {
    const json body = parse_body(req);
    if (!body.contains("prompt")) {
        throw std::invalid_argument("\"prompt\" is required");
    }
    std::vector<std::vector<zx::Message>> prompts;
    std::vector<std::string> texts;
    prompts_from_json(body.at("prompt"), prompts, texts);
    return handle_completions_impl(req, body, std::move(prompts), std::move(texts), TASK_RESPONSE_TYPE_NONE, nullptr);
}

http_res_ptr server_routes::post_completions_oai(const http_req & req) {
    json body = parse_body(req);
    if (!body.contains("prompt")) {
        throw std::invalid_argument("\"prompt\" is required");
    }
    if (json_value(body, "echo", false)) {
        throw std::invalid_argument("Only no echo is supported");
    }
    for (const char * param : {"best_of", "suffix"}) {
        if (body.contains(param) && !body.at(param).is_null()) {
            throw std::invalid_argument(std::string("Unsupported param: ") + param);
        }
    }
    std::vector<std::vector<zx::Message>> prompts;
    std::vector<std::string> texts;
    prompts_from_json(body.at("prompt"), prompts, texts);
    return handle_completions_impl(req, body, std::move(prompts), std::move(texts), TASK_RESPONSE_TYPE_OAI_CMPL, nullptr);
}

http_res_ptr server_routes::post_chat_completions(const http_req & req) {
    const json body = parse_body(req);
    chat_inputs in = oai_chat_to_sdk(body, convert_options());
    std::vector<std::vector<zx::Message>> prompts;
    prompts.push_back(std::move(in.messages));
    return handle_completions_impl(req, body, std::move(prompts), {""}, TASK_RESPONSE_TYPE_OAI_CHAT, &in);
}

http_res_ptr server_routes::post_responses_oai(const http_req & req) {
    const json body = convert_responses_to_chatcmpl(parse_body(req));
    SRV_DBG("Request converted: OpenAI Responses -> OpenAI Chat Completions: %s\n", safe_json_to_str(body).c_str());
    chat_inputs in = oai_chat_to_sdk(body, convert_options());
    std::vector<std::vector<zx::Message>> prompts;
    prompts.push_back(std::move(in.messages));
    return handle_completions_impl(req, body, std::move(prompts), {""}, TASK_RESPONSE_TYPE_OAI_RESP, &in);
}

http_res_ptr server_routes::post_anthropic_messages(const http_req & req) {
    const json body = convert_anthropic_to_oai(parse_body(req));
    SRV_DBG("Request converted: Anthropic -> OpenAI Chat Completions: %s\n", safe_json_to_str(body).c_str());
    chat_inputs in = oai_chat_to_sdk(body, convert_options());
    std::vector<std::vector<zx::Message>> prompts;
    prompts.push_back(std::move(in.messages));
    return handle_completions_impl(req, body, std::move(prompts), {""}, TASK_RESPONSE_TYPE_ANTHROPIC, &in);
}

http_res_ptr server_routes::post_transcriptions_oai(const http_req & req) {
    auto res = make_res();
    if (!eng_.has_llm() || !eng_.caps().audio) {
        res->error("The current model does not support audio input.", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    std::vector<raw_buffer> files;
    json body = convert_transcriptions_to_chatcmpl(parse_body(req), req.files, files, "");
    const std::string language = json_value(body, "language", std::string());
    const std::string prompt = json_value(parse_body(req), "prompt", std::string());

    media_options mopts = media_opts();
    const std::string kind = sniff_media_kind(files[0].data(), files[0].size());
    if (!kind.empty() && kind != "audio") {
        throw std::invalid_argument("the uploaded file is not an audio file (" + kind + ")");
    }
    std::vector<zx::Part> parts;
    for (auto & p : media_to_parts(files[0], "audio", mopts)) parts.push_back(std::move(p));
    if (!prompt.empty()) {
        parts.push_back(zx::Part::Text(prompt));
    }
    std::vector<zx::Message> msgs;
    msgs.push_back(zx::Message::System(lfm3_asr_system_prompt(language)));
    msgs.push_back(zx::Message::User(std::move(parts)));
    body["audio"] = json {{"language", language}};  // for verbose_json
    body.erase("messages");

    const std::string format = json_value(body, "__transcription_format", std::string("json"));
    std::vector<std::vector<zx::Message>> prompts;
    prompts.push_back(std::move(msgs));
    auto out = handle_completions_impl(req, body, std::move(prompts), {prompt}, TASK_RESPONSE_TYPE_OAI_ASR, nullptr);
    if (format == "text" && !out->is_stream() && out->status == 200) {
        try {
            const json j = json::parse(out->data);
            out->data = json_value(j, "text", std::string()) + "\n";
            out->content_type = "text/plain; charset=utf-8";
        } catch (...) {
        }
    }
    return out;
}

//
// speech
//

http_res_ptr server_routes::handle_tts_impl(const http_req & req, json body) {
    auto res = make_res();
    if (!eng_.has_llm() || !eng_.caps().speech_out) {
        res->error("this server does not support audio generation", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    const std::string input = json_value(body, "input", json_value(body, "prompt", std::string()));
    if (input.empty()) {
        res->error("\"input\" must be a non-empty string", ERROR_TYPE_INVALID_REQUEST);
        return res;
    }
    const std::string response_format = json_value(body, "response_format", std::string("wav"));
    if (response_format != "wav" && response_format != "pcm" && response_format != "pcm16") {
        res->error("response_format must be wav, pcm or pcm16", ERROR_TYPE_INVALID_REQUEST);
        return res;
    }
    const bool pcm16 = response_format == "pcm16";
    const bool stream = json_value(body, "stream", false);
    const std::string lang = json_value(body, "lang", json_value(body, "language", params_.tts_lang));
    const std::string voice = json_value(body, "voice", params_.tts_voice);
    if (!voice.empty() && !lfm3_is_voice(voice)) {
        res->error("unknown voice \"" + voice + "\", see \"voices\" in GET /props", ERROR_TYPE_INVALID_REQUEST);
        return res;
    }
    if (req.files.count("speaker_ref") || !json_value(body, "speaker_ref_b64", std::string()).empty()) {
        res->error("voice cloning (speaker_ref) is not available: the LFM3 packages speak with the female or male voice", ERROR_TYPE_INVALID_REQUEST);
        return res;
    }

    task_params params = task_params_from_json(body, params_, eng_.n_ctx());
    params.want_text = false;
    params.want_audio = true;
    params.audio_voice = voice;
    params.audio_lang = lang;
    params.audio_temperature = json_value(body, "temperature", json_value(body, "audio_temperature", -1.0f));
    params.audio_top_k = json_value(body, "top_k", json_value(body, "audio_top_k", -1));
    params.audio_repetition_penalty = json_value(body, "repeat_penalty", json_value(body, "audio_repetition_penalty", -1.0f));
    if (body.contains("n_predict") && json_value(body, "n_predict", -1) > 0) {
        // llama-server counts speech frames (80 ms) in n_predict
        params.max_audio_seconds = std::max(0.08f, 0.08f * json_value(body, "n_predict", 0));
    } else if (body.contains("max_audio_seconds")) {
        params.max_audio_seconds = json_value(body, "max_audio_seconds", 30.0f);
    }
    params.sampling.n_probs = 0;

    std::vector<zx::Message> msgs;
    msgs.push_back(zx::Message::System(lfm3_tts_system_prompt(lang, voice)));
    msgs.push_back(zx::Message::User(input));

    const int sample_rate = eng_.caps().speech_sample_rate > 0 ? eng_.caps().speech_sample_rate : 24000;
    const std::string pcm_type = "audio/pcm;rate=" + std::to_string(sample_rate);
    const std::string content_type = pcm16 ? pcm_type + ";encoding=signed-int;bits=16"
        : response_format == "pcm" ? pcm_type + ";encoding=float;bits=32"
        : "audio/wav";
    const std::string fmt = pcm16 ? "pcm16" : response_format == "pcm" ? "pcm32" : "wav";
    const int32_t n_ctx = eng_.n_ctx();

    auto channel = std::make_shared<task_channel>();
    const int task_id = eng_.post([=, msgs = std::move(msgs)](engine & eng, engine::task & task) mutable {
        zx::PromptInfo info;
        try {
            info = eng.llm().inspect(msgs, params.to_sdk(1));
        } catch (const zx::Error & e) {
            throw status_to_api_error(e.status(), e.what());
        }
        const int32_t audio_reserve = (int32_t) std::ceil(params.max_audio_seconds * 12.5) + 1;
        if (n_ctx - info.rows - audio_reserve <= 0) {
            channel->set_error(api_error(ERROR_TYPE_EXCEED_CONTEXT_SIZE,
                string_format("the request needs %d prompt rows + %d speech rows, the context holds %d", info.rows, audio_reserve, n_ctx),
                json {{"n_prompt_tokens", info.rows}, {"n_ctx", n_ctx}}));
            return;
        }
        zx::GenerationConfig cfg = params.to_sdk(std::max(1, n_ctx - info.rows - audio_reserve));
        eng.slot_begin(task, info.rows);
        if (stream) {
            channel->push("");  // headers out, see handle_completions_impl
        }
        bool header_sent = false;
        std::vector<float> collected;
        int32_t frames = 0;
        const int64_t t_start = time_us();

        auto on_event = [&](const zx::StreamEvent & ev) -> bool {
            if (task.cancelled.load() || channel->consumer_gone()) return false;
            if (ev.type != zx::StreamEvent::Type::Audio || !ev.audio) return true;
            frames++;
            if (stream) {
                std::string bytes;
                if (fmt == "wav" && !header_sent) {
                    bytes = wav_header((uint32_t) ev.audio->sample_rate, 1, 16, 0xFFFFFFFFu);
                    header_sent = true;
                }
                bytes += audio_bytes(ev.audio->pcm, ev.audio->samples, fmt == "wav" ? "pcm16" : fmt);
                return channel->push(bytes);
            }
            return true;
        };

        zx::Response resp = eng.llm().chat(msgs, cfg, on_event);
        eng.slot_end(task);
        if (resp.status != zx::Status::Ok) {
            const api_error err = status_to_api_error(resp.status, resp.error, info.rows, n_ctx);
            SRV_WRN("task %d speech failed: %s\n", task.id, err.what());
            if (stream) {
                channel->close();  // the headers are out: the stream just ends
            } else {
                channel->set_error(err);
            }
            return;
        }
        task.stats.n_prompt_tokens = resp.usage.prompt_rows;
        task.stats.n_prompt_tokens_cache = resp.usage.cached_rows;
        task.stats.n_decoded = resp.usage.completion_tokens;
        task.stats.n_decode_steps = resp.timings.decode_steps;
        task.stats.t_prompt_us = (int64_t) (resp.timings.ttft * 1000.0);
        task.stats.t_gen_us = (int64_t) (resp.timings.decode * 1000.0);
        task.stats.n_audio_frames = resp.usage.audio_frames;
        eng.record_stats(task.stats);
        SRV_INF("task %d speech done: %.2f s of audio in %.0f ms (rtf %.2fx)\n", task.id, resp.timings.audio_seconds, (time_us() - t_start) / 1000.0, resp.timings.realtime_factor);

        if (stream) {
            if (fmt == "wav" && !header_sent) {
                channel->push(wav_header((uint32_t) sample_rate, 1, 16, 0xFFFFFFFFu));
            }
            channel->close();
            return;
        }
        std::string bytes;
        if (fmt == "wav") {
            bytes = encode_wav_s16(resp.audio.data(), resp.audio.size(), (uint32_t) sample_rate);
        } else {
            bytes = audio_bytes(resp.audio.data(), resp.audio.size(), fmt);
        }
        channel->set_final(json {{"bytes", std::move(bytes)}});
    }, channel, params.to_json(true), input);

    const auto state = channel->wait_started(req.should_stop);
    if (channel->consumer_gone() && state == task_channel::state::pending) {
        eng_.cancel_task(task_id);
        return res;
    }
    if (state == task_channel::state::error) {
        res->error(channel->error());
        return res;
    }
    res->status = 200;
    res->content_type = content_type;
    if (!stream) {
        if (state != task_channel::state::final) {
            res->error("the request was cancelled", ERROR_TYPE_SERVER);
            return res;
        }
        res->data = channel->final().at("bytes").get<std::string>();
        return res;
    }
    res->next = [channel](std::string & output) -> bool {
        while (true) {
            if (channel->next_chunk(output, 250)) return true;
            if (channel->closed_and_drained() || channel->consumer_gone()) {
                output.clear();
                return false;
            }
        }
    };
    res->on_complete = [channel, task_id, this]() {
        if (!channel->closed_and_drained()) {
            channel->mark_consumer_gone();
            eng_.cancel_task(task_id);
        }
    };
    return res;
}

http_res_ptr server_routes::post_tts(const http_req & req) {
    return handle_tts_impl(req, fix_form_types(parse_body(req)));
}

http_res_ptr server_routes::post_speech_oai(const http_req & req) {
    const json body = convert_speech_to_tts(fix_form_types(parse_body(req)));
    return handle_tts_impl(req, body);
}

//
// decisions
//

http_res_ptr server_routes::post_systemone(const http_req & req) {
    auto res = make_res();
    if (!eng_.is_decision()) {
        res->error("This model is not a decision model", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    const json body = parse_body(req);
    decision_request dreq = parse_decision_request(body, media_opts());
    if (!dreq.media.empty() && !eng_.caps().image && !eng_.caps().audio) {
        res->error("This server does not support image input for decisions. For a model that supports it, load a d1-2B or d1-Omni-2B package", ERROR_TYPE_NOT_SUPPORTED);
        return res;
    }
    const std::string model_name = eng_.model_name();

    auto channel = std::make_shared<task_channel>();
    json slot_params = {{"n_questions", dreq.questions.size()}, {"n_media", dreq.media.size()}};
    const int task_id = eng_.post([=, dreq = std::move(dreq)](engine & eng, engine::task & task) mutable {
        eng.slot_begin(task, 0);
        const int64_t t0 = time_us();
        zx::DecisionResult out = eng.decision().systemOne(dreq.state, dreq.questions, dreq.media);
        eng.slot_end(task);
        if (out.status != zx::Status::Ok) {
            channel->set_error(status_to_api_error(out.status, out.error, out.input_tokens, 2048));
            return;
        }
        json answers = json::object();
        for (size_t i = 0; i < out.answers.size(); i++) {
            const auto & [name, a] = out.answers[i];
            const decision_question_meta * meta = nullptr;
            for (const auto & m : dreq.meta) {
                if (m.id == name) meta = &m;
            }
            if (!meta && i < dreq.meta.size()) meta = &dreq.meta[i];
            if (!meta) continue;
            answers[name] = format_decision_answer(*meta, a);
        }
        task.stats.n_prompt_tokens = out.input_tokens;
        task.stats.t_prompt_us = (int64_t) ((out.timings.prefill + out.timings.encode) * 1000.0);
        task.stats.n_decisions = (int32_t) out.answers.size();
        eng.record_stats(task.stats);
        SRV_INF("task %d decision done: %zu answers, %d input tokens, %d pass(es), %.1f ms\n", task.id, out.answers.size(), out.input_tokens, out.passes, (time_us() - t0) / 1000.0);
        channel->set_final(json {
            {"model",   model_name},
            {"answers", answers},
            {"usage",   json {{"input_tokens", out.input_tokens}, {"output_tokens", 0}}},
            {"timings", json {
                {"passes", out.passes},
                {"encode_ms", out.timings.encode},
                {"prefill_ms", out.timings.prefill},
                {"total_ms", out.timings.ttft},
            }},
        });
    }, channel, slot_params, "");

    const auto state = channel->wait_started(req.should_stop);
    if (channel->consumer_gone() && state == task_channel::state::pending) {
        eng_.cancel_task(task_id);
        return res;
    }
    if (state == task_channel::state::error) {
        res->error(channel->error());
        return res;
    }
    if (state != task_channel::state::final) {
        res->error("the request was cancelled", ERROR_TYPE_SERVER);
        return res;
    }
    res->ok(channel->final());
    return res;
}

}  // namespace ixsrv
