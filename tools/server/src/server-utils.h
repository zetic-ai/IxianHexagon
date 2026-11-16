// Ixian Hexagon server - shared helpers: JSON, API errors, ids, UTF-8, SSE, strings, time.
// The response shapes follow llama.cpp's tools/server so existing clients work unchanged.
#pragma once

#include <json.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace ixsrv {

// ordered_json keeps the key order of the literals, like llama.cpp's common_json
using json = nlohmann::ordered_json;

// read body[key] as T, the default when absent, null or of the wrong type (a warning is logged in that case)
template <typename T>
T json_value(const json & body, const std::string & key, const T & default_value) {
    if (body.is_object() && body.contains(key) && !body.at(key).is_null()) {
        try {
            return body.at(key).get<T>();
        } catch (const nlohmann::json::exception &) {
            return default_value;
        }
    }
    return default_value;
}

// https://community.openai.com/t/openai-chat-list-of-error-codes-and-types/357791/11
enum error_type {
    ERROR_TYPE_INVALID_REQUEST,
    ERROR_TYPE_AUTHENTICATION,
    ERROR_TYPE_SERVER,
    ERROR_TYPE_NOT_FOUND,
    ERROR_TYPE_PERMISSION,
    ERROR_TYPE_UNAVAILABLE,          // custom error (503)
    ERROR_TYPE_NOT_SUPPORTED,        // custom error (501)
    ERROR_TYPE_EXCEED_CONTEXT_SIZE,  // custom error (400)
};

// {"code": 400, "message": "...", "type": "invalid_request_error"}
json format_error_response(const std::string & message, error_type type);
int  error_type_http_status(error_type type);

// thrown by request handlers; the HTTP layer turns it into {"error": ...} with the right status
struct api_error : std::exception {
    error_type  type;
    std::string message;
    json        extra;  // merged into the error object (e.g. n_prompt_tokens / n_ctx)

    api_error(error_type type, std::string message, json extra = json::object())
        : type(type), message(std::move(message)), extra(std::move(extra)) {}

    const char * what() const noexcept override { return message.c_str(); }

    json to_json() const;
};

//
// ids
//

std::string random_string();     // 32 alphanumeric characters
std::string gen_chatcmplid();    // "chatcmpl-" + random_string()
std::string gen_tool_call_id();  // random_string()
std::string gen_resp_id(const char * prefix);  // prefix + random_string()

// the text that marks where a media file goes in a /completion prompt; LLAMA_MEDIA_MARKER pins it
const char * get_media_marker();

//
// strings
//

std::string string_format(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> string_split(const std::string & s, char sep);
std::string string_strip(const std::string & s);
std::string string_lower(std::string s);
bool string_starts_with(const std::string & s, const std::string & prefix);
bool string_ends_with(const std::string & s, const std::string & suffix);
void string_replace_all(std::string & s, const std::string & search, const std::string & replace);
std::string string_join(const std::vector<std::string> & parts, const std::string & sep);

// index of the last byte that ends a complete UTF-8 sequence (a cut multibyte tail is excluded)
size_t validate_utf8(const std::string & text);
bool   is_valid_utf8(const std::string & str);
// bytes of a string as a JSON array of integers (OpenAI logprobs "bytes")
json str_to_bytes_json(const std::string & str);

//
// JSON
//

std::string safe_json_to_str(const json & data);  // invalid UTF-8 is replaced, never throws
json json_get_nested_values(const std::vector<std::string> & paths, const json & js);  // "a/b" paths
bool json_is_array_of_numbers(const json & data);
bool json_is_array_of_mixed_numbers_strings(const json & data);
bool json_is_array_and_contains_numbers(const json & data);

//
// server-sent events
//

// "data: {...}\n\n" per item; an array is one event per item
std::string format_oai_sse(const json & data);
// "event: <name>\ndata: {...}\n\n" from {"event": ..., "data": ...}
std::string format_oai_resp_sse(const json & data);
// Anthropic events ({"event","data"}), or plain data events
std::string format_anthropic_sse(const json & data);

//
// time
//

int64_t time_us();
int64_t time_ms();
double  time_now_s();

//
// build
//

const char * build_info();     // "b<number>-<commit>", the system_fingerprint of the responses
const char * server_version(); // IXSRV_VERSION
const char * sdk_version();    // the SDK headers the server was built with

}  // namespace ixsrv
