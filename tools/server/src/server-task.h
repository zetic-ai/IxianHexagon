// Ixian Hexagon server - one generation request: its parameters (parsed from the JSON body with llama-server's names
// and defaults) and the JSON of its results in every supported shape (native /completion, OpenAI chat / completions /
// responses / transcriptions, Anthropic messages), for the final answer and for streamed deltas.
#pragma once

#include "server-args.h"
#include "server-chat.h"
#include "server-engine.h"
#include "server-utils.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace ixsrv {

enum task_response_type {
    TASK_RESPONSE_TYPE_NONE,       // llama.cpp native format (/completion)
    TASK_RESPONSE_TYPE_OAI_CHAT,
    TASK_RESPONSE_TYPE_OAI_CMPL,
    TASK_RESPONSE_TYPE_OAI_RESP,
    TASK_RESPONSE_TYPE_OAI_ASR,
    TASK_RESPONSE_TYPE_ANTHROPIC,
};

enum stop_type {
    STOP_TYPE_NONE,
    STOP_TYPE_EOS,
    STOP_TYPE_WORD,
    STOP_TYPE_LIMIT,
};
const char * stop_type_to_str(stop_type t);

struct task_params {
    bool stream             = false;
    bool include_usage      = false;
    bool cache_prompt       = true;
    bool return_tokens      = false;
    bool return_progress    = false;
    bool timings_per_token  = false;
    bool post_sampling_probs = false;
    bool verbose            = false;

    int32_t sse_ping_interval = 30;
    int32_t n_predict  = -1;   // -1 = as many as fit
    int32_t n_probs    = 0;
    int32_t n_cmpl     = 1;    // "n": completions per prompt (run one after another)
    int32_t n_keep     = 0;
    int32_t n_discard  = 0;
    int64_t t_max_predict_ms = -1;

    std::vector<std::string> antiprompt;       // "stop"
    std::vector<std::string> response_fields;

    struct {
        float   temp           = -1;
        float   top_p          = -1;
        float   min_p          = -1;
        int32_t top_k          = -1;
        float   penalty_repeat = -1;
        int32_t penalty_last_n = 64;
        int64_t seed           = -1;  // -1 random
        bool    ignore_eos     = false;
        int32_t n_probs        = 0;
    } sampling;

    // chat
    bool             enable_thinking = false;
    reasoning_format reasoning_fmt   = reasoning_format::deepseek;

    // speech output (modalities / audio)
    bool        want_text  = true;
    bool        want_audio = false;
    std::string audio_voice;
    std::string audio_format = "wav";      // wav | pcm16 | pcm (float32)
    std::string audio_lang;
    float       max_audio_seconds = 30;
    int32_t     audio_first_chunk_frames = -1;
    int32_t     audio_chunk_frames = -1;
    float       audio_temperature = -1;
    int32_t     audio_top_k = -1;
    float       audio_repetition_penalty = -1;

    // response formatting
    task_response_type res_type = TASK_RESPONSE_TYPE_NONE;
    std::string oaicompat_model;
    std::string oaicompat_cmpl_id;
    std::string transcription_format = "json";

    // "generation_settings" of the native format and GET /slots (the same keys as llama-server)
    json to_json(bool only_metrics = false) const;
    // the SDK settings; max_tokens is the value resolved against the context
    zx::GenerationConfig to_sdk(int32_t max_tokens) const;
};

// parses the body (after the OpenAI -> native key mapping done by the caller); throws api_error
task_params task_params_from_json(const json & data, const server_params & defaults, int32_t n_ctx);

//
// results
//

struct token_prob {
    int32_t tok = -1;
    float   logprob = 0;      // natural log probability
    std::string text;
    std::vector<token_prob> top;  // top alternatives (the token itself first), without `top`
    json to_json(bool post_sampling_probs) const;
    static json vector_to_json(const std::vector<token_prob> & probs, bool post_sampling_probs);
};

// the timings object of llama-server (server_slot_stats::to_json), built from the SDK's Timings / Usage
struct task_timings {
    int32_t cache_n     = 0;
    int32_t prompt_n    = 0;
    double  prompt_ms   = 0;
    int32_t predicted_n = 0;
    double  predicted_ms = 0;
    int32_t predicted_steps = 0;   // decode steps (predicted_n - 1 when > 0)
    double  predicted_per_second_sdk = 0;  // the SDK's tokens_per_second (callbacks excluded)
    // ixian extras
    double  ttft_ms = 0, encode_ms = 0, audio_seconds = 0, realtime_factor = 0;
    int32_t audio_frames = 0;
    bool    is_set = false;
    json to_json() const;
};

// one completion (one choice), accumulated while the SDK streams, then formatted
struct completion_result {
    size_t      index = 0;
    task_params params;

    std::string content;             // the answer so far (tool-call blocks removed when tools are active)
    std::string reasoning;           // the thinking so far
    std::vector<int32_t> tokens;
    std::vector<tool_call_out> tool_calls;
    std::vector<token_prob> probs;   // all tokens (final) or the delta's (partial)

    int32_t n_decoded = 0;
    int32_t n_prompt_tokens = 0;
    int32_t n_prompt_tokens_cache = 0;
    bool    truncated = false;
    bool    has_new_line = false;
    std::string stopping_word;
    stop_type stop = STOP_TYPE_NONE;
    std::string prompt;              // the rendered prompt (verbose / native format)
    task_timings timings;

    // speech
    std::vector<float> audio;        // 24 kHz mono (non-stream)
    int32_t audio_sample_rate = 24000;
    std::string audio_id;

    // streaming state (Anthropic / Responses block indices, OpenAI "first chunk" role)
    bool thinking_block_started = false;
    bool text_block_started = false;
    std::unordered_set<size_t> tool_calls_started;
    bool oai_resp_created = false;
    std::string oai_resp_id, oai_resp_reasoning_id, oai_resp_message_id, oai_resp_fc_id;
    bool legacy_think_open = false;  // reasoning_format none / deepseek-legacy: "<think>" was written to content

    completion_result();

    json usage_json_oaicompat() const;
    json message_json_oaicompat(bool concat_typed_text = true) const;  // the assistant message
    std::string finish_reason_oaicompat() const;                       // "stop" | "length" | "tool_calls"

    // final result (non-stream), or the trailing events of a stream (an array)
    json to_json_final() const;
    json to_json_non_oaicompat() const;
    json to_json_oaicompat_cmpl() const;
    json to_json_oaicompat_chat() const;
    json to_json_oaicompat_chat_stream() const;
    json to_json_oaicompat_resp() const;
    json to_json_oaicompat_resp_stream() const;
    json to_json_oaicompat_asr() const;
    json to_json_anthropic() const;
    json to_json_anthropic_stream() const;
};

// what one streamed step adds
struct completion_delta {
    std::string content;
    std::string reasoning;
    std::vector<std::pair<size_t, tool_call_out>> tool_calls;  // (index, call) completed in this step
    std::vector<token_prob> probs;
    std::string audio_bytes;     // speech: the chunk, in the request's audio_format
    bool first = false;          // the first step of the reply (OpenAI role chunk)
    bool is_progress = false;    // a prompt-progress update
    json progress;               // {total, cache, processed, time_ms}
    bool with_timings = false;   // attach timings (timings_per_token)
};

// the JSON (object or array of events) of one streamed step; updates the streaming state in `r`
json to_json_partial(completion_result & r, const completion_delta & d);

// the SSE text of a result in the request's format (object or array)
std::string format_sse(task_response_type type, const json & data);
// the error event of a stream
std::string format_sse_error(task_response_type type, const json & error_obj);

}  // namespace ixsrv
