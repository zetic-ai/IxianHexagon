// Ixian Hexagon server - the HTTP routes (the same paths and payloads as llama-server).
#pragma once

#include "server-args.h"
#include "server-chat.h"
#include "server-engine.h"
#include "server-http.h"
#include "server-task.h"

namespace ixsrv {

class server_routes {
public:
    server_routes(engine & eng, const server_params & params);

    // registers every route on the server
    void register_routes(http_server & http);

    // the handlers
    http_res_ptr get_health(const http_req & req);
    http_res_ptr get_metrics(const http_req & req);
    http_res_ptr get_slots(const http_req & req);
    http_res_ptr post_slots(const http_req & req);
    http_res_ptr get_props(const http_req & req);
    http_res_ptr post_props(const http_req & req);
    http_res_ptr get_models(const http_req & req);
    http_res_ptr post_completions(const http_req & req);        // /completion, /completions (native)
    http_res_ptr post_completions_oai(const http_req & req);    // /v1/completions
    http_res_ptr post_chat_completions(const http_req & req);   // /v1/chat/completions
    http_res_ptr post_chat_completions_tok(const http_req & req);
    http_res_ptr post_responses_oai(const http_req & req);      // /v1/responses
    http_res_ptr post_responses_tok_oai(const http_req & req);
    http_res_ptr post_transcriptions_oai(const http_req & req); // /v1/audio/transcriptions
    http_res_ptr post_speech_oai(const http_req & req);         // /v1/audio/speech
    http_res_ptr post_tts(const http_req & req);                // /tts
    http_res_ptr post_anthropic_messages(const http_req & req); // /v1/messages
    http_res_ptr post_anthropic_count_tokens(const http_req & req);
    http_res_ptr post_apply_template(const http_req & req);
    http_res_ptr post_tokenize(const http_req & req);
    http_res_ptr post_detokenize(const http_req & req);
    http_res_ptr post_systemone(const http_req & req);          // /v1/systemone (d1)
    http_res_ptr post_control(const http_req & req);
    http_res_ptr get_lora_adapters(const http_req & req);
    http_res_ptr not_supported(const http_req & req, const std::string & what);

    json props_json() const;
    json models_json() const;

private:
    // the chat template options of this server (defaults from the command line)
    chat_convert_options convert_options() const;
    media_options media_opts() const;

    // runs one or more generations on the inference thread and answers (streamed or not)
    http_res_ptr handle_completions_impl(const http_req & req, json body,
                                         std::vector<std::vector<zx::Message>> prompts, std::vector<std::string> prompt_texts,
                                         task_response_type res_type, const chat_inputs * chat);
    http_res_ptr handle_tts_impl(const http_req & req, json body);
    http_res_ptr handle_count_tokens(const http_req & req, task_response_type res_type);
    http_res_ptr handle_slots_erase(const http_req & req);

    // /completion prompts: string, tokens, mixed, {prompt_string, multimodal_data}, or an array of those
    void prompts_from_json(const json & prompt, std::vector<std::vector<zx::Message>> & prompts, std::vector<std::string> & texts);

    void log_prompt(const std::string & prompt) const;

    engine & eng_;
    const server_params & params_;
};

}  // namespace ixsrv
