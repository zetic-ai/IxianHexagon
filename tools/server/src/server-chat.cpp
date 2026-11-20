#include "server-chat.h"

#include "server-log.h"

#include <cpp-httplib/httplib.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ixsrv {

//
// reasoning
//

reasoning_format reasoning_format_from_name(const std::string & name) {
    if (name == "none") return reasoning_format::none;
    if (name == "deepseek-legacy") return reasoning_format::deepseek_legacy;
    if (name == "deepseek" || name == "auto" || name.empty()) return reasoning_format::deepseek;
    throw std::invalid_argument("Unknown reasoning format: " + name);
}

const char * reasoning_format_name(reasoning_format fmt) {
    switch (fmt) {
        case reasoning_format::none:            return "none";
        case reasoning_format::deepseek:        return "deepseek";
        case reasoning_format::deepseek_legacy: return "deepseek-legacy";
    }
    return "none";
}

//
// tools
//

std::vector<chat_tool> parse_tools(const json & tools) {
    std::vector<chat_tool> result;
    if (tools.is_null()) {
        return result;
    }
    if (!tools.is_array()) {
        throw std::invalid_argument("Expected 'tools' to be an array, got " + tools.dump());
    }
    for (const auto & tool : tools) {
        if (!tool.is_object() || !tool.contains("type")) {
            throw std::invalid_argument("Missing tool type: " + tool.dump());
        }
        const auto & type = tool.at("type");
        if (!type.is_string() || type != "function") {
            throw std::invalid_argument("Unsupported tool type: " + tool.dump());
        }
        if (!tool.contains("function") || !tool.at("function").is_object()) {
            throw std::invalid_argument("Missing tool function: " + tool.dump());
        }
        const auto & fn = tool.at("function");
        if (!fn.contains("name") || !fn.at("name").is_string()) {
            throw std::invalid_argument("Missing tool function name: " + tool.dump());
        }
        chat_tool t;
        t.name = fn.at("name");
        t.description = json_value(fn, "description", std::string());
        t.parameters = fn.contains("parameters") && !fn.at("parameters").is_null() ? fn.at("parameters") : json::object();
        const json & defer = fn.contains("defer_loading") ? fn.at("defer_loading") : tool.value("defer_loading", json(false));
        if (!defer.is_boolean()) {
            throw std::invalid_argument("Expected 'defer_loading' to be a boolean, got " + defer.dump());
        }
        t.defer_loading = defer.get<bool>();
        result.push_back(std::move(t));
    }
    return result;
}

tool_choice parse_tool_choice(const json & tc) {
    tool_choice out;
    if (tc.is_null()) {
        return out;
    }
    if (tc.is_string()) {
        const std::string s = tc.get<std::string>();
        if (s == "auto") out.kind = tool_choice_kind::auto_;
        else if (s == "none") out.kind = tool_choice_kind::none;
        else if (s == "required") out.kind = tool_choice_kind::required;
        else throw std::invalid_argument("Invalid tool_choice: " + s);
        return out;
    }
    if (tc.is_object()) {
        // {"type": "function", "function": {"name": "..."}}
        const json fn = json_value(tc, "function", json::object());
        const std::string name = json_value(fn, "name", std::string());
        if (name.empty()) {
            throw std::invalid_argument("Invalid tool_choice: " + tc.dump());
        }
        out.kind = tool_choice_kind::named;
        out.name = name;
        return out;
    }
    throw std::invalid_argument("Invalid tool_choice: " + tc.dump());
}

// `tojson` as Hugging Face transformers renders a chat template: its own filter is json.dumps(x, ensure_ascii=False)
// with the default separators ", " and ": " and the keys in insertion order. The model saw the tools in this form.
static void jinja_tojson_string(const std::string & s, std::string & out) {
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    out += string_format("\\u%04x", c);
                } else {
                    out += (char) c;
                }
        }
    }
    out += '"';
}

static void jinja_tojson(const json & v, std::string & out) {
    if (v.is_string()) {
        jinja_tojson_string(v.get<std::string>(), out);
    } else if (v.is_object()) {
        out += '{';
        bool first = true;
        for (const auto & [k, item] : v.items()) {
            if (!first) out += ", ";
            first = false;
            jinja_tojson_string(k, out);
            out += ": ";
            jinja_tojson(item, out);
        }
        out += '}';
    } else if (v.is_array()) {
        out += '[';
        for (size_t i = 0; i < v.size(); i++) {
            if (i) out += ", ";
            jinja_tojson(v[i], out);
        }
        out += ']';
    } else {
        out += v.dump();  // numbers, booleans, null
    }
}

std::string jinja_tojson(const json & v) {
    std::string out;
    jinja_tojson(v, out);
    return out;
}

std::string lfm3_render_tools(const std::vector<chat_tool> & tools) {
    // the template writes every non-deferred tool's function object as one line inside <tools>:
    // "{" + for k, v in fn.items(): k | tojson + ": " + v | tojson, joined by ", " + "}"
    std::vector<std::string> lines;
    for (const auto & t : tools) {
        if (t.defer_loading) {
            continue;
        }
        std::string line = "{\"name\": " + jinja_tojson(json(t.name));
        if (!t.description.empty()) {
            line += ", \"description\": " + jinja_tojson(json(t.description));
        }
        line += ", \"parameters\": " + jinja_tojson(t.parameters) + "}";
        lines.push_back(line);
    }
    if (lines.empty()) {
        return "";
    }
    return "<tools>\n" + string_join(lines, "\n") + "\n</tools>";
}

static std::string lfm3_render_value(const json & v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_null()) return "null";
    return v.dump();
}

std::string lfm3_render_tool_calls(const json & oai_tool_calls) {
    std::string out;
    if (!oai_tool_calls.is_array()) {
        return out;
    }
    bool first = true;
    for (const auto & tc : oai_tool_calls) {
        if (!first) out += "\n\n";
        first = false;
        const json fn = tc.contains("function") ? tc.at("function") : tc;
        const std::string name = json_value(fn, "name", std::string());
        json args = fn.contains("arguments") ? fn.at("arguments") : json::object();
        if (args.is_string()) {
            const std::string s = string_strip(args.get<std::string>());
            if (s.empty() || s == "{}" || s == "null") {
                args = json::object();
            } else {
                try {
                    args = json::parse(s);
                } catch (...) {
                    args = json::object({{"input", s}});
                }
            }
        }
        out += "<tool_call>\n<function=" + name + ">\n";
        if (args.is_object()) {
            for (const auto & [k, v] : args.items()) {
                out += "<parameter=" + k + ">\n" + lfm3_render_value(v) + "\n</parameter>\n";
            }
        }
        out += "</function>\n</tool_call>";
    }
    return out;
}

std::string lfm3_render_tool_response(const json & content) {
    std::string body;
    if (content.is_string()) {
        const std::string s = content.get<std::string>();
        if (s.find("<tool_response>") != std::string::npos) {
            return s;
        }
        body = s;
    } else if (content.is_array()) {
        std::vector<std::string> items;
        for (const auto & item : content) {
            if (item.is_string()) {
                items.push_back(item.get<std::string>());
            } else if (item.is_object() && json_value(item, "type", std::string()) == "text") {
                items.push_back(json_value(item, "text", std::string()));
            } else if (item.is_object() && json_value(item, "type", std::string()) == "tool_reference") {
                items.push_back("<additional_tools>\n" + json {{"name", json_value(item, "name", std::string())}}.dump() + "\n</additional_tools>");
            } else {
                items.push_back(item.dump());
            }
        }
        body = string_join(items, "\n");
    } else if (!content.is_null()) {
        body = content.dump();
    }
    return "<tool_response>\n" + body + "\n</tool_response>";
}

// the JSON value of a parameter: typed by the tool's schema when known, else by what parses
static json coerce_tool_argument(const std::string & raw, const chat_tool * tool, const std::string & param) {
    std::string type;
    if (tool && tool->parameters.is_object()) {
        const json props = json_value(tool->parameters, "properties", json::object());
        if (props.is_object() && props.contains(param) && props.at(param).is_object()) {
            const json & t = props.at(param).at("type");
            if (t.is_string()) {
                type = t.get<std::string>();
            } else if (t.is_array() && !t.empty() && t[0].is_string()) {
                type = t[0].get<std::string>();
            }
        }
    }
    if (type == "string") {
        return raw;
    }
    const std::string s = string_strip(raw);
    if (!type.empty() || !s.empty()) {
        try {
            json parsed = json::parse(s);
            if (type.empty() && parsed.is_string()) {
                return raw;  // a quoted string without a schema is more likely literal text
            }
            return parsed;
        } catch (...) {
        }
    }
    return raw;
}

// parses one "<function=NAME>...</function>" body (between <tool_call> and </tool_call>)
static bool parse_lfm3_call_block(const std::string & block, const std::vector<chat_tool> & tools, tool_call_out & out) {
    size_t pos = block.find("<function=");
    if (pos == std::string::npos) {
        return false;
    }
    pos += strlen("<function=");
    const size_t name_end = block.find('>', pos);
    if (name_end == std::string::npos) {
        return false;
    }
    out.name = string_strip(block.substr(pos, name_end - pos));
    if (out.name.empty()) {
        return false;
    }
    const chat_tool * tool = nullptr;
    for (const auto & t : tools) {
        if (t.name == out.name) {
            tool = &t;
            break;
        }
    }
    json args = json::object();
    pos = name_end + 1;
    while (true) {
        const size_t p = block.find("<parameter=", pos);
        const size_t f = block.find("</function>", pos);
        if (p == std::string::npos || (f != std::string::npos && f < p)) {
            break;
        }
        const size_t pname_start = p + strlen("<parameter=");
        const size_t pname_end = block.find('>', pname_start);
        if (pname_end == std::string::npos) {
            break;
        }
        const std::string pname = string_strip(block.substr(pname_start, pname_end - pname_start));
        size_t vstart = pname_end + 1;
        if (vstart < block.size() && block[vstart] == '\n') {
            vstart++;
        }
        size_t vend = block.find("</parameter>", vstart);
        if (vend == std::string::npos) {
            vend = block.size();
        }
        std::string value = block.substr(vstart, vend - vstart);
        if (!value.empty() && value.back() == '\n') {
            value.pop_back();
        }
        args[pname] = coerce_tool_argument(value, tool, pname);
        pos = vend == block.size() ? vend : vend + strlen("</parameter>");
    }
    out.arguments = args.dump();
    return true;
}

static const char * TOOL_CALL_START = "<tool_call>";
static const char * TOOL_CALL_END   = "</tool_call>";

void lfm3_parse_tool_calls(const std::string & text, const std::vector<chat_tool> & tools,
                           std::string & content, std::vector<tool_call_out> & calls) {
    content.clear();
    calls.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t s = text.find(TOOL_CALL_START, pos);
        if (s == std::string::npos) {
            content += text.substr(pos);
            break;
        }
        content += text.substr(pos, s - pos);
        const size_t body_start = s + strlen(TOOL_CALL_START);
        const size_t e = text.find(TOOL_CALL_END, body_start);
        if (e == std::string::npos) {
            content += text.substr(s);  // unterminated: plain text
            break;
        }
        tool_call_out tc;
        if (parse_lfm3_call_block(text.substr(body_start, e - body_start), tools, tc)) {
            tc.id = gen_tool_call_id();
            calls.push_back(std::move(tc));
        } else {
            content += text.substr(s, e + strlen(TOOL_CALL_END) - s);
        }
        pos = e + strlen(TOOL_CALL_END);
    }
    if (!calls.empty() && string_strip(content).empty()) {
        content.clear();
    }
}

lfm3_tool_parser::lfm3_tool_parser(bool enabled, std::vector<chat_tool> tools) : enabled_(enabled), tools_(std::move(tools)) {}

// length of the longest suffix of s that is a prefix of tag (0 when none)
static size_t partial_tag_suffix(const std::string & s, const char * tag) {
    const size_t tag_len = strlen(tag);
    const size_t max_len = std::min(s.size(), tag_len - 1);
    for (size_t len = max_len; len > 0; len--) {
        if (s.compare(s.size() - len, len, tag, len) == 0) {
            return len;
        }
    }
    return 0;
}

void lfm3_tool_parser::feed(const std::string & piece, std::string & content_delta, std::vector<tool_call_out> & completed) {
    if (!enabled_) {
        content_delta = piece;
        content_ += piece;
        return;
    }
    pending_ += piece;
    while (true) {
        if (!in_call_) {
            const size_t s = pending_.find(TOOL_CALL_START);
            if (s != std::string::npos) {
                std::string before = pending_.substr(0, s);
                if (!calls_.empty() && string_strip(before).empty()) {
                    before.clear();  // whitespace between tool calls
                }
                content_delta += before;
                content_ += before;
                pending_.erase(0, s + strlen(TOOL_CALL_START));
                in_call_ = true;
                continue;
            }
            // release everything but a possible start of the tag
            const size_t hold = partial_tag_suffix(pending_, TOOL_CALL_START);
            std::string release = pending_.substr(0, pending_.size() - hold);
            pending_.erase(0, pending_.size() - hold);
            if (!calls_.empty() && string_strip(release).empty()) {
                // trailing whitespace after a tool call: keep holding, it is dropped at finish()
                pending_ = release + pending_;
                return;
            }
            content_delta += release;
            content_ += release;
            return;
        }
        const size_t e = pending_.find(TOOL_CALL_END);
        if (e == std::string::npos) {
            return;  // the block is not complete yet
        }
        tool_call_out tc;
        if (parse_lfm3_call_block(pending_.substr(0, e), tools_, tc)) {
            tc.id = gen_tool_call_id();
            calls_.push_back(tc);
            completed.push_back(tc);
        } else {
            const std::string raw = std::string(TOOL_CALL_START) + pending_.substr(0, e + strlen(TOOL_CALL_END));
            content_delta += raw;
            content_ += raw;
        }
        pending_.erase(0, e + strlen(TOOL_CALL_END));
        in_call_ = false;
    }
}

void lfm3_tool_parser::finish(std::string & content_delta, std::vector<tool_call_out> & completed) {
    (void) completed;
    if (!enabled_) {
        return;
    }
    if (in_call_) {
        // an unterminated block: show it as text
        const std::string raw = std::string(TOOL_CALL_START) + pending_;
        content_delta += raw;
        content_ += raw;
        in_call_ = false;
    } else if (!pending_.empty()) {
        if (!(!calls_.empty() && string_strip(pending_).empty())) {
            content_delta += pending_;
            content_ += pending_;
        }
    }
    pending_.clear();
}

//
// media
//

namespace {

bool ipv4_is_public(const unsigned char * b) {
    return !(b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224
        || (b[0] == 100 && (b[1] & 0xc0) == 64)
        || (b[0] == 169 && b[1] == 254)
        || (b[0] == 172 && (b[1] & 0xf0) == 16)
        || (b[0] == 192 && b[1] == 168)
        || (b[0] == 192 && b[1] == 0 && b[2] == 0)
        || (b[0] == 198 && (b[1] & 0xfe) == 18));
}

bool addr_is_public(const std::string & addr) {
    unsigned char b[16];
    if (inet_pton(AF_INET, addr.c_str(), b) == 1) {
        return ipv4_is_public(b);
    }
    if (inet_pton(AF_INET6, addr.c_str(), b) != 1) {
        return false;
    }
    static const unsigned char v4_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    static const unsigned char nat64[12]     = {0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0};
    static const unsigned char zero[12]      = {};
    if (memcmp(b, v4_mapped, 12) == 0 || memcmp(b, nat64, 12) == 0) {
        return ipv4_is_public(b + 12);
    }
    return memcmp(b, zero, 12) != 0 && (b[0] & 0xfe) != 0xfc && !(b[0] == 0xfe && (b[1] & 0x80) == 0x80) && b[0] != 0xff;
}

// splits "http://host:port/path" into scheme host-with-port and path
bool split_url(const std::string & url, std::string & scheme_host, std::string & path) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    const size_t path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        scheme_host = url;
        path = "/";
    } else {
        scheme_host = url.substr(0, path_start);
        path = url.substr(path_start);
    }
    return true;
}

std::string host_of(const std::string & scheme_host) {
    const size_t scheme_end = scheme_host.find("://");
    std::string hp = scheme_host.substr(scheme_end + 3);
    if (!hp.empty() && hp[0] == '[') {
        const size_t e = hp.find(']');
        return e == std::string::npos ? hp : hp.substr(1, e - 1);
    }
    const size_t colon = hp.rfind(':');
    return colon == std::string::npos ? hp : hp.substr(0, colon);
}

raw_buffer download_media(const std::string & url, bool allow_private) {
    std::string scheme_host, path;
    if (!split_url(url, scheme_host, path)) {
        throw std::invalid_argument("invalid media URL: " + url);
    }
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (string_starts_with(url, "https://")) {
        throw api_error(ERROR_TYPE_NOT_SUPPORTED, "https media URLs need an OpenSSL build of the server (or send the media as a data URL)");
    }
#endif
    const std::string host = host_of(scheme_host);
    std::vector<std::string> addrs;
    httplib::hosted_at(host, addrs);
    if (addrs.empty()) {
        throw std::invalid_argument("cannot resolve host: " + host);
    }
    for (const auto & a : addrs) {
        if (!allow_private && !addr_is_public(a)) {
            throw std::invalid_argument("host is not a public address: " + host);
        }
    }
    SRV_INF("downloading media from '%s'\n", url.c_str());
    httplib::Client cli(scheme_host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);
    cli.set_follow_location(true);
    raw_buffer data;
    const size_t max_size = 10 * 1024 * 1024;
    auto res = cli.Get(path, [&](const char * d, size_t n) {
        if (data.size() + n > max_size) return false;
        data.insert(data.end(), d, d + n);
        return true;
    });
    if (!res) {
        throw std::runtime_error("Failed to download image: " + httplib::to_string(res.error()));
    }
    if (res->status < 200 || res->status >= 300) {
        throw std::runtime_error(string_format("Failed to download image: HTTP %d", res->status));
    }
    SRV_INF("downloaded %zu bytes\n", data.size());
    return data;
}

bool validate_media_filename(const std::string & path) {
    // relative, no traversal, no control characters
    if (path.empty() || path.find("..") != std::string::npos) return false;
    for (unsigned char c : path) {
        if (c < 0x20) return false;
    }
    return true;
}

}  // namespace

raw_buffer load_media_url(const std::string & url, const media_options & opt, std::string * mime_out) {
    if (mime_out) mime_out->clear();
    if (string_starts_with(url, "http://") || string_starts_with(url, "https://")) {
        return download_media(url, opt.allow_private_urls);
    }
    if (string_starts_with(url, "file://")) {
        if (opt.media_path.empty()) {
            throw std::invalid_argument("file:// URLs are not allowed unless --media-path is specified");
        }
        std::string file_path = url.substr(7);
        while (!file_path.empty() && file_path[0] == '/') {
            file_path.erase(0, 1);
        }
        if (!validate_media_filename(file_path)) {
            throw std::invalid_argument("file path is not allowed: " + file_path);
        }
        std::ifstream file(opt.media_path + file_path, std::ios::binary);
        if (!file) {
            throw std::invalid_argument("file does not exist or cannot be opened: " + file_path);
        }
        raw_buffer data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return data;
    }
    if (string_starts_with(string_lower(url.substr(0, 5)), "data:")) {
        std::string mime;
        raw_buffer out;
        parse_data_url(url, mime, out);  // throws on malformed
        if (!string_starts_with(mime, "image/") && !string_starts_with(mime, "video/") && !string_starts_with(mime, "audio/")) {
            throw std::invalid_argument("Invalid uri format: data:" + mime);
        }
        if (mime_out) *mime_out = mime;
        return out;
    }
    // raw base64
    raw_buffer out = base64_decode(url);
    if (out.empty()) {
        throw std::invalid_argument("Invalid base64 value");
    }
    return out;
}

namespace {

// PPM stream (ffmpeg -vcodec ppm) -> frames
std::vector<DecodedImage> parse_ppm_stream(const std::string & data) {
    std::vector<DecodedImage> frames;
    size_t pos = 0;
    while (pos + 2 < data.size()) {
        if (data.compare(pos, 2, "P6") != 0) {
            break;
        }
        // "P6\n<w> <h>\n<maxval>\n"
        size_t p = pos + 2;
        int vals[3];
        for (int i = 0; i < 3; i++) {
            while (p < data.size() && isspace((unsigned char) data[p])) p++;
            size_t q = p;
            while (q < data.size() && isdigit((unsigned char) data[q])) q++;
            if (q == p) return frames;
            vals[i] = std::stoi(data.substr(p, q - p));
            p = q;
        }
        p++;  // the single whitespace after maxval
        const size_t n = (size_t) vals[0] * vals[1] * 3;
        if (p + n > data.size()) break;
        DecodedImage img;
        img.width = vals[0];
        img.height = vals[1];
        img.rgb.assign(data.begin() + p, data.begin() + p + n);
        frames.push_back(std::move(img));
        pos = p + n;
    }
    return frames;
}

std::vector<DecodedImage> ffmpeg_decode_video(const raw_buffer & bytes, double fps, const media_options & opt) {
    const std::string ffmpeg = opt.ffmpeg_dir.empty() ? "ffmpeg" : opt.ffmpeg_dir + "/ffmpeg";
    // ffmpeg needs a seekable input for most containers: a temporary file
    char path[] = "/tmp/ixian-server-video-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        throw std::runtime_error("cannot create a temporary file for the video");
    }
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = write(fd, bytes.data() + written, bytes.size() - written);
        if (n <= 0) break;
        written += (size_t) n;
    }
    close(fd);
    const int size = opt.image_size;
    const std::string cmd = string_format(
        "%s -v error -nostdin -i '%s' -vf \"fps=%g,scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2\" -f image2pipe -vcodec ppm - 2>/dev/null",
        ffmpeg.c_str(), path, fps, size, size, size, size);
    std::string out;
    FILE * p = popen(cmd.c_str(), "r");
    if (p) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) {
            out.append(buf, n);
        }
        pclose(p);
    }
    unlink(path);
    if (!p) {
        throw api_error(ERROR_TYPE_NOT_SUPPORTED, "video input needs ffmpeg (install it or set --video-ffmpeg-dir)");
    }
    auto frames = parse_ppm_stream(out);
    if (frames.empty()) {
        throw api_error(ERROR_TYPE_NOT_SUPPORTED, "video input needs ffmpeg (install it or set --video-ffmpeg-dir), or the video could not be decoded");
    }
    return frames;
}

}  // namespace

std::vector<zx::Part> media_to_parts(const raw_buffer & bytes, const std::string & kind_hint, const media_options & opt) {
    const std::string sniffed = sniff_media_kind(bytes.data(), bytes.size());
    std::string kind = kind_hint.empty() ? sniffed : kind_hint;
    if (!kind_hint.empty() && !sniffed.empty() && sniffed != kind_hint) {
        throw std::invalid_argument("Failed to load " + kind_hint + " file: the data is " + sniffed);
    }
    if (kind.empty()) {
        throw std::invalid_argument("Failed to load image or audio file: unknown media format");
    }
    std::vector<zx::Part> parts;
    if (kind == "image") {
        if (!opt.allow_image) {
            throw std::runtime_error("image input is not supported by this model");
        }
        DecodedImage img = decode_image(bytes.data(), bytes.size());
        if (img.width != opt.image_size || img.height != opt.image_size) {
            SRV_DBG("image %dx%d -> %dx%d (%s)\n", img.width, img.height, opt.image_size, opt.image_size, image_fit_name(opt.image_fit));
            img = fit_image(img, opt.image_size, opt.image_size, opt.image_fit);
        }
        parts.push_back(zx::Part::Img(zx::Image::fromRgb(img.rgb.data(), img.width, img.height)));
        return parts;
    }
    if (kind == "audio") {
        if (!opt.allow_audio) {
            throw std::runtime_error("audio input is not supported by this model");
        }
        DecodedAudio audio = decode_audio(bytes.data(), bytes.size());
        std::vector<float> mono = audio_to_mono(audio);
        if (mono.empty()) {
            throw std::invalid_argument("the audio clip is empty");
        }
        for (const auto & clip : split_audio(mono, audio.sample_rate, opt.audio_max_seconds)) {
            parts.push_back(zx::Part::Aud(zx::Audio::fromPcm(clip.data(), clip.size(), audio.sample_rate, 1)));
        }
        return parts;
    }
    if (kind == "video") {
        if (!opt.allow_video) {
            throw std::runtime_error("video input is not supported by this model");
        }
        auto frames = ffmpeg_decode_video(bytes, opt.video_fps, opt);
        std::vector<zx::Image> sdk_frames;
        sdk_frames.reserve(frames.size());
        for (auto & f : frames) {
            if (f.width != opt.image_size || f.height != opt.image_size) {
                f = fit_image(f, opt.image_size, opt.image_size, ImageFit::Letterbox);
            }
            sdk_frames.push_back(zx::Image::fromRgb(f.rgb.data(), f.width, f.height));
        }
        parts.push_back(zx::Part::Vid(zx::Video::fromFrames(std::move(sdk_frames), opt.video_fps)));
        return parts;
    }
    throw std::invalid_argument("unsupported media kind: " + kind);
}

zx::Part video_frames_to_part(const std::vector<raw_buffer> & frames, double fps, const media_options & opt) {
    if (!opt.allow_video) {
        throw std::runtime_error("video input is not supported by this model");
    }
    if (frames.empty()) {
        throw std::invalid_argument("video_frames needs at least one frame");
    }
    std::vector<zx::Image> sdk_frames;
    for (const auto & bytes : frames) {
        DecodedImage img = decode_image(bytes.data(), bytes.size());
        if (img.width != opt.image_size || img.height != opt.image_size) {
            img = fit_image(img, opt.image_size, opt.image_size, ImageFit::Letterbox);
        }
        sdk_frames.push_back(zx::Image::fromRgb(img.rgb.data(), img.width, img.height));
    }
    return zx::Part::Vid(zx::Video::fromFrames(std::move(sdk_frames), fps > 0 ? fps : opt.video_fps));
}

//
// chat conversion
//

std::vector<zx::Part> oai_content_to_parts(const json & content, const media_options & opt, bool allow_media, int * n_media) {
    std::vector<zx::Part> parts;
    if (content.is_null()) {
        return parts;
    }
    if (content.is_string()) {
        parts.push_back(zx::Part::Text(content.get<std::string>()));
        return parts;
    }
    if (!content.is_array()) {
        throw std::invalid_argument("Expected 'content' to be a string or an array");
    }
    for (const auto & p : content) {
        if (!p.is_object() || !p.contains("type")) {
            throw std::invalid_argument("Missing content part type: " + p.dump());
        }
        const std::string type = json_value(p, "type", std::string());
        if (type == "text") {
            if (!p.contains("text") || !p.at("text").is_string()) {
                throw std::invalid_argument("text content part needs a string 'text': " + p.dump());
            }
            parts.push_back(zx::Part::Text(p.at("text").get<std::string>()));
        } else if (type == "image_url") {
            if (!allow_media || !opt.allow_image) {
                throw std::runtime_error("image input is not supported - hint: this model takes no images");
            }
            const json image_url = json_value(p, "image_url", json::object());
            const std::string url = image_url.is_string() ? image_url.get<std::string>() : json_value(image_url, "url", std::string());
            std::string mime;
            const raw_buffer bytes = load_media_url(url, opt, &mime);
            for (auto & part : media_to_parts(bytes, string_starts_with(mime, "image/") ? "image" : "", opt)) {
                parts.push_back(std::move(part));
            }
            if (n_media) (*n_media)++;
        } else if (type == "input_audio") {
            if (!allow_media || !opt.allow_audio) {
                throw std::runtime_error("audio input is not supported - hint: this model takes no audio");
            }
            const json input_audio = json_value(p, "input_audio", json::object());
            const std::string url = json_value(input_audio, "data", json_value(input_audio, "url", std::string()));
            const raw_buffer bytes = load_media_url(url, opt);
            for (auto & part : media_to_parts(bytes, "audio", opt)) {
                parts.push_back(std::move(part));
            }
            if (n_media) (*n_media)++;
        } else if (type == "input_video" || type == "video_url") {
            if (!allow_media || !opt.allow_video) {
                throw std::runtime_error("video input is not supported - hint: this model takes no video");
            }
            const json input_video = json_value(p, type, json::object());
            const std::string url = input_video.is_string() ? input_video.get<std::string>() : json_value(input_video, "data", json_value(input_video, "url", std::string()));
            const raw_buffer bytes = load_media_url(url, opt);
            for (auto & part : media_to_parts(bytes, "video", opt)) {
                parts.push_back(std::move(part));
            }
            if (n_media) (*n_media)++;
        } else if (type == "video_frames") {
            if (!allow_media || !opt.allow_video) {
                throw std::runtime_error("video input is not supported - hint: this model takes no video");
            }
            std::vector<raw_buffer> frames;
            for (const auto & f : json_value(p, "frames", json::array())) {
                const std::string url = f.is_string() ? f.get<std::string>() : json_value(f, "url", std::string());
                frames.push_back(load_media_url(url, opt));
            }
            parts.push_back(video_frames_to_part(frames, json_value(p, "fps", 0.0), opt));
            if (n_media) (*n_media)++;
        } else if (type == "tool_reference") {
            parts.push_back(zx::Part::Text("<additional_tools>\n" + json {{"name", json_value(p, "name", std::string())}}.dump() + "\n</additional_tools>"));
        } else {
            throw std::invalid_argument("unsupported content[].type");
        }
    }
    return parts;
}

static std::string parts_text(const std::vector<zx::Part> & parts) {
    std::string out;
    for (const auto & p : parts) {
        if (p.type == zx::Part::Type::Text) {
            out += p.text;
        }
    }
    return out;
}

chat_inputs oai_chat_to_sdk(const json & body, const chat_convert_options & opt) {
    chat_inputs out;

    if (!body.is_object() || !body.contains("messages")) {
        throw std::invalid_argument("'messages' is required");
    }
    const json & messages = body.at("messages");
    if (!messages.is_array()) {
        throw std::invalid_argument("Expected 'messages' to be an array");
    }

    out.tools  = parse_tools(json_value(body, "tools", json()));
    out.choice = parse_tool_choice(body.contains("tool_choice") ? body.at("tool_choice") : json());
    out.parallel_tool_calls = json_value(body, "parallel_tool_calls", true);
    out.reasoning_fmt = opt.reasoning_fmt;
    if (body.contains("reasoning_format") && body.at("reasoning_format").is_string()) {
        out.reasoning_fmt = reasoning_format_from_name(body.at("reasoning_format").get<std::string>());
    }

    // grammar: the NPU runtime samples without a logit processor
    if (body.contains("grammar") && !body.at("grammar").is_null() && !json_value(body, "grammar", std::string()).empty()) {
        throw api_error(ERROR_TYPE_NOT_SUPPORTED, "grammar-constrained sampling is not available on the Ixian Hexagon runtime (use response_format or json_schema for a prompt-level JSON request)");
    }

    // response_format / json_schema (soft: rendered into the system prompt)
    json json_schema = json_value(body, "json_schema", json());
    if (body.contains("response_format") && !body.at("response_format").is_null()) {
        const json response_format = body.at("response_format");
        if (!response_format.is_object()) {
            throw std::invalid_argument("response_format must be an object");
        }
        const std::string response_type = json_value(response_format, "type", std::string());
        if (response_type == "json_object") {
            if (response_format.contains("schema") || json_schema.is_null()) {
                json_schema = json_value(response_format, "schema", json::object());
            }
        } else if (response_type == "json_schema") {
            if (response_format.contains("json_schema")) {
                const json wrapper = json_value(response_format, "json_schema", json::object());
                json_schema = json_value(wrapper, "schema", json::object());
            } else {
                json_schema = json_value(response_format, "schema", json::object());
            }
        } else if (!response_type.empty() && response_type != "text") {
            throw std::invalid_argument("response_format type must be one of \"text\", \"json_object\" or \"json_schema\", but got: " + response_type);
        }
    }
    if (!json_schema.is_null()) {
        if (!json_schema.is_object()) {
            throw std::invalid_argument("json_schema must be an object");
        }
        if (json_schema.contains("type") && !json_schema.at("type").is_string() && !json_schema.at("type").is_array()) {
            throw std::invalid_argument("json_schema: 'type' must be a string");
        }
        if (json_schema.contains("type") && json_schema.at("type").is_string()) {
            static const std::vector<std::string> known = {"object", "array", "string", "number", "integer", "boolean", "null"};
            const std::string t = json_schema.at("type").get<std::string>();
            if (std::find(known.begin(), known.end(), t) == known.end()) {
                throw std::invalid_argument("json_schema: unknown type '" + t + "'");
            }
        }
        if (json_schema.empty()) {
            json_schema["type"] = "object";
        }
        out.json_schema = json_schema;
    }

    // thinking: server default, then --chat-template-kwargs, then the request's chat_template_kwargs, then reasoning_effort
    out.enable_thinking = opt.enable_thinking;
    std::map<std::string, std::string> kwargs = opt.template_kwargs;
    const json kw = json_value(body, "chat_template_kwargs", json::object());
    if (!kw.is_object()) {
        throw std::invalid_argument("chat_template_kwargs must be an object");
    }
    for (const auto & [k, v] : kw.items()) {
        kwargs[k] = v.dump();
    }
    const auto et = kwargs.find("enable_thinking");
    if (et != kwargs.end()) {
        if (et->second == "true") out.enable_thinking = true;
        else if (et->second == "false") out.enable_thinking = false;
        else if (!et->second.empty() && et->second[0] == '"') throw std::invalid_argument("invalid type for \"enable_thinking\" (expected boolean, got string)");
    }
    if (body.contains("reasoning_effort")) {
        const std::string effort = json_value(body, "reasoning_effort", std::string());
        if (effort == "none") {
            out.enable_thinking = false;
        }
    }

    // continuation of the final assistant message: the runtime always appends its own generation prompt
    if (body.contains("continue_final_message")) {
        const json & cfm = body.at("continue_final_message");
        const bool wants = cfm.is_boolean() ? cfm.get<bool>() : (cfm.is_string() && cfm.get<std::string>() != "none");
        if (wants && json_value(body, "add_generation_prompt", true) && cfm.is_boolean()) {
            throw std::invalid_argument("Cannot set both add_generation_prompt and continue_final_message to true.");
        }
        if (wants) {
            throw api_error(ERROR_TYPE_NOT_SUPPORTED, "continue_final_message (assistant prefill) is not available on the Ixian Hexagon runtime: the runtime always starts a new assistant turn");
        }
    }

    // pass 1: validate, like llama.cpp's common_chat_msgs_parse_oaicompat
    for (const auto & msg : messages) {
        if (!msg.is_object()) {
            throw std::invalid_argument("Expected 'message' to be an object, got " + msg.dump());
        }
        if (!msg.contains("role") || !msg.at("role").is_string()) {
            throw std::invalid_argument("Missing 'role' in message: " + msg.dump());
        }
        const std::string role = msg.at("role");
        if (role != "assistant" && !msg.contains("content")) {
            throw std::invalid_argument("All non-assistant messages must contain 'content'");
        }
        if (role == "assistant" && !msg.contains("content") && !msg.contains("tool_calls")) {
            throw std::invalid_argument("Assistant message must contain either 'content' or 'tool_calls'!");
        }
        if (msg.contains("content")) {
            const json & c = msg.at("content");
            if (!c.is_string() && !c.is_array() && !c.is_null()) {
                throw std::invalid_argument("Invalid 'content' type: expected string or array, got " + c.dump());
            }
        }
        if (msg.contains("tool_calls")) {
            if (!msg.at("tool_calls").is_array()) {
                throw std::invalid_argument("tool_calls must be an array");
            }
            for (const auto & tc : msg.at("tool_calls")) {
                if (!tc.is_object() || !tc.contains("type")) throw std::invalid_argument("Missing tool call type: " + tc.dump());
                if (tc.at("type") != "function") throw std::invalid_argument("Unsupported tool call type: " + tc.dump());
                if (!tc.contains("function") || !tc.at("function").is_object()) throw std::invalid_argument("Missing tool call function: " + tc.dump());
                if (!tc.at("function").contains("name")) throw std::invalid_argument("Missing tool call name: " + tc.dump());
                if (!tc.at("function").contains("arguments")) throw std::invalid_argument("Missing tool call arguments: " + tc.dump());
            }
        }
    }
    if (messages.size() >= 2) {
        const std::string last = json_value(messages[messages.size() - 1], "role", std::string());
        const std::string prev = json_value(messages[messages.size() - 2], "role", std::string());
        if (last == "assistant" && prev == "assistant") {
            throw std::invalid_argument("Cannot have 2 or more assistant messages at the end of the list.");
        }
    }

    // pass 2: build the SDK messages
    const bool has_tools = !out.tools.empty() && out.choice.kind != tool_choice_kind::none;
    std::string system_text;
    bool has_system = false;
    std::vector<zx::Message> converted;
    std::vector<std::string> pending_tool_results;  // consecutive tool messages become one user turn

    auto flush_tool_results = [&]() {
        if (pending_tool_results.empty()) return;
        converted.push_back(zx::Message::User(string_join(pending_tool_results, "\n")));
        pending_tool_results.clear();
    };

    for (const auto & msg : messages) {
        const std::string role = msg.at("role");
        const json content = msg.contains("content") ? msg.at("content") : json();
        if (role == "system" || role == "developer") {
            std::string text = parts_text(oai_content_to_parts(content, opt.media, false, nullptr));
            if (!system_text.empty() && !text.empty()) system_text += "\n";
            system_text += text;
            has_system = true;
            continue;
        }
        if (role == "user") {
            flush_tool_results();
            converted.push_back(zx::Message::User(oai_content_to_parts(content, opt.media, true, &out.n_media)));
            continue;
        }
        if (role == "assistant") {
            flush_tool_results();
            std::string text = parts_text(oai_content_to_parts(content, opt.media, false, nullptr));
            if (msg.contains("tool_calls") && msg.at("tool_calls").is_array() && !msg.at("tool_calls").empty()) {
                text += lfm3_render_tool_calls(msg.at("tool_calls"));
            }
            zx::Message recalled;
            if (opt.eng && !msg.contains("tool_calls") && opt.eng->recall_reply(text, recalled)) {
                converted.push_back(std::move(recalled));  // exact ids of the reply the SDK produced earlier
            } else {
                converted.push_back(zx::Message::Assistant(text));
            }
            continue;
        }
        if (role == "tool") {
            pending_tool_results.push_back(lfm3_render_tool_response(content));
            continue;
        }
        throw std::invalid_argument("Unsupported message role: " + role);
    }
    flush_tool_results();

    // the system prompt: the user's text, the tools, the output format
    if (has_tools) {
        const std::string tools_text = lfm3_render_tools(out.tools);
        if (!tools_text.empty()) {
            if (!system_text.empty()) system_text += "\n\n";
            system_text += tools_text;
        }
        if (out.choice.kind == tool_choice_kind::required) {
            system_text += "\n\nYou must answer with one or more tool calls.";
        } else if (out.choice.kind == tool_choice_kind::named) {
            system_text += "\n\nYou must answer with a call to the tool \"" + out.choice.name + "\".";
        }
        has_system = true;
    }
    if (!out.json_schema.is_null()) {
        if (!system_text.empty()) system_text += "\n\n";
        if (out.json_schema.size() == 1 && out.json_schema.value("type", "") == "object") {
            system_text += "Answer with a single valid JSON object and nothing else.";
        } else {
            system_text += "Answer with a single JSON document that matches this JSON schema, and nothing else:\n" + out.json_schema.dump();
        }
        has_system = true;
    }
    if (has_system) {
        converted.insert(converted.begin(), zx::Message::System(system_text));
    }

    if (!converted.empty() && converted.back().role == zx::Role::Assistant) {
        out.has_trailing_assistant = true;
    }
    if (converted.empty() || (converted.size() == 1 && converted[0].role == zx::Role::System)) {
        throw std::invalid_argument("'messages' needs at least one user message");
    }

    out.messages = std::move(converted);
    return out;
}

//
// speech / ASR prompts
//

std::string lfm3_language_name(const std::string & language) {
    static const std::map<std::string, std::string> names = {
        {"ar", "Arabic"}, {"da", "Danish"}, {"de", "German"}, {"el", "Greek"}, {"en", "English"},
        {"es", "Spanish"}, {"fi", "Finnish"}, {"fr", "French"}, {"he", "Hebrew"}, {"it", "Italian"},
        {"ja", "Japanese"}, {"ko", "Korean"}, {"nl", "Dutch"}, {"pl", "Polish"}, {"pt", "Portuguese"},
        {"sv", "Swedish"}, {"tr", "Turkish"}, {"zh", "Chinese"},
    };
    const auto it = names.find(string_lower(language));
    return it != names.end() ? it->second : language;
}

std::string lfm3_asr_system_prompt(const std::string & language) {
    if (language.empty()) {
        return "Perform ASR.";
    }
    return "Perform ASR in " + lfm3_language_name(language) + ".";
}

bool lfm3_is_voice(const std::string & voice) {
    const std::string v = string_lower(voice);
    return v == "female" || v == "male";
}

std::string lfm3_tts_system_prompt(const std::string & language, const std::string & voice) {
    std::string lang = language.empty() || string_lower(language) == "en" ? "American English" : lfm3_language_name(language);
    std::string v = lfm3_is_voice(voice) ? string_lower(voice) : "female";
    return "Perform TTS in " + lang + " using a " + v + " voice.";
}

std::string lfm3_speech_system_prompt(const std::string & language, const std::string & voice) {
    const std::string v = string_lower(voice);
    const bool has_voice = v == "female" || v == "male";
    if (language.empty() && !has_voice) {
        return "";
    }
    std::string out = "Speak";
    if (!language.empty()) {
        out += " in " + lfm3_language_name(language);
    }
    if (has_voice) {
        out += " using a " + v + " voice";
    }
    return out + ".";
}

//
// Responses API -> chat completions (ported from llama.cpp's server-chat.cpp)
//

static json convert_responses_tools(const json & tools) {
    if (!tools.is_array()) {
        throw std::invalid_argument("'tools' must be an array of objects");
    }
    json chatcmpl_tools = json::array();
    for (json resp_tool : tools) {
        const std::string type = json_value(resp_tool, "type", std::string());
        if (type != "function") {
            SRV_WRN("unsupported Responses tool type '%s' skipped\n", type.c_str());
            continue;
        }
        resp_tool.erase("type");
        if (!resp_tool.contains("strict")) {
            resp_tool["strict"] = true;
        }
        chatcmpl_tools.push_back(json {{"type", "function"}, {"function", resp_tool}});
    }
    return chatcmpl_tools;
}

json convert_responses_to_chatcmpl(const json & response_body) {
    if (!response_body.contains("input")) {
        throw std::invalid_argument("'input' is required");
    }
    if (!json_value(response_body, "previous_response_id", std::string{}).empty()) {
        throw std::invalid_argument("'previous_response_id' is not supported.");
    }

    const json input_value = response_body.at("input");
    json chatcmpl_body = response_body;
    chatcmpl_body.erase("input");
    std::vector<json> chatcmpl_messages;

    if (response_body.contains("instructions")) {
        chatcmpl_messages.push_back({{"role", "system"}, {"content", json_value(response_body, "instructions", std::string())}});
        chatcmpl_body.erase("instructions");
    }

    static auto exists_and_is_array = [](const json & j, const char * key) { return j.contains(key) && j.at(key).is_array(); };
    static auto exists_and_is_string = [](const json & j, const char * key) { return j.contains(key) && j.at(key).is_string(); };

    if (input_value.is_string()) {
        chatcmpl_messages.push_back({{"role", "user"}, {"content", input_value}});
    } else if (input_value.is_array()) {
        for (json item : input_value) {
            const bool merge_prev = !chatcmpl_messages.empty() && chatcmpl_messages.back().value("role", "") == "assistant";

            if (exists_and_is_string(item, "content")) {
                item["content"] = json::array({json {{"text", item.at("content")}, {"type", "input_text"}}});
            }

            if (exists_and_is_array(item, "content") && exists_and_is_string(item, "role") &&
                (item.at("role") == "user" || item.at("role") == "system" || item.at("role") == "developer")) {
                std::vector<json> chatcmpl_content;
                for (const json & input_item : item.at("content")) {
                    const std::string type = json_value(input_item, "type", std::string());
                    if (type == "input_text") {
                        if (!input_item.contains("text")) throw std::invalid_argument("'Input text' requires 'text'");
                        chatcmpl_content.push_back({{"text", input_item.at("text")}, {"type", "text"}});
                    } else if (type == "input_image") {
                        if (!input_item.contains("image_url")) throw std::invalid_argument("'image_url' is required");
                        chatcmpl_content.push_back({{"image_url", json {{"url", input_item.at("image_url")}}}, {"type", "image_url"}});
                    } else if (type == "input_audio") {
                        chatcmpl_content.push_back({{"input_audio", json_value(input_item, "input_audio", json::object())}, {"type", "input_audio"}});
                    } else if (type == "input_file") {
                        throw std::invalid_argument("'input_file' is not supported at this moment");
                    } else {
                        throw std::invalid_argument("'type' must be one of 'input_text', 'input_image', or 'input_file'");
                    }
                }
                item.erase("type");
                item.erase("status");
                item["content"] = chatcmpl_content;
                chatcmpl_messages.push_back(item);
            } else if (exists_and_is_string(item, "role") && item.at("role") == "assistant" && exists_and_is_string(item, "type") && item.at("type") == "message") {
                json chatcmpl_content = json::array();
                if (item.contains("content") && item.at("content").is_string()) {
                    chatcmpl_content.push_back({{"text", item.at("content")}, {"type", "text"}});
                } else if (exists_and_is_array(item, "content")) {
                    for (const auto & output_text : item.at("content")) {
                        const std::string type = json_value(output_text, "type", std::string());
                        if (type == "output_text" || type == "input_text") {
                            if (!exists_and_is_string(output_text, "text")) throw std::invalid_argument("'Output text' requires 'text'");
                            chatcmpl_content.push_back({{"text", output_text.at("text")}, {"type", "text"}});
                        } else if (type == "refusal") {
                            if (!exists_and_is_string(output_text, "refusal")) throw std::invalid_argument("'Refusal' requires 'refusal'");
                            chatcmpl_content.push_back({{"text", output_text.at("refusal")}, {"type", "text"}});
                        } else {
                            throw std::invalid_argument("'type' must be one of 'output_text' or 'refusal'");
                        }
                    }
                }
                if (merge_prev) {
                    auto & prev_msg = chatcmpl_messages.back();
                    if (!exists_and_is_array(prev_msg, "content")) {
                        prev_msg["content"] = json::array();
                    }
                    for (const auto & c : chatcmpl_content) prev_msg["content"].push_back(c);
                } else {
                    item.erase("status");
                    item.erase("type");
                    item["content"] = chatcmpl_content;
                    chatcmpl_messages.push_back(item);
                }
            } else if (exists_and_is_string(item, "arguments") && exists_and_is_string(item, "call_id") && exists_and_is_string(item, "name") &&
                       exists_and_is_string(item, "type") && item.at("type") == "function_call") {
                json tool_call = {
                    {"function", json {{"arguments", item.at("arguments")}, {"name", item.at("name")}}},
                    {"id", item.at("call_id")},
                    {"type", "function"},
                };
                if (merge_prev) {
                    auto & prev_msg = chatcmpl_messages.back();
                    if (!exists_and_is_array(prev_msg, "tool_calls")) {
                        prev_msg["tool_calls"] = json::array();
                    }
                    prev_msg["tool_calls"].push_back(tool_call);
                } else {
                    chatcmpl_messages.push_back(json {{"role", "assistant"}, {"tool_calls", json::array({tool_call})}});
                }
            } else if (exists_and_is_string(item, "call_id") && (exists_and_is_string(item, "output") || exists_and_is_array(item, "output")) &&
                       exists_and_is_string(item, "type") && item.at("type") == "function_call_output") {
                if (item.at("output").is_string()) {
                    chatcmpl_messages.push_back(json {{"content", item.at("output")}, {"role", "tool"}, {"tool_call_id", item.at("call_id")}});
                } else {
                    json outputs = item.at("output");
                    for (json & o : outputs) {
                        if (!o.contains("type")) throw std::invalid_argument("Output of tool call missing 'type' field");
                        const auto type = o.at("type");
                        if (type != "input_text" && type != "input_image") throw std::invalid_argument("Output of tool call should be 'Input text' or 'Input image'");
                        if (type == "input_text") {
                            o["type"] = "text";
                        } else {
                            if (!o.contains("image_url")) throw std::invalid_argument("'image_url' is required");
                            o["type"] = "image_url";
                            o["image_url"] = json {{"url", o.at("image_url")}};
                        }
                    }
                    chatcmpl_messages.push_back(json {{"content", outputs}, {"role", "tool"}, {"tool_call_id", item.at("call_id")}});
                }
            } else if (exists_and_is_array(item, "summary") && exists_and_is_string(item, "type") && item.at("type") == "reasoning") {
                if (!exists_and_is_array(item, "content") || item.at("content").empty() || !exists_and_is_string(item.at("content")[0], "text")) {
                    throw std::invalid_argument("reasoning item needs content[0].text");
                }
                if (merge_prev) {
                    chatcmpl_messages.back()["reasoning_content"] = item.at("content")[0].at("text");
                } else {
                    chatcmpl_messages.push_back(json {{"role", "assistant"}, {"content", json::array()}, {"reasoning_content", item.at("content")[0].at("text")}});
                }
            } else {
                throw std::invalid_argument("Cannot determine type of 'item'");
            }
        }
    } else {
        throw std::invalid_argument("'input' must be a string or array of objects");
    }

    chatcmpl_body["messages"] = chatcmpl_messages;

    if (response_body.contains("tools")) {
        const json chatcmpl_tools = convert_responses_tools(response_body.at("tools"));
        chatcmpl_body.erase("tools");
        if (!chatcmpl_tools.empty()) {
            chatcmpl_body["tools"] = chatcmpl_tools;
        }
    }
    if (response_body.contains("max_output_tokens")) {
        chatcmpl_body.erase("max_output_tokens");
        chatcmpl_body["max_tokens"] = response_body["max_output_tokens"];
    }
    if (response_body.contains("reasoning")) {
        const json & reasoning = response_body.at("reasoning");
        if (reasoning.is_object() && reasoning.contains("effort")) {
            chatcmpl_body["reasoning_effort"] = reasoning.at("effort");
        }
        chatcmpl_body.erase("reasoning");
    }
    if (response_body.contains("text")) {
        const json & text = response_body.at("text");
        if (!text.is_object()) throw std::invalid_argument("'text' must be an object");
        chatcmpl_body.erase("text");
        if (text.contains("format")) {
            const json & format = text.at("format");
            if (!format.is_object()) throw std::invalid_argument("'text.format' must be an object");
            const std::string type = json_value(format, "type", std::string());
            if (type == "json_schema") {
                chatcmpl_body["response_format"] = {{"type", "json_schema"}, {"json_schema", format}};
                chatcmpl_body["response_format"]["json_schema"].erase("type");
            } else if (type == "json_object" || type == "text") {
                chatcmpl_body["response_format"] = format;
            } else if (!type.empty()) {
                throw std::invalid_argument("'text.format.type' must be one of 'text', 'json_object', or 'json_schema'");
            }
        }
    }
    if (response_body.contains("extra_params")) {
        chatcmpl_body.erase("extra_params");
        if (response_body.at("extra_params").is_object()) {
            for (const auto & [key, value] : response_body.at("extra_params").items()) {
                chatcmpl_body[key] = value;
            }
        }
    }
    return chatcmpl_body;
}

//
// Anthropic Messages -> chat completions (ported from llama.cpp's server-chat.cpp)
//

static void normalize_anthropic_billing_header(std::string & system_text) {
    if (system_text.rfind("x-anthropic-billing-header:", 0) != 0) {
        return;
    }
    const size_t header_prefix_length = strlen("x-anthropic-billing-header:");
    const size_t cch_length = 5;
    const size_t index_cch = system_text.find("cch=", header_prefix_length);
    if (index_cch == std::string::npos) {
        return;
    }
    const size_t index_replace = index_cch + 4;
    if (index_replace + cch_length < system_text.length() && system_text[index_replace + cch_length] == ';') {
        for (size_t i = 0; i < cch_length; ++i) {
            system_text[index_replace + i] = 'f';
        }
    }
}

static json anthropic_image_part(const json & block) {
    const json source = json_value(block, "source", json::object());
    const std::string source_type = json_value(source, "type", std::string());
    if (source_type == "base64") {
        const std::string media_type = json_value(source, "media_type", std::string("image/jpeg"));
        const std::string data = json_value(source, "data", std::string());
        return json {{"type", "image_url"}, {"image_url", {{"url", "data:" + media_type + ";base64," + data}}}};
    }
    if (source_type == "url") {
        return json {{"type", "image_url"}, {"image_url", {{"url", json_value(source, "url", std::string())}}}};
    }
    return json();
}

json convert_anthropic_to_oai(const json & body) {
    json oai_body;
    json oai_messages = json::array();

    const json system_param = json_value(body, "system", json());
    if (!system_param.is_null()) {
        std::string system_content;
        if (system_param.is_string()) {
            system_content = system_param.get<std::string>();
            normalize_anthropic_billing_header(system_content);
        } else if (system_param.is_array()) {
            for (const auto & block : system_param) {
                if (json_value(block, "type", std::string()) == "text") {
                    std::string t = json_value(block, "text", std::string());
                    normalize_anthropic_billing_header(t);
                    system_content += t;
                }
            }
        }
        oai_messages.push_back({{"role", "system"}, {"content", system_content}});
    }

    if (!body.contains("messages")) {
        throw std::invalid_argument("'messages' is required");
    }
    const json & messages = body.at("messages");
    if (messages.is_array()) {
        for (const auto & msg : messages) {
            const std::string role = json_value(msg, "role", std::string());
            if (!msg.contains("content")) {
                if (role == "assistant") continue;
                oai_messages.push_back(msg);
                continue;
            }
            const json & content = msg.at("content");
            if (content.is_string() || !content.is_array()) {
                oai_messages.push_back(msg);
                continue;
            }
            json tool_calls = json::array();
            json converted_content = json::array();
            json tool_results = json::array();
            std::string reasoning_content;
            bool has_tool_calls = false;

            for (const auto & block : content) {
                const std::string type = json_value(block, "type", std::string());
                if (type == "text") {
                    converted_content.push_back(block);
                } else if (type == "thinking") {
                    reasoning_content += json_value(block, "thinking", std::string());
                } else if (type == "image") {
                    const json part = anthropic_image_part(block);
                    if (!part.is_null()) converted_content.push_back(part);
                } else if (type == "tool_use") {
                    tool_calls.push_back({
                        {"id", json_value(block, "id", std::string())},
                        {"type", "function"},
                        {"function", {{"name", json_value(block, "name", std::string())}, {"arguments", json_value(block, "input", json::object()).dump()}}},
                    });
                    has_tool_calls = true;
                } else if (type == "tool_result") {
                    const std::string tool_use_id = json_value(block, "tool_use_id", std::string());
                    const json result_content = json_value(block, "content", json());
                    if (result_content.is_string()) {
                        tool_results.push_back({{"role", "tool"}, {"tool_call_id", tool_use_id}, {"content", result_content.get<std::string>()}});
                    } else if (result_content.is_array()) {
                        std::string result_text;
                        json content_parts = json::array();
                        bool keep_parts = false;
                        for (const auto & c : result_content) {
                            const std::string c_type = json_value(c, "type", std::string());
                            if (c_type == "text") {
                                const std::string text = json_value(c, "text", std::string());
                                result_text += text;
                                content_parts.push_back({{"type", "text"}, {"text", text}});
                            } else if (c_type == "image") {
                                keep_parts = true;
                                const json part = anthropic_image_part(c);
                                if (!part.is_null()) content_parts.push_back(part);
                            } else if (c_type == "tool_reference") {
                                keep_parts = true;
                                content_parts.push_back({{"type", "tool_reference"}, {"name", json_value(c, "tool_name", std::string())}});
                            }
                        }
                        tool_results.push_back({{"role", "tool"}, {"tool_call_id", tool_use_id}, {"content", keep_parts ? content_parts : json(result_text)}});
                    } else {
                        tool_results.push_back({{"role", "tool"}, {"tool_call_id", tool_use_id}, {"content", ""}});
                    }
                }
            }

            if (!converted_content.empty() || has_tool_calls || !reasoning_content.empty()) {
                json new_msg = {{"role", role}};
                if (!converted_content.empty()) {
                    new_msg["content"] = converted_content;
                } else {
                    new_msg["content"] = "";
                }
                if (!tool_calls.empty()) new_msg["tool_calls"] = tool_calls;
                if (!reasoning_content.empty()) new_msg["reasoning_content"] = reasoning_content;
                oai_messages.push_back(new_msg);
            }
            for (const auto & tool_msg : tool_results) {
                oai_messages.push_back(tool_msg);
            }
        }
    }
    oai_body["messages"] = oai_messages;

    if (body.contains("tools") && body.at("tools").is_array()) {
        json oai_tools = json::array();
        for (const auto & tool : body.at("tools")) {
            oai_tools.push_back({
                {"type", "function"},
                {"function", {
                    {"name", json_value(tool, "name", std::string())},
                    {"description", json_value(tool, "description", std::string())},
                    {"parameters", tool.contains("input_schema") ? tool.at("input_schema") : json::object()},
                }},
            });
            if (tool.contains("defer_loading")) {
                oai_tools.back()["defer_loading"] = tool.at("defer_loading");
            }
        }
        oai_body["tools"] = oai_tools;
    }
    if (body.contains("tool_choice") && body.at("tool_choice").is_object()) {
        const json & tc = body.at("tool_choice");
        const std::string type = json_value(tc, "type", std::string());
        if (type == "auto") oai_body["tool_choice"] = "auto";
        else if (type == "any") oai_body["tool_choice"] = "required";
        else if (type == "tool") oai_body["tool_choice"] = json {{"type", "function"}, {"function", {{"name", json_value(tc, "name", std::string())}}}};
        else if (type == "none") oai_body["tool_choice"] = "none";
    }
    if (body.contains("stop_sequences")) {
        oai_body["stop"] = body.at("stop_sequences");
    }
    oai_body["max_tokens"] = body.contains("max_tokens") ? body.at("max_tokens") : json(4096);
    for (const auto & key : {"temperature", "top_p", "top_k", "stream", "chat_template_kwargs", "id_slot"}) {
        if (body.contains(key)) {
            oai_body[key] = body.at(key);
        }
    }
    if (body.contains("thinking")) {
        const json thinking = json_value(body, "thinking", json::object());
        if (json_value(thinking, "type", std::string()) == "enabled") {
            oai_body["thinking_budget_tokens"] = json_value(thinking, "budget_tokens", 10000);
            json kw = json_value(oai_body, "chat_template_kwargs", json::object());
            kw["enable_thinking"] = true;
            oai_body["chat_template_kwargs"] = kw;
        }
    }
    if (body.contains("metadata")) {
        const json metadata = json_value(body, "metadata", json::object());
        const std::string user_id = json_value(metadata, "user_id", std::string());
        if (!user_id.empty()) oai_body["__metadata_user_id"] = user_id;
    }
    if (body.contains("extra_params") && body.at("extra_params").is_object()) {
        for (const auto & [key, value] : body.at("extra_params").items()) {
            oai_body[key] = value;
        }
    }
    return oai_body;
}

//
// transcriptions / speech
//

json convert_transcriptions_to_chatcmpl(const json & inp_body, const std::map<std::string, uploaded_file> & in_files,
                                        std::vector<raw_buffer> & out_files, const std::string & asr_system_prompt) {
    out_files.clear();
    const auto it = in_files.find("file");
    if (it != in_files.end()) {
        out_files.push_back(it->second.data);
    } else if (inp_body.contains("file") && inp_body.at("file").is_string()) {
        // ixian extension: a JSON body with the audio as a data URL or base64
        std::string mime;
        raw_buffer bytes;
        const std::string f = inp_body.at("file").get<std::string>();
        if (string_starts_with(string_lower(f.substr(0, 5)), "data:")) {
            parse_data_url(f, mime, bytes);
        } else {
            bytes = base64_decode(f);
        }
        out_files.push_back(std::move(bytes));
    } else {
        throw std::invalid_argument("No input file found for transcription");
    }

    std::string prompt = json_value(inp_body, "prompt", std::string());
    const std::string language = json_value(inp_body, "language", std::string());
    const std::string response_format = json_value(inp_body, "response_format", std::string("json"));
    if (response_format != "json" && response_format != "text" && response_format != "verbose_json") {
        throw std::invalid_argument("Only 'json', 'text' and 'verbose_json' response_format are supported for transcription");
    }
    const std::string system = asr_system_prompt.empty() ? lfm3_asr_system_prompt(language) : asr_system_prompt;

    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", system}});
    json user_content = json::array();
    user_content.push_back({{"type", "input_audio"}, {"input_audio", {{"data", "__ixian_file_0__"}, {"format", "wav"}}}});
    if (!prompt.empty()) {
        user_content.push_back({{"type", "text"}, {"text", prompt}});
    }
    messages.push_back({{"role", "user"}, {"content", user_content}});

    json chatcmpl_body = inp_body;
    chatcmpl_body["messages"] = messages;
    chatcmpl_body.erase("file");
    chatcmpl_body.erase("prompt");

    // multipart fields are strings: restore the types
    if (inp_body.contains("stream") && inp_body.at("stream").is_string()) {
        chatcmpl_body["stream"] = inp_body.at("stream").get<std::string>() == "true";
    }
    if (inp_body.contains("max_tokens") && inp_body.at("max_tokens").is_string()) {
        chatcmpl_body["max_tokens"] = std::stol(inp_body.at("max_tokens").get<std::string>());
    }
    if (inp_body.contains("temperature") && inp_body.at("temperature").is_string()) {
        chatcmpl_body["temperature"] = std::stof(inp_body.at("temperature").get<std::string>());
    }
    if (!chatcmpl_body.contains("temperature")) {
        chatcmpl_body["temperature"] = 0.0;  // greedy transcription
    }
    chatcmpl_body["__transcription_format"] = response_format;
    return chatcmpl_body;
}

json convert_speech_to_tts(const json & inp_body) {
    json tts_body = inp_body;
    if (inp_body.contains("voice") && inp_body.at("voice").is_object()) {
        tts_body["voice"] = json_value(inp_body.at("voice"), "id", std::string());
    }
    const std::string response_format = json_value(inp_body, "response_format", std::string("wav"));
    if (response_format == "pcm") {
        tts_body["response_format"] = "pcm16";
    } else if (response_format == "wav") {
        tts_body["response_format"] = "wav";
    } else {
        throw std::invalid_argument("Only 'wav' and 'pcm' response_format are supported for speech");
    }
    const std::string stream_format = json_value(inp_body, "stream_format", std::string());
    if (stream_format == "audio") {
        tts_body["stream"] = true;
    } else if (!stream_format.empty()) {
        throw std::invalid_argument("Only 'audio' stream_format is supported for speech");
    }
    if (json_value(inp_body, "speed", 1.0) != 1.0) {
        throw std::invalid_argument("'speed' is not supported for speech");
    }
    if (!json_value(inp_body, "instructions", std::string()).empty()) {
        SRV_WRN("%s", "'instructions' is not supported for speech and is ignored\n");
    }
    return tts_body;
}

}  // namespace ixsrv
