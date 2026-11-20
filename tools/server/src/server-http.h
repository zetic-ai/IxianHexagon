// Ixian Hexagon server - the HTTP layer (cpp-httplib): listeners, middleware (CORS, API key, loading state), the
// built-in web page, and the adapter between request handlers and streamed (chunked) responses.
//
// The request / response types and the middleware rules follow llama.cpp's tools/server/server-http.* so the same
// clients, the same security expectations and the same tests apply.
#pragma once

#include "server-args.h"
#include "server-utils.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ixsrv {

// a response: either `data` in one piece, or a stream of chunks pulled through `next`
struct http_res {
    std::string content_type = "application/json; charset=utf-8";
    int status = 200;
    std::string data;
    std::map<std::string, std::string> headers;

    // streaming: fills the next chunk; returns false after the last one (the chunk may be empty)
    std::function<bool(std::string &)> next;
    // fired when the response is finished or the connection dropped
    std::function<void()> on_complete;

    bool is_stream() const { return next != nullptr; }

    void ok(const json & body) {
        status = 200;
        data = safe_json_to_str(body);
    }
    void error(const json & error_obj) {
        status = json_value(error_obj, "code", 500);
        data = safe_json_to_str({{"error", error_obj}});
    }
    void error(const api_error & e) { error(e.to_json()); }
    void error(const std::string & message, error_type type) { error(format_error_response(message, type)); }
};
using http_res_ptr = std::unique_ptr<http_res>;

struct uploaded_file {
    raw_buffer data;
    std::string filename;
    std::string content_type;
};

struct http_req {
    std::map<std::string, std::string> params;   // query + path params
    std::map<std::string, std::string> headers;
    std::string path;
    std::string query_string;
    std::string body;                             // JSON body, or the text fields of a multipart form as a JSON object
    std::map<std::string, uploaded_file> files;   // multipart file fields
    std::function<bool()> should_stop;            // true once the client is gone

    std::string get_param(const std::string & key, const std::string & def = "") const {
        auto it = params.find(key);
        return it != params.end() ? it->second : def;
    }
    std::string get_header(const std::string & key) const {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : std::string();
    }
    bool stopped() const { return should_stop && should_stop(); }
};

using http_handler = std::function<http_res_ptr(const http_req &)>;

class http_server {
public:
    explicit http_server(const server_params & params);
    ~http_server();

    bool init();   // listeners and middleware; false on a configuration error (logged)
    bool start();  // bind and listen; false when a port cannot be bound
    void stop();
    void join();

    void get (const std::string & path, http_handler handler);
    void post(const std::string & path, http_handler handler);
    void del (const std::string & path, http_handler handler);

    // handlers are refused with 503 "Loading model" until this is set
    std::atomic<bool> is_ready{false};

    std::vector<std::string> listening_addresses;
    int  port   = 0;
    bool is_ssl = false;

    // wraps a handler so it never throws: api_error / invalid_argument / json errors -> 400 family, else 500
    static http_handler guard(http_handler handler);

private:
    class impl;
    std::unique_ptr<impl> impl_;
    server_params params_;
};

}  // namespace ixsrv
