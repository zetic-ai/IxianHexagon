#include "server-utils.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>

#ifndef IXSRV_VERSION
#define IXSRV_VERSION "0.0.0"
#endif
#ifndef IXSRV_BUILD_NUMBER
#define IXSRV_BUILD_NUMBER 0
#endif
#ifndef IXSRV_BUILD_COMMIT
#define IXSRV_BUILD_COMMIT "unknown"
#endif
#ifndef IXSRV_SDK_VERSION
#define IXSRV_SDK_VERSION "unknown"
#endif

namespace ixsrv {

//
// errors
//

int error_type_http_status(error_type type) {
    switch (type) {
        case ERROR_TYPE_INVALID_REQUEST:     return 400;
        case ERROR_TYPE_AUTHENTICATION:      return 401;
        case ERROR_TYPE_NOT_FOUND:           return 404;
        case ERROR_TYPE_SERVER:              return 500;
        case ERROR_TYPE_PERMISSION:          return 403;
        case ERROR_TYPE_NOT_SUPPORTED:       return 501;
        case ERROR_TYPE_UNAVAILABLE:         return 503;
        case ERROR_TYPE_EXCEED_CONTEXT_SIZE: return 400;
    }
    return 500;
}

json format_error_response(const std::string & message, error_type type) {
    std::string type_str;
    switch (type) {
        case ERROR_TYPE_INVALID_REQUEST:     type_str = "invalid_request_error"; break;
        case ERROR_TYPE_AUTHENTICATION:      type_str = "authentication_error"; break;
        case ERROR_TYPE_NOT_FOUND:           type_str = "not_found_error"; break;
        case ERROR_TYPE_SERVER:              type_str = "server_error"; break;
        case ERROR_TYPE_PERMISSION:          type_str = "permission_error"; break;
        case ERROR_TYPE_NOT_SUPPORTED:       type_str = "not_supported_error"; break;
        case ERROR_TYPE_UNAVAILABLE:         type_str = "unavailable_error"; break;
        case ERROR_TYPE_EXCEED_CONTEXT_SIZE: type_str = "exceed_context_size_error"; break;
    }
    return json {
        {"code",    error_type_http_status(type)},
        {"message", message},
        {"type",    type_str},
    };
}

json api_error::to_json() const {
    json res = format_error_response(message, type);
    if (extra.is_object()) {
        for (const auto & [k, v] : extra.items()) {
            res[k] = v;
        }
    }
    return res;
}

//
// ids
//

std::string random_string() {
    static const std::string chars("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    thread_local std::mt19937_64 generator{std::random_device{}()};
    std::string result(32, ' ');
    for (int i = 0; i < 32; ++i) {
        result[i] = chars[generator() % chars.size()];
    }
    return result;
}

std::string gen_chatcmplid() {
    return "chatcmpl-" + random_string();
}

std::string gen_tool_call_id() {
    return random_string();
}

std::string gen_resp_id(const char * prefix) {
    return std::string(prefix) + random_string();
}

const char * get_media_marker() {
    static const std::string marker = []() {
        const char * env = getenv("LLAMA_MEDIA_MARKER");
        if (env && env[0] != '\0') {
            return std::string(env);
        }
        return std::string("<__media_") + random_string() + "__>";
    }();
    return marker.c_str();
}

//
// strings
//

std::string string_format(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int size = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string buf;
    if (size > 0) {
        buf.resize((size_t) size);
        vsnprintf(&buf[0], (size_t) size + 1, fmt, ap2);
    }
    va_end(ap2);
    return buf;
}

std::vector<std::string> string_split(const std::string & s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

std::string string_strip(const std::string & s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isspace((unsigned char) s[b])) b++;
    while (e > b && isspace((unsigned char) s[e - 1])) e--;
    return s.substr(b, e - b);
}

std::string string_lower(std::string s) {
    for (auto & c : s) {
        c = (char) tolower((unsigned char) c);
    }
    return s;
}

bool string_starts_with(const std::string & s, const std::string & prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool string_ends_with(const std::string & s, const std::string & suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void string_replace_all(std::string & s, const std::string & search, const std::string & replace) {
    if (search.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = s.find(search, pos)) != std::string::npos) {
        s.replace(pos, search.size(), replace);
        pos += replace.size();
    }
}

std::string string_join(const std::vector<std::string> & parts, const std::string & sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

size_t validate_utf8(const std::string & text) {
    size_t len = text.size();
    if (len == 0) return 0;
    for (size_t i = 1; i <= 4 && i <= len; ++i) {
        unsigned char c = text[len - i];
        if ((c & 0xE0) == 0xC0) {
            if (i < 2) return len - i;
        } else if ((c & 0xF0) == 0xE0) {
            if (i < 3) return len - i;
        } else if ((c & 0xF8) == 0xF0) {
            if (i < 4) return len - i;
        }
    }
    return len;
}

bool is_valid_utf8(const std::string & str) {
    const unsigned char * bytes = (const unsigned char *) str.data();
    const unsigned char * end   = bytes + str.length();
    while (bytes < end) {
        if (*bytes <= 0x7F) {
            bytes++;
        } else if ((*bytes & 0xE0) == 0xC0) {
            if (end - bytes < 2 || (bytes[1] & 0xC0) != 0x80) return false;
            bytes += 2;
        } else if ((*bytes & 0xF0) == 0xE0) {
            if (end - bytes < 3 || (bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80) return false;
            bytes += 3;
        } else if ((*bytes & 0xF8) == 0xF0) {
            if (end - bytes < 4 || (bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (bytes[3] & 0xC0) != 0x80) return false;
            bytes += 4;
        } else {
            return false;
        }
    }
    return true;
}

json str_to_bytes_json(const std::string & str) {
    json bytes = json::array();
    for (unsigned char c : str) {
        bytes.push_back((int) c);
    }
    return bytes;
}

//
// JSON
//

std::string safe_json_to_str(const json & data) {
    return data.dump(-1, ' ', false, json::error_handler_t::replace);
}

json json_get_nested_values(const std::vector<std::string> & paths, const json & js) {
    json result = json::object();
    for (const std::string & path : paths) {
        json current = js;
        const auto keys = string_split(path, '/');
        bool valid_path = true;
        for (const std::string & k : keys) {
            if (valid_path && current.is_object() && current.contains(k)) {
                current = current[k];
            } else {
                valid_path = false;
            }
        }
        if (valid_path) {
            result[path] = current;
        }
    }
    return result;
}

bool json_is_array_of_numbers(const json & data) {
    if (!data.is_array()) return false;
    for (const auto & e : data) {
        if (!e.is_number_integer()) return false;
    }
    return true;
}

bool json_is_array_of_mixed_numbers_strings(const json & data) {
    bool seen_string = false;
    bool seen_number = false;
    if (data.is_array()) {
        for (const auto & e : data) {
            seen_string |= e.is_string();
            seen_number |= e.is_number_integer();
            if (seen_number && seen_string) return true;
        }
    }
    return false;
}

bool json_is_array_and_contains_numbers(const json & data) {
    if (!data.is_array()) return false;
    for (const auto & e : data) {
        if (e.is_number_integer()) return true;
    }
    return false;
}

//
// SSE
//

std::string format_oai_sse(const json & data) {
    std::ostringstream ss;
    auto send_single = [&ss](const json & d) {
        ss << "data: " << safe_json_to_str(d) << "\n\n";
    };
    if (data.is_array()) {
        for (const auto & item : data) send_single(item);
    } else {
        send_single(data);
    }
    return ss.str();
}

std::string format_oai_resp_sse(const json & data) {
    std::ostringstream ss;
    auto send_single = [&ss](const json & event_obj) {
        ss << "event: " << event_obj.at("event").get<std::string>() << "\n";
        ss << "data: " << safe_json_to_str(event_obj.at("data")) << "\n\n";
    };
    if (data.is_array()) {
        for (const auto & item : data) send_single(item);
    } else {
        send_single(data);
    }
    return ss.str();
}

std::string format_anthropic_sse(const json & data) {
    std::ostringstream ss;
    auto send_event = [&ss](const json & event_obj) {
        if (event_obj.is_object() && event_obj.contains("event") && event_obj.contains("data")) {
            ss << "event: " << event_obj.at("event").get<std::string>() << "\n";
            ss << "data: " << safe_json_to_str(event_obj.at("data")) << "\n\n";
        } else {
            ss << "data: " << safe_json_to_str(event_obj) << "\n\n";
        }
    };
    if (data.is_array()) {
        for (const auto & event : data) send_event(event);
    } else {
        send_event(data);
    }
    return ss.str();
}

//
// time
//

int64_t time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t time_ms() {
    return time_us() / 1000;
}

double time_now_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

//
// build
//

const char * build_info() {
    static const std::string info = string_format("b%d-%s", (int) IXSRV_BUILD_NUMBER, IXSRV_BUILD_COMMIT);
    return info.c_str();
}

const char * server_version() {
    return IXSRV_VERSION;
}

const char * sdk_version() {
    return IXSRV_SDK_VERSION;
}

}  // namespace ixsrv
