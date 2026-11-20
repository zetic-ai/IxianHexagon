#include "server-http.h"

#include "server-log.h"

#include <cpp-httplib/httplib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_set>

// embedded assets (generated at build time from ui/index.html)
extern const unsigned char ixsrv_asset_index_html[];
extern const size_t ixsrv_asset_index_html_size;

namespace ixsrv {

namespace {

std::string ascii_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

// true if the text is a dotted IPv4 address in 127.0.0.0/8
bool ipv4_is_loopback(const std::string & s) {
    unsigned a, b, c, d;
    char tail;
    return std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a == 127 && b < 256 && c < 256 && d < 256;
}

// loopback: 127.0.0.0/8, ::1 or an IPv4-mapped 127.0.0.0/8
bool addr_is_loopback(const std::string & addr) {
    static const std::string mapped = "::ffff:";
    if (addr.compare(0, mapped.size(), mapped) == 0) {
        return ipv4_is_loopback(addr.substr(mapped.size()));
    }
    return addr == "::1" || ipv4_is_loopback(addr);
}

// the name of a Host header value: lower case, no port, no IPv6 brackets
std::string host_name(const std::string & host) {
    if (!host.empty() && host[0] == '[') {
        const size_t end = host.find(']');
        return end == std::string::npos ? "" : ascii_lower(host.substr(1, end - 1));
    }
    return ascii_lower(host.substr(0, host.find(':')));
}

// DNS rebinding guard: on a loopback connection, Host must name loopback or the host of an allowed origin
bool request_host_is_allowed(const std::string & cors_origins, const httplib::Request & req) {
    if (!addr_is_loopback(req.local_addr)) {
        return true;
    }
    const std::string host = req.get_header_value("Host");
    const std::string name = host_name(host);
    if (name == "localhost" || name == "0.0.0.0" || name == "::" || addr_is_loopback(name)) {
        return true;
    }
    if (cors_origins == "localhost") {
        return false;
    }
    for (const auto & entry : string_split(cors_origins, ',')) {
        const std::string allowed = string_strip(entry);
        const size_t sep = allowed.find("://");
        if (sep != std::string::npos && ascii_lower(allowed.substr(sep + 3)) == ascii_lower(host)) {
            return true;
        }
    }
    return false;
}

// CSRF guard when CORS is not "*": browsers still send simple requests without a preflight
bool request_origin_is_allowed(const std::string & cors_origins, const httplib::Request & req, bool is_frontend_path) {
    if (cors_origins == "*") {
        return true;
    }
    if (!request_host_is_allowed(cors_origins, req)) {
        return false;
    }
    const std::string origin = req.get_header_value("Origin");
    if (!origin.empty()) {
        const std::string host = req.get_header_value("Host");
        const size_t sep = origin.find("://");
        if (!host.empty() && sep != std::string::npos && ascii_lower(origin.substr(sep + 3)) == ascii_lower(host)) {
            return true;  // same-origin
        }
        if (cors_origins == "localhost") {
            return false;
        }
        for (const auto & allowed : string_split(cors_origins, ',')) {
            if (string_strip(allowed) == origin) {
                return true;
            }
        }
        return false;
    }
    // no Origin: a non-browser client, or a browser no-cors GET; cross-site only navigation to the page is allowed
    const std::string site = req.get_header_value("Sec-Fetch-Site");
    if (site == "cross-site" || site == "same-site") {
        return req.method == "GET" && req.get_header_value("Sec-Fetch-Mode") == "navigate" && is_frontend_path;
    }
    return true;
}

std::string json_error_body(int code, const std::string & message, const std::string & type) {
    return safe_json_to_str(json {{"error", {{"message", message}, {"type", type}, {"code", code}}}});
}

std::map<std::string, std::string> get_params(const httplib::Request & req) {
    std::map<std::string, std::string> params;
    for (const auto & [key, value] : req.params) {
        params[key] = value;
    }
    for (const auto & [key, value] : req.path_params) {
        params[key] = value;
    }
    return params;
}

std::map<std::string, std::string> get_headers(const httplib::Request & req) {
    std::map<std::string, std::string> headers;
    for (const auto & [key, value] : req.headers) {
        headers[key] = value;
    }
    return headers;
}

std::string build_query_string(const httplib::Request & req) {
    std::string qs;
    for (const auto & [key, value] : req.params) {
        if (!qs.empty()) qs += '&';
        qs += httplib::encode_query_component(key) + "=" + httplib::encode_query_component(value);
    }
    return qs;
}

void set_headers(httplib::Response & res, const std::map<std::string, std::string> & headers) {
    for (const auto & [key, value] : headers) {
        res.set_header(key, value);
    }
}

const char * sw_remove_js = R"(
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil((async () => {
    await self.registration.unregister();
    for (const key of await caches.keys()) await caches.delete(key);
    for (const c of await self.clients.matchAll({ type: 'window' })) c.navigate(c.url);
})()));
)";

}  // namespace

class http_server::impl {
public:
    std::vector<std::unique_ptr<httplib::Server>> servers;
    std::vector<std::string> hosts;
    std::vector<std::thread> threads;
    std::unique_ptr<httplib::ThreadPool> pool;
    int n_threads_http = 0;
    std::unordered_set<std::string> frontend_paths;
};

class http_task_queue : public httplib::TaskQueue {
    httplib::ThreadPool & pool_;
public:
    explicit http_task_queue(httplib::ThreadPool & pool) : pool_(pool) {}
    bool enqueue(std::function<void()> fn) override { return pool_.enqueue(std::move(fn)); }
    void shutdown() override {}
};

http_server::http_server(const server_params & params) : impl_(std::make_unique<impl>()), params_(params) {}

http_server::~http_server() {
    try {
        stop();
        join();
    } catch (...) {
    }
}

http_handler http_server::guard(http_handler handler) {
    return [handler = std::move(handler)](const http_req & req) -> http_res_ptr {
        error_type type = ERROR_TYPE_SERVER;
        std::string message;
        json extra = json::object();
        try {
            return handler(req);
        } catch (const api_error & e) {
            auto res = std::make_unique<http_res>();
            res->error(e);
            SRV_WRN("request error: %s\n", res->data.c_str());
            return res;
        } catch (const std::invalid_argument & e) {
            type = ERROR_TYPE_INVALID_REQUEST;
            message = e.what();
        } catch (const nlohmann::json::exception & e) {
            type = ERROR_TYPE_INVALID_REQUEST;
            message = e.what();
        } catch (const std::exception & e) {
            type = ERROR_TYPE_SERVER;
            message = e.what();
        } catch (...) {
            message = "unknown error";
        }
        auto res = std::make_unique<http_res>();
        res->error(format_error_response(message, type));
        SRV_WRN("request error: %s\n", res->data.c_str());
        return res;
    };
}

bool http_server::init() {
    port = params_.port;
    impl_->hosts = params_.hostnames;

    size_t n_tcp_hosts = 0;
    for (const auto & host : impl_->hosts) {
        if (!string_ends_with(host, ".sock")) {
            n_tcp_hosts++;
        }
    }
    if (port == 0 && n_tcp_hosts > 1) {
        SRV_ERR("%s", "--port 0 is not supported with multiple TCP addresses\n");
        return false;
    }

    impl_->frontend_paths = {"/", "/index.html", "/sw.js", "/manifest.webmanifest", "/_app/version.json", "/favicon.ico"};

    for (size_t i = 0; i < impl_->hosts.size(); ++i) {
        std::unique_ptr<httplib::Server> srv;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        if (!params_.ssl_file_key.empty() && !params_.ssl_file_cert.empty()) {
            srv = std::make_unique<httplib::SSLServer>(params_.ssl_file_cert.c_str(), params_.ssl_file_key.c_str());
            is_ssl = true;
        } else {
            srv = std::make_unique<httplib::Server>();
        }
#else
        if (!params_.ssl_file_key.empty() && !params_.ssl_file_cert.empty()) {
            SRV_ERR("%s", "the server is built without SSL support (configure with -DIXIAN_SERVER_OPENSSL=ON)\n");
            return false;
        }
        srv = std::make_unique<httplib::Server>();
#endif
        srv->set_default_headers({{"Server", "ixian-server"}});

        srv->set_exception_handler([](const httplib::Request &, httplib::Response & res, const std::exception_ptr & ep) {
            std::string message;
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception & e) {
                message = e.what();
            } catch (...) {
                message = "Unknown Exception";
            }
            res.status = 500;
            res.set_content(json_error_body(500, message, "server_error"), "application/json; charset=utf-8");
            SRV_ERR("got exception: %s\n", message.c_str());
        });

        srv->set_error_handler([](const httplib::Request &, httplib::Response & res) {
            if (res.status == 404) {
                res.set_content(json_error_body(404, "File Not Found", "not_found_error"), "application/json; charset=utf-8");
            }
        });

        srv->set_read_timeout(params_.timeout_read);
        srv->set_write_timeout(params_.timeout_write);
        srv->set_socket_options([reuse_port = params_.reuse_port](const socket_t sock) {
            httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
            if (reuse_port) {
#ifdef SO_REUSEPORT
                httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
            }
        });

        // middleware: CORS, state, API key
        const auto & frontend_paths = impl_->frontend_paths;
        const std::unordered_set<std::string> public_endpoints = [&] {
            std::unordered_set<std::string> s = {params_.api_prefix + "/health", params_.api_prefix + "/v1/health"};
            for (const auto & p : frontend_paths) {
                s.insert(params_.api_prefix + p);
            }
            return s;
        }();

        srv->set_pre_routing_handler([this, public_endpoints](const httplib::Request & req, httplib::Response & res) {
            const auto & p = params_;
            if (p.cors_credentials && p.cors_origins == "*") {
                res.set_header("Access-Control-Allow-Origin", req.get_header_value("Origin"));
            } else if (p.cors_origins == "localhost") {
                // same-origin only, no CORS header
            } else {
                res.set_header("Access-Control-Allow-Origin", p.cors_origins);
            }
            if (req.method == "OPTIONS") {
                res.set_header("Access-Control-Allow-Credentials", p.cors_credentials ? "true" : "false");
                res.set_header("Access-Control-Allow-Methods", p.cors_methods);
                res.set_header("Access-Control-Allow-Headers", p.cors_headers);
                res.set_content("", "text/html");
                return httplib::Server::HandlerResponse::Handled;
            }
            std::string rel_path = req.path;
            if (!p.api_prefix.empty() && string_starts_with(rel_path, p.api_prefix)) {
                rel_path = rel_path.substr(p.api_prefix.size());
            }
            const bool is_frontend = impl_->frontend_paths.count(rel_path) > 0;
            if (!request_origin_is_allowed(p.cors_origins, req, is_frontend)) {
                SRV_WRN("(CORS) rejected cross-origin request: %s %s, host = '%s', origin = '%s'\n",
                        req.method.c_str(), req.path.c_str(), req.get_header_value("Host").c_str(), req.get_header_value("Origin").c_str());
                res.status = 403;
                res.set_content(json_error_body(403, "Cross-origin request not allowed (see --cors-origins)", "permission_error"), "application/json; charset=utf-8");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (!is_ready.load()) {
                if (is_frontend) {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
                res.status = 503;
                res.set_content(json_error_body(503, "Loading model", "unavailable_error"), "application/json; charset=utf-8");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (!p.api_keys.empty() && !public_endpoints.count(req.path)) {
                std::string key = req.get_header_value("Authorization");
                if (key.empty()) {
                    key = req.get_header_value("X-Api-Key");
                }
                static const std::string prefix = "Bearer ";
                if (key.compare(0, prefix.size(), prefix) == 0) {
                    key = key.substr(prefix.size());
                }
                if (std::find(p.api_keys.begin(), p.api_keys.end(), key) == p.api_keys.end()) {
                    res.status = 401;
                    res.set_content(json_error_body(401, "Invalid API Key", "authentication_error"), "application/json; charset=utf-8");
                    SRV_WRN("%s", "unauthorized: Invalid API Key\n");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        srv->new_task_queue = [this] { return new http_task_queue(*impl_->pool); };

        // the web page
        const std::string prefix = params_.api_prefix;
        if (!params_.ui) {
            SRV_INF("%s", "the web page is disabled (--no-ui)\n");
        } else if (!params_.public_path.empty()) {
            if (!srv->set_mount_point(prefix + "/", params_.public_path)) {
                SRV_ERR("static assets path not found: %s\n", params_.public_path.c_str());
                return false;
            }
        } else {
            auto serve_index = [](const httplib::Request &, httplib::Response & res) {
                res.set_header("Cache-Control", "no-cache");
                res.set_content((const char *) ixsrv_asset_index_html, ixsrv_asset_index_html_size, "text/html; charset=utf-8");
            };
            srv->Get(prefix + "/", serve_index);
            srv->Get(prefix + "/index.html", serve_index);
            srv->Get(prefix + "/manifest.webmanifest", [](const httplib::Request &, httplib::Response & res) {
                res.set_header("Cache-Control", "no-cache");
                res.set_content(R"({"name":"ixian-server","short_name":"ixian","start_url":"/","display":"standalone","background_color":"#0f1115","theme_color":"#0f1115","icons":[]})", "application/manifest+json");
            });
            srv->Get(prefix + "/_app/version.json", [](const httplib::Request &, httplib::Response & res) {
                res.set_header("Cache-Control", "no-cache");
                res.set_content(safe_json_to_str(json {{"version", build_info()}}), "application/json");
            });
            srv->Get(prefix + "/favicon.ico", [](const httplib::Request &, httplib::Response & res) {
                res.status = 204;
            });
        }
        // a browser that used another UI on this port keeps its service worker: answer its update with a worker that removes itself
        if (!params_.ui || !params_.public_path.empty() || true) {
            srv->Get(prefix + "/sw.js", [](const httplib::Request &, httplib::Response & res) {
                res.set_header("Cache-Control", "no-cache");
                res.set_content(sw_remove_js, "application/javascript");
            });
        }

        impl_->servers.push_back(std::move(srv));
        if (n_tcp_hosts > 1) {
            impl_->servers.back()->set_ipv6_v6only(true);
        }
    }

    impl_->n_threads_http = params_.n_threads_http;
    if (impl_->n_threads_http < 1) {
        impl_->n_threads_http = std::max(params_.n_parallel + 4, (int) std::thread::hardware_concurrency() - 1);
        impl_->n_threads_http = std::max(impl_->n_threads_http, 8);
    }
    SRV_DBG("using %d threads for the HTTP server\n", impl_->n_threads_http);
    return true;
}

bool http_server::start() {
    listening_addresses.clear();
    for (size_t i = 0; i < impl_->servers.size(); ++i) {
        auto & srv = impl_->servers[i];
        const auto & host = impl_->hosts[i];
        const bool is_sock = string_ends_with(host, ".sock");
        bool was_bound;
        if (is_sock) {
            srv->set_address_family(AF_UNIX);
            was_bound = srv->bind_to_port(host, 8080);
        } else if (port == 0) {
            const int bound = srv->bind_to_any_port(host);
            was_bound = bound >= 0;
            if (was_bound) port = bound;
        } else {
            was_bound = srv->bind_to_port(host, port);
        }
        if (!was_bound) {
            SRV_ERR("couldn't bind HTTP server socket, hostname: %s, port: %d\n", host.c_str(), port);
            stop();
            listening_addresses.clear();
            return false;
        }
        std::string shown = host;
        if (!is_sock && host.find(':') != std::string::npos) {
            shown = "[" + host + "]";
        }
        listening_addresses.push_back(is_sock ? string_format("unix://%s", host.c_str())
                                              : string_format("%s://%s:%d", is_ssl ? "https" : "http", shown.c_str(), port));
    }

    impl_->pool = std::make_unique<httplib::ThreadPool>(impl_->n_threads_http, impl_->n_threads_http + 1024);
    for (size_t i = 0; i < impl_->servers.size(); ++i) {
        auto & srv = impl_->servers[i];
        impl_->threads.emplace_back([srv = srv.get(), addr = listening_addresses[i]] {
            if (!srv->listen_after_bind()) {
                SRV_ERR("listener on %s stopped unexpectedly\n", addr.c_str());
            }
        });
        srv->wait_until_ready();
        if (!srv->is_running()) {
            SRV_ERR("couldn't start HTTP listener on %s\n", listening_addresses[i].c_str());
            stop();
            join();
            listening_addresses.clear();
            return false;
        }
    }
    return true;
}

void http_server::stop() {
    for (const auto & srv : impl_->servers) {
        if (srv) srv->stop();
    }
}

void http_server::join() {
    for (auto & t : impl_->threads) {
        if (t.joinable()) t.join();
    }
    impl_->threads.clear();
    if (impl_->pool) {
        impl_->pool->shutdown();
        impl_->pool.reset();
    }
}

namespace {

using http_req_ptr = std::unique_ptr<http_req>;

void process_handler_response(http_req_ptr && request, http_res_ptr & response, httplib::Response & res) {
    if (response->is_stream()) {
        res.status = response->status;
        response->headers["X-Accel-Buffering"] = "no";  // nginx: do not buffer
        set_headers(res, response->headers);
        const std::string content_type = response->content_type;
        std::shared_ptr<http_req> q_ptr = std::move(request);
        std::shared_ptr<http_res> r_ptr = std::move(response);

        const auto provider = [response = r_ptr](size_t, httplib::DataSink & sink) -> bool {
            std::string chunk;
            const bool has_next = response->next(chunk);
            if (!chunk.empty()) {
                if (!sink.write(chunk.data(), chunk.size())) {
                    return false;
                }
            }
            if (!has_next) {
                sink.done();
            }
            return has_next;
        };
        const auto on_complete = [request = q_ptr, response = r_ptr](bool) mutable {
            if (response->on_complete) response->on_complete();
            response.reset();
            request.reset();
        };
        res.set_chunked_content_provider(content_type, provider, on_complete);
    } else {
        res.status = response->status;
        set_headers(res, response->headers);
        res.set_content(response->data, response->content_type);
        if (response->on_complete) response->on_complete();
    }
}

http_req_ptr make_request(const httplib::Request & req, std::string body, std::map<std::string, uploaded_file> files) {
    auto r = std::make_unique<http_req>();
    r->params = get_params(req);
    r->headers = get_headers(req);
    r->path = req.path;
    r->query_string = build_query_string(req);
    r->body = std::move(body);
    r->files = std::move(files);
    r->should_stop = [closed = req.is_connection_closed]() { return closed && closed(); };
    return r;
}

}  // namespace

void http_server::get(const std::string & path, http_handler handler) {
    auto callback = [handler](const httplib::Request & req, httplib::Response & res) {
        auto request = make_request(req, req.body, {});
        auto response = handler(*request);
        process_handler_response(std::move(request), response, res);
    };
    for (const auto & srv : impl_->servers) {
        srv->Get(params_.api_prefix + path, callback);
    }
}

void http_server::post(const std::string & path, http_handler handler) {
    auto callback = [handler](const httplib::Request & req, httplib::Response & res) {
        std::string body = req.body;
        std::map<std::string, uploaded_file> files;
        if (req.is_multipart_form_data()) {
            // the text fields become a JSON object (repeated keys become arrays), the files are kept apart
            json form_json = json::object();
            for (const auto & [key, field] : req.form.fields) {
                if (form_json.contains(key)) {
                    if (!form_json[key].is_array()) {
                        json existing = form_json[key];
                        form_json[key] = json::array({existing});
                    }
                    form_json[key].push_back(field.content);
                } else {
                    form_json[key] = field.content;
                }
            }
            body = form_json.dump();
            for (const auto & [key, file] : req.form.files) {
                files[key] = uploaded_file{raw_buffer(file.content.begin(), file.content.end()), file.filename, file.content_type};
            }
        }
        auto request = make_request(req, std::move(body), std::move(files));
        auto response = handler(*request);
        process_handler_response(std::move(request), response, res);
    };
    for (const auto & srv : impl_->servers) {
        srv->Post(params_.api_prefix + path, callback);
    }
}

void http_server::del(const std::string & path, http_handler handler) {
    auto callback = [handler](const httplib::Request & req, httplib::Response & res) {
        auto request = make_request(req, req.body, {});
        auto response = handler(*request);
        process_handler_response(std::move(request), response, res);
    };
    for (const auto & srv : impl_->servers) {
        srv->Delete(params_.api_prefix + path, callback);
    }
}

}  // namespace ixsrv
