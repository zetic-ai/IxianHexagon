#include "server-engine.h"

#include "server-log.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <type_traits>

namespace ixsrv {

//
// task_channel
//

bool task_channel::push(std::string chunk) {
    if (consumer_gone()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.push_back(std::move(chunk));
    started_ = true;
    cv_.notify_all();
    return true;
}

void task_channel::set_final(json result) {
    std::lock_guard<std::mutex> lock(mutex_);
    final_ = std::move(result);
    has_final_ = true;
    started_ = true;
    closed_ = true;
    cv_.notify_all();
}

void task_channel::set_error(const api_error & err) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = err;
    has_error_ = true;
    started_ = true;
    closed_ = true;
    cv_.notify_all();
}

void task_channel::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
}

task_channel::state task_channel::wait_started(const std::function<bool()> & should_stop) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!started_ && !closed_) {
        cv_.wait_for(lock, std::chrono::milliseconds(100));
        if (!started_ && !closed_ && should_stop && should_stop()) {
            consumer_gone_.store(true, std::memory_order_relaxed);
            break;
        }
    }
    if (has_error_)   return state::error;
    if (has_final_)   return state::final;
    if (!chunks_.empty() || started_) return state::streaming;
    return state::pending;
}

bool task_channel::next_chunk(std::string & out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (chunks_.empty() && !closed_) {
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    }
    if (chunks_.empty()) {
        out.clear();
        return false;
    }
    out = std::move(chunks_.front());
    chunks_.pop_front();
    return true;
}

bool task_channel::closed_and_drained() {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_ && chunks_.empty();
}

bool task_channel::has_error() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(mutex_));
    return has_error_;
}

//
// status helpers
//

api_error status_to_api_error(zx::Status status, const std::string & message, int32_t n_prompt_tokens, int32_t n_ctx) {
    const std::string prefix = std::string(zx::statusName(status)) + ": ";
    const std::string msg = string_starts_with(message, prefix) ? message : prefix + message;
    switch (status) {
        case zx::Status::Ok:              return api_error(ERROR_TYPE_SERVER, "unexpected Ok status");
        case zx::Status::InvalidArgument: return api_error(ERROR_TYPE_INVALID_REQUEST, msg);
        case zx::Status::Unsupported:     return api_error(ERROR_TYPE_NOT_SUPPORTED, msg);
        case zx::Status::ContextExceeded: return api_error(ERROR_TYPE_EXCEED_CONTEXT_SIZE, msg, json {{"n_prompt_tokens", n_prompt_tokens}, {"n_ctx", n_ctx}});
        case zx::Status::Busy:            return api_error(ERROR_TYPE_UNAVAILABLE, msg);
        case zx::Status::PackageError:    return api_error(ERROR_TYPE_SERVER, msg);
        case zx::Status::DeviceError:     return api_error(ERROR_TYPE_SERVER, msg);
        case zx::Status::Internal:        return api_error(ERROR_TYPE_SERVER, msg);
    }
    return api_error(ERROR_TYPE_SERVER, msg);
}

const char * finish_reason_oai(zx::FinishReason r) {
    switch (r) {
        case zx::FinishReason::Stop:         return "stop";
        case zx::FinishReason::StopSequence: return "stop";
        case zx::FinishReason::Length:       return "length";
        case zx::FinishReason::ContextFull:  return "length";
        case zx::FinishReason::Cancelled:    return "stop";
        case zx::FinishReason::Error:        return "stop";
    }
    return "stop";
}

//
// engine
//

engine::engine(const server_params & params) : params_(params) {
    metrics_.t_start = time_us();
}

engine::~engine() {
    shutdown();
}

void engine::sdk_log(zx::LogLevel level, std::string_view msg) {
    switch (level) {
        case zx::LogLevel::Debug: LOG_DBG("sdk: %.*s\n", (int) msg.size(), msg.data()); break;
        case zx::LogLevel::Info:  LOG_INF("sdk: %.*s\n", (int) msg.size(), msg.data()); break;
        case zx::LogLevel::Warn:  LOG_WRN("sdk: %.*s\n", (int) msg.size(), msg.data()); break;
        case zx::LogLevel::Error: LOG_ERR("sdk: %.*s\n", (int) msg.size(), msg.data()); break;
    }
}

void engine::load_llm() {
    zx::LLMOptions opt;
    opt.max_context      = params_.n_ctx > 0 ? params_.n_ctx : 0;
    opt.max_new_tokens   = params_.n_predict > 0 ? params_.n_predict : 256;
    opt.temperature      = params_.temperature;
    opt.native_lib_dir   = params_.native_lib_dir;
    opt.target           = params_.target;
    opt.verify_checksums = params_.verify_checksums;
    opt.warmup           = params_.warmup;
    opt.host_affinity    = params_.host_affinity;
    opt.wait_if_busy     = true;
    opt.log              = [this](zx::LogLevel level, std::string_view msg) { sdk_log(level, msg); };
    llm_ = std::make_unique<zx::LLM>(zx::LLM::fromPath(params_.model_path, opt));
    info_ = llm_->info();
    caps_ = llm_->capabilities();
}

void engine::load_decision() {
    zx::DecisionOptions opt;
    opt.native_lib_dir   = params_.native_lib_dir;
    opt.target           = params_.target;
    opt.verify_checksums = params_.verify_checksums;
    opt.warmup           = params_.warmup;
    opt.host_affinity    = params_.host_affinity;
    opt.log              = [this](zx::LogLevel level, std::string_view msg) { sdk_log(level, msg); };
    decision_ = std::make_unique<zx::Decision>(zx::Decision::fromPath(params_.model_path, opt));
    info_ = decision_->info();
    caps_ = decision_->capabilities();
}

void engine::load() {
    const int64_t t0 = time_ms();
    try {
        if (params_.model_type == "decision") {
            load_decision();
        } else if (params_.model_type == "llm") {
            load_llm();
        } else {
            try {
                load_llm();
            } catch (const zx::Error & e) {
                const std::string what = e.what();
                if (e.status() == zx::Status::Unsupported && what.find("Decision::fromPath") != std::string::npos) {
                    SRV_INF("%s", "the package is a d1 decision model, loading it as Decision\n");
                    load_decision();
                } else {
                    throw;
                }
            }
        }
    } catch (const zx::Error & e) {
        throw std::runtime_error(string_format("cannot load %s: %s (%s%s)", params_.model_path.c_str(), e.what(),
                                               zx::statusName(e.status()), e.dspError() ? string_format(", dsp 0x%x", e.dspError()).c_str() : ""));
    } catch (const std::exception & e) {
        throw std::runtime_error(string_format("cannot load %s: %s", params_.model_path.c_str(), e.what()));
    }

    if (!params_.model_alias.empty()) {
        model_name_ = *params_.model_alias.begin();
    } else {
        // the directory name of the model root, like llama-server names a model after its file
        std::filesystem::path p(params_.model_path);
        while (!p.empty() && p.filename().empty()) {
            p = p.parent_path();
        }
        std::string name = p.filename().string();
        if (name == info_.target || name == "v73" || name == "v73-1nsp") {
            name = p.parent_path().filename().string();
        }
        model_name_ = name.empty() ? info_.model_id : name;
        params_.model_alias.insert(model_name_);
    }

    loaded_ = true;
    stop_.store(false);
    worker_ = std::thread([this] { worker_loop(); });

    SRV_INF("loaded %s (%s, %s, v%d, %d NSP) as '%s' in %.1f s: text %d image %d audio %d video %d speech %d decision %d, max_context %d\n",
            info_.model_id.c_str(), info_.target.c_str(), info_.soc.c_str(), info_.hexagon_arch, info_.nsp_count,
            model_name_.c_str(), (time_ms() - t0) / 1000.0,
            caps_.text, caps_.image, caps_.audio, caps_.video, caps_.speech_out, caps_.decision, caps_.max_context);
}

void engine::shutdown() {
    if (!worker_.joinable()) {
        return;
    }
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (running_) {
            running_->cancelled.store(true);
        }
        queue_cv_.notify_all();
    }
    if (llm_) {
        llm_->cancel();
    }
    if (decision_) {
        decision_->cancel();
    }
    worker_.join();
}

//
// helpers
//

template <typename F>
auto engine::helper(F && f) -> decltype(f()) {
    if (params_.helpers_concurrent || std::this_thread::get_id() == worker_.get_id()) {
        std::lock_guard<std::mutex> lock(helper_mutex_);
        return f();
    }
    // serialized: run on the inference thread, between tasks
    using R = decltype(f());
    if constexpr (std::is_void_v<R>) {
        run_sync([&](engine &) { f(); });
    } else {
        R result{};
        std::exception_ptr ex;
        run_sync([&](engine &) {
            try {
                result = f();
            } catch (...) {
                ex = std::current_exception();
            }
        });
        if (ex) std::rethrow_exception(ex);
        return result;
    }
}

std::vector<int32_t> engine::tokenize(std::string_view text, bool parse_special) {
    if (!llm_) throw api_error(ERROR_TYPE_NOT_SUPPORTED, "this model has no tokenizer API (d1 decision package)");
    return helper([&] { return llm_->tokenize(text, parse_special); });
}

std::string engine::detokenize(const std::vector<int32_t> & ids) {
    if (!llm_) throw api_error(ERROR_TYPE_NOT_SUPPORTED, "this model has no tokenizer API (d1 decision package)");
    return helper([&] { return llm_->detokenize(ids); });
}

std::string engine::token_piece(int32_t id) {
    return detokenize({id});
}

std::string engine::apply_template(const std::vector<zx::Message> & msgs, bool add_generation_prompt, bool enable_thinking) {
    if (!llm_) throw api_error(ERROR_TYPE_NOT_SUPPORTED, "this model has no chat template (d1 decision package)");
    zx::TemplateOptions opt;
    opt.add_generation_prompt = add_generation_prompt;
    opt.enable_thinking = enable_thinking;
    return helper([&] { return llm_->applyChatTemplate(msgs, opt); });
}

zx::PromptInfo engine::inspect(const std::vector<zx::Message> & msgs, const zx::GenerationConfig & cfg) {
    if (!llm_) throw api_error(ERROR_TYPE_NOT_SUPPORTED, "this model has no prompt inspection (d1 decision package)");
    // inspect reads the prefix cache that chat() updates: it runs between tasks on the inference thread
    if (std::this_thread::get_id() == worker_.get_id()) {
        return llm_->inspect(msgs, cfg);
    }
    zx::PromptInfo result;
    std::exception_ptr ex;
    run_sync([&](engine &) {
        try {
            result = llm_->inspect(msgs, cfg);
        } catch (...) {
            ex = std::current_exception();
        }
    });
    if (ex) std::rethrow_exception(ex);
    return result;
}

int32_t engine::bos_token() {
    if (bos_token_ == -2) {
        int32_t id = -1;
        try {
            const auto ids = tokenize(bos_str_, true);
            if (ids.size() == 1) id = ids[0];
        } catch (...) {
        }
        bos_token_ = id;
    }
    return bos_token_;
}

//
// tasks
//

int engine::post(task_fn fn, std::shared_ptr<task_channel> channel, json slot_params, std::string prompt, bool high_priority) {
    auto t = std::make_shared<task>();
    t->fn = std::move(fn);
    t->channel = std::move(channel);
    t->params = std::move(slot_params);
    t->prompt = std::move(prompt);
    t->t_queued = time_us();
    int id;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        id = next_task_id_++;
        t->id = id;
        if (high_priority) {
            queue_.push_front(std::move(t));
        } else {
            queue_.push_back(std::move(t));
        }
        queue_cv_.notify_all();
    }
    return id;
}

void engine::cancel_task(int id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (auto & t : queue_) {
        if (t->id == id) {
            t->cancelled.store(true);
        }
    }
    if (running_ && running_->id == id) {
        running_->cancelled.store(true);
        if (llm_) llm_->cancel();
        if (decision_) decision_->cancel();
    }
}

void engine::run_sync(const std::function<void(engine &)> & fn, bool high_priority) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr ex;
    post([&](engine & eng, task &) {
        try {
            fn(eng);
        } catch (...) {
            ex = std::current_exception();
        }
        std::lock_guard<std::mutex> lock(m);
        done = true;
        cv.notify_all();
    }, nullptr, json::object(), "", high_priority);
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return done; });
    if (ex) std::rethrow_exception(ex);
}

void engine::worker_loop() {
    while (true) {
        std::shared_ptr<task> t;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [&] { return stop_.load() || !queue_.empty(); });
            if (stop_.load()) {
                // drop the queue: tell the waiting handlers
                for (auto & q : queue_) {
                    if (q->channel) q->channel->set_error(api_error(ERROR_TYPE_UNAVAILABLE, "server is shutting down"));
                }
                queue_.clear();
                return;
            }
            t = queue_.front();
            queue_.pop_front();
            running_ = t;
            processing_.store(true);
        }

        if (t->cancelled.load() || (t->channel && t->channel->consumer_gone())) {
            SRV_DBG("task %d skipped: cancelled before it started\n", t->id);
            if (t->channel) t->channel->close();
        } else {
            SRV_DBG("task %d starts (waited %.1f ms)\n", t->id, (time_us() - t->t_queued) / 1000.0);
            try {
                t->fn(*this, *t);
            } catch (const api_error & e) {
                SRV_WRN("task %d failed: %s\n", t->id, e.what());
                if (t->channel) t->channel->set_error(e);
            } catch (const zx::Error & e) {
                SRV_ERR("task %d failed in the SDK: %s\n", t->id, e.what());
                if (t->channel) t->channel->set_error(status_to_api_error(e.status(), e.what()));
            } catch (const std::exception & e) {
                SRV_ERR("task %d failed: %s\n", t->id, e.what());
                if (t->channel) t->channel->set_error(api_error(ERROR_TYPE_SERVER, e.what()));
            }
            if (t->channel) t->channel->close();
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            running_.reset();
            processing_.store(false);
        }
    }
}

//
// slot state and metrics
//

void engine::slot_begin(const task & t, int32_t n_prompt_tokens) {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    slot_.id_task = t.id;
    slot_.n_prompt_tokens = n_prompt_tokens;
    slot_.n_decoded = 0;
    slot_.n_remain = json_value(t.params, "n_predict", -1);
    slot_.has_new_line = false;
    slot_.has_next_token = true;
    slot_.params = t.params;
    slot_.prompt = t.prompt;
    slot_.generated.clear();
}

void engine::slot_progress(int32_t n_decoded, int32_t n_remain, bool has_new_line, const std::string & generated_tail) {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    slot_.n_decoded = n_decoded;
    slot_.n_remain = n_remain;
    slot_.has_new_line = has_new_line;
    slot_.generated += generated_tail;
    if (slot_.generated.size() > 4096) {
        slot_.generated.erase(0, slot_.generated.size() - 4096);
    }
}

void engine::slot_end(const task & t) {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    slot_.has_next_token = false;
    slot_.n_remain = -1;
    slot_.n_decoded = 0;
    slot_.last_stats = t.stats;
}

void engine::record_stats(const task_stats & s) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    const uint64_t n_processed = (uint64_t) std::max(0, s.n_prompt_tokens - s.n_prompt_tokens_cache);
    metrics_.prompt.add(n_processed, n_processed, (uint64_t) std::max<int64_t>(0, s.t_prompt_us));
    metrics_.prompt_bucket.add(n_processed, n_processed, (uint64_t) std::max<int64_t>(0, s.t_prompt_us));
    metrics_.n_prompt_cached += (uint64_t) std::max(0, s.n_prompt_tokens_cache);
    const uint64_t n_gen = (uint64_t) std::max(0, s.n_decoded);
    const uint64_t n_steps = (uint64_t) std::max(0, s.n_decode_steps);
    metrics_.predict.add(n_gen, n_steps, (uint64_t) std::max<int64_t>(0, s.t_gen_us));
    metrics_.predict_bucket.add(n_gen, n_steps, (uint64_t) std::max<int64_t>(0, s.t_gen_us));
    metrics_.n_tokens_max = std::max<uint64_t>(metrics_.n_tokens_max, (uint64_t) std::max(0, s.n_prompt_tokens + s.n_decoded));
    // one decode call for the prefill (when something was processed) plus one per generation step
    metrics_.n_decode += (n_processed > 0 ? 1 : 0) + n_steps;
    metrics_.n_busy_slots += (n_processed > 0 ? 1 : 0) + n_steps;
    metrics_.n_requests += 1;
    metrics_.n_audio_frames += (uint64_t) std::max(0, s.n_audio_frames);
    metrics_.n_decisions += (uint64_t) std::max(0, s.n_decisions);
}

json engine::slot_json(bool only_metrics) const {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    json res = {
        {"id",            0},
        {"n_ctx",         caps_.max_context},
        {"speculative",   false},
        {"is_processing", is_processing()},
    };
    if (slot_.id_task >= 0) {
        res["id_task"] = slot_.id_task;
        res["n_prompt_tokens"] = slot_.n_prompt_tokens;
        res["n_prompt_tokens_processed"] = std::max(0, slot_.last_stats.n_prompt_tokens - slot_.last_stats.n_prompt_tokens_cache);
        res["n_prompt_tokens_cache"] = slot_.last_stats.n_prompt_tokens_cache;
        res["params"] = slot_.params;
        res["next_token"] = json::array({
            {
                {"has_next_token", slot_.has_next_token},
                {"has_new_line",   slot_.has_new_line},
                {"n_remain",       slot_.n_remain},
                {"n_decoded",      slot_.n_decoded},
            }
        });
        if (!only_metrics) {
            res["prompt"] = slot_.prompt;
            res["generated"] = slot_.generated;
        }
    }
    return res;
}

int engine::n_queued() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return (int) queue_.size();
}

engine_metrics engine::metrics(bool reset_bucket) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    engine_metrics m = metrics_;
    if (reset_bucket) {
        metrics_.reset_bucket();
    }
    return m;
}

void engine::erase_cache() {
    run_sync([](engine & eng) {
        if (eng.has_llm()) {
            eng.llm().reset();
        }
    });
    std::lock_guard<std::mutex> lock(replies_mutex_);
    replies_.clear();
}

void engine::remember_reply(const zx::Message & msg) {
    if (!msg.generated) {
        return;
    }
    std::lock_guard<std::mutex> lock(replies_mutex_);
    replies_.push_front(msg);
    while (replies_.size() > kMaxReplies) {
        replies_.pop_back();
    }
}

bool engine::recall_reply(const std::string & text, zx::Message & out) const {
    if (text.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(replies_mutex_);
    for (const auto & m : replies_) {
        if (m.text() == text) {
            out = m;
            return true;
        }
    }
    return false;
}

}  // namespace ixsrv
