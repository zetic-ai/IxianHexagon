// Ixian Hexagon server - chat conversions: OpenAI / Anthropic / Responses request bodies -> SDK messages, media
// loading, the LFM3 tool-call text format (rendering and parsing), and the speech / ASR prompts.
//
// The SDK renders the LFM3 chat template itself from Message{role, parts}; it has no notion of tools or of the
// "tool" role. This file renders what the LFM3 template would have rendered for those (the <tools> block in the
// system prompt, <tool_call> blocks in assistant turns, <tool_response> blocks for tool results) into plain text
// parts, and parses the <tool_call> blocks the model writes back into OpenAI tool_calls.
#pragma once

#include "server-engine.h"
#include "server-http.h"
#include "server-media.h"
#include "server-utils.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ixsrv {

//
// reasoning
//
enum class reasoning_format {
    none,             // thinking stays in message.content as <think>...</think>
    deepseek,         // thinking goes to message.reasoning_content (default)
    deepseek_legacy,  // both
};
reasoning_format reasoning_format_from_name(const std::string & name);  // "auto" = deepseek
const char *     reasoning_format_name(reasoning_format fmt);

//
// tools
//
struct chat_tool {
    std::string name;
    std::string description;
    json        parameters;  // JSON schema object
    bool        defer_loading = false;
};

enum class tool_choice_kind { auto_, none, required, named };

struct tool_choice {
    tool_choice_kind kind = tool_choice_kind::auto_;
    std::string name;  // named
};

std::vector<chat_tool> parse_tools(const json & tools);          // throws std::invalid_argument
tool_choice            parse_tool_choice(const json & tc);       // "auto" | "none" | "required" | {"type":"function","function":{"name":..}}

struct tool_call_out {
    std::string id;
    std::string name;
    std::string arguments;  // JSON text
};

// the `tojson` filter as Hugging Face transformers renders a chat template (", " / ": " separators, ensure_ascii=False)
std::string jinja_tojson(const json & v);

// the LFM3 text forms
std::string lfm3_render_tools(const std::vector<chat_tool> & tools);            // "<tools>\n{...}\n</tools>"
std::string lfm3_render_tool_calls(const json & oai_tool_calls);                // "<tool_call>\n<function=...>..."
std::string lfm3_render_tool_response(const json & content);                    // "<tool_response>\n...\n</tool_response>"

// parses every <tool_call>...</tool_call> block out of a complete reply; the rest is `content`
void lfm3_parse_tool_calls(const std::string & text, const std::vector<chat_tool> & tools,
                           std::string & content, std::vector<tool_call_out> & calls);

// incremental version for streaming: pieces in, content deltas and completed tool calls out
class lfm3_tool_parser {
public:
    lfm3_tool_parser() = default;
    lfm3_tool_parser(bool enabled, std::vector<chat_tool> tools);

    // feeds one piece; `content_delta` receives the text that can be shown now (the parser holds back a possible
    // start of a tag), `completed` the tool calls that ended in this piece
    void feed(const std::string & piece, std::string & content_delta, std::vector<tool_call_out> & completed);
    // end of the reply: releases the held text (an unterminated block becomes content)
    void finish(std::string & content_delta, std::vector<tool_call_out> & completed);

    const std::vector<tool_call_out> & tool_calls() const { return calls_; }
    const std::string & content() const { return content_; }
    bool enabled() const { return enabled_; }

private:
    bool enabled_ = false;
    std::vector<chat_tool> tools_;
    std::string pending_;   // text not yet released
    std::string content_;   // released content so far
    std::vector<tool_call_out> calls_;
    bool in_call_ = false;
};

//
// media
//
struct media_options {
    bool allow_image = false;
    bool allow_audio = false;
    bool allow_video = false;
    std::string media_path;             // file:// root ("" = disabled)
    bool allow_private_urls = false;    // http(s) to private addresses
    ImageFit image_fit = ImageFit::Letterbox;
    double video_fps = 2.0;
    std::string ffmpeg_dir;
    int image_size = 512;               // the model's input size (the SDK takes exactly 512 x 512)
    double audio_max_seconds = 30.0;    // one audio part holds up to 30 s; longer clips are split
};

// fetches the bytes behind a media reference: data URL, raw base64, http(s) URL (public addresses only unless
// allowed), file:// (under media_path only). Throws std::invalid_argument / api_error.
raw_buffer load_media_url(const std::string & url, const media_options & opt, std::string * mime_out = nullptr);

// turns media bytes into SDK parts: an image becomes one Part::Img at image_size (resized per image_fit), audio
// one or more Part::Aud (mono, split at audio_max_seconds), a video one Part::Vid (frames decoded with ffmpeg at
// video_fps, letterboxed). `kind_hint` is "image" / "audio" / "video" / "" (sniffed).
std::vector<zx::Part> media_to_parts(const raw_buffer & bytes, const std::string & kind_hint, const media_options & opt);

// a video given as frames (ixian extension: {"type": "video_frames", "frames": [image urls], "fps": 2})
zx::Part video_frames_to_part(const std::vector<raw_buffer> & frames, double fps, const media_options & opt);

//
// chat request -> SDK messages
//
struct chat_convert_options {
    media_options media;
    bool enable_thinking = false;            // the server default (after --reasoning and --chat-template-kwargs)
    reasoning_format reasoning_fmt = reasoning_format::deepseek;
    std::map<std::string, std::string> template_kwargs;  // --chat-template-kwargs (key -> JSON text)
    bool prefill_assistant = true;           // llama.cpp semantics; prefill itself is not possible, see README
    engine * eng = nullptr;                  // for recall_reply (exact prefix cache reuse)
};

struct chat_inputs {
    std::vector<zx::Message> messages;  // what the SDK gets
    std::vector<chat_tool>   tools;
    tool_choice              choice;
    bool                     parallel_tool_calls = true;
    bool                     enable_thinking = false;
    reasoning_format         reasoning_fmt = reasoning_format::deepseek;
    json                     json_schema;       // response_format / json_schema (soft, rendered into the system prompt); null when none
    int                      n_media = 0;
    bool                     has_trailing_assistant = false;  // the last message was an assistant message (no prefill possible)
};

// the OpenAI chat-completions body (messages, tools, tool_choice, response_format, json_schema, chat_template_kwargs,
// reasoning_effort, parallel_tool_calls, add_generation_prompt, continue_final_message) -> SDK messages.
// Validation errors are std::invalid_argument (400). Unsupported requests (grammar, continue_final_message) are api_error 501.
chat_inputs oai_chat_to_sdk(const json & body, const chat_convert_options & opt);

// one OpenAI content value (string or array of parts) -> SDK parts, loading the media
std::vector<zx::Part> oai_content_to_parts(const json & content, const media_options & opt, bool allow_media, int * n_media = nullptr);

//
// speech and ASR prompts (LFM3-Audio / LFM3-Omni)
//
std::string lfm3_language_name(const std::string & code);                         // "ko" -> "Korean", unknown -> as is
std::string lfm3_asr_system_prompt(const std::string & language);                 // "Perform ASR." / "Perform ASR in Korean."
std::string lfm3_tts_system_prompt(const std::string & language, const std::string & voice);     // "Perform TTS in American English using a female voice."
std::string lfm3_speech_system_prompt(const std::string & language, const std::string & voice);  // "Speak in Korean using a male voice." or ""
bool        lfm3_is_voice(const std::string & voice);                             // female | male

//
// other API shapes -> OpenAI chat completions body
//
json convert_responses_to_chatcmpl(const json & body);
json convert_anthropic_to_oai(const json & body);
// the multipart transcription request: the uploaded file goes to out_files, the fields become a chat body
json convert_transcriptions_to_chatcmpl(const json & body, const std::map<std::string, uploaded_file> & in_files,
                                        std::vector<raw_buffer> & out_files, const std::string & asr_system_prompt);
json convert_speech_to_tts(const json & body);

}  // namespace ixsrv
