// Ixian Hexagon server - the engine: one SDK object (LLM or Decision), one inference thread, a FIFO of tasks.
//
// The NPU runtime serves one request at a time (one active model per process, one KV cache), so the server has one
// "slot": HTTP threads post tasks, the inference thread runs them in order and streams their output back through a
// task_channel. Helpers that need no DSP work (tokenize, detokenize, applyChatTemplate, inspect) run on the calling
// thread under their own mutex, or on the inference thread when --serialize-helpers is given.
#pragma once

#include "server-args.h"
#include "server-utils.h"

#include <zetic_decision.h>
#include <zetic_llm.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ixsrv {

namespace zx = zetic::ixian;

//
// task_channel: the pipe between a request handler (consumer, HTTP thread) and its task (producer, inference thread)
//
class task_channel {
public:
    enum class state {
        pending,    // nothing happened yet
        streaming,  // at least one chunk was pushed
        final,      // set_final() was called (non-stream result)
        error,      // set_error() was called before any chunk
    };

    // producer side
    bool push(std::string chunk);              // false when the consumer is gone (stop producing)
    void set_final(json result);
    void set_error(const api_error & err);
    void close();                              // no more chunks
    bool consumer_gone() const { return consumer_gone_.load(std::memory_order_relaxed); }

    // consumer side
    // blocks until the producer pushed a chunk, a final result, an error, or closed; or until should_stop() is true
    // (polled every 100 ms); returns the state at that time
    state wait_started(const std::function<bool()> & should_stop);
    // next chunk; returns false on timeout (out is empty) or when the channel is closed and drained (check closed())
    bool next_chunk(std::string & out, int timeout_ms);
    bool closed_and_drained();
    bool has_error() const;
    const api_error & error() const { return error_; }
    const json & final() const { return final_; }
    void mark_consumer_gone() { consumer_gone_.store(true, std::memory_order_relaxed); }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> chunks_;
    bool closed_  = false;
    bool started_ = false;
    bool has_final_ = false;
    bool has_error_ = false;
    json final_;
    api_error error_{ERROR_TYPE_SERVER, ""};
    std::atomic<bool> consumer_gone_{false};
};

//
// metrics, cumulative since start (GET /metrics) - the same names and meaning as llama-server's
//
struct engine_metrics {
    int64_t t_start = 0;  // us

    struct bucket {
        uint64_t count = 0;  // tokens
        uint64_t steps = 0;  // decode steps (the first generated token is free: it comes from the prefill)
        uint64_t time  = 0;  // us
        double n_per_second() const { return time > 0 ? (double) steps / (double) time * 1e6 : 0.0; }
        void add(uint64_t n, uint64_t n_steps, uint64_t t_us) { count += n; steps += n_steps; time += t_us; }
    };

    bucket prompt_bucket, predict_bucket;  // reset at every scrape, give the rates
    bucket prompt, predict;                // cumulative

    uint64_t n_prompt_cached = 0;
    uint64_t n_tokens_max    = 0;
    uint64_t n_decode        = 0;
    uint64_t n_busy_slots    = 0;
    uint64_t n_requests      = 0;
    uint64_t n_errors        = 0;
    uint64_t n_audio_frames  = 0;   // speech frames produced (ixian)
    uint64_t n_decisions     = 0;   // d1 questions answered (ixian)

    void reset_bucket() { prompt_bucket = {}; predict_bucket = {}; }
};

// what the task reports when it finishes (also feeds the metrics)
struct task_stats {
    int32_t n_prompt_tokens        = 0;  // rows the request occupies
    int32_t n_prompt_tokens_cache  = 0;  // of which reused from the prefix cache
    int32_t n_decoded              = 0;  // generated tokens
    int32_t n_decode_steps         = 0;
    int64_t t_prompt_us            = 0;  // prompt processing (tokenize + template + encode + prefill)
    int64_t t_gen_us               = 0;  // first token to last token
    int32_t n_audio_frames         = 0;
    int32_t n_decisions            = 0;
};

//
// engine
//
class engine {
public:
    explicit engine(const server_params & params);
    ~engine();

    engine(const engine &) = delete;
    engine & operator=(const engine &) = delete;

    // loads the package (LLM or Decision) and starts the inference thread; throws std::runtime_error with the message to log
    void load();
    void shutdown();  // stops the inference thread (cancels the running task); the destructor calls it too

    bool is_loaded()   const { return loaded_; }
    bool is_decision() const { return decision_ != nullptr; }
    bool has_llm()     const { return llm_ != nullptr; }

    // the SDK objects: only from the inference thread (inside a task)
    zx::LLM &      llm()      { return *llm_; }
    zx::Decision & decision() { return *decision_; }

    const zx::ModelInfo &    info() const { return info_; }
    const zx::Capabilities & caps() const { return caps_; }
    const server_params &    params() const { return params_; }
    const std::string &      model_name() const { return model_name_; }  // id of GET /models
    int32_t                  n_ctx() const { return caps_.max_context; }

    // helpers (thread-safe; see the file comment)
    std::vector<int32_t> tokenize(std::string_view text, bool parse_special);
    std::string          detokenize(const std::vector<int32_t> & ids);
    std::string          token_piece(int32_t id);
    std::string          apply_template(const std::vector<zx::Message> & msgs, bool add_generation_prompt, bool enable_thinking);
    zx::PromptInfo       inspect(const std::vector<zx::Message> & msgs, const zx::GenerationConfig & cfg);
    int32_t              bos_token();  // -1 when unknown
    const std::string &  bos_str() const { return bos_str_; }
    const std::string &  eos_str() const { return eos_str_; }

    //
    // tasks
    //
    struct task;
    using task_fn = std::function<void(engine & eng, task & t)>;

    struct task {
        int                           id = -1;
        task_fn                       fn;
        std::shared_ptr<task_channel> channel;  // may be null for fire-and-forget tasks
        json                          params;   // generation settings shown by GET /slots
        std::string                   prompt;   // the rendered prompt shown by GET /slots (verbose)
        int64_t                       t_queued = 0;
        std::atomic<bool>             cancelled{false};
        task_stats                    stats;
    };

    // posts a task; returns its id. high_priority tasks (metrics, slot erase) go to the front of the queue.
    int post(task_fn fn, std::shared_ptr<task_channel> channel, json slot_params = json::object(), std::string prompt = "", bool high_priority = false);
    // cancels a queued task, or stops the running one at its next step
    void cancel_task(int id);
    // runs fn on the inference thread and waits for it (slot erase, serialized helpers)
    void run_sync(const std::function<void(engine &)> & fn, bool high_priority = true);

    // progress of the running task, reported by the task function itself
    void slot_begin(const task & t, int32_t n_prompt_tokens);
    void slot_progress(int32_t n_decoded, int32_t n_remain, bool has_new_line, const std::string & generated_tail);
    void slot_end(const task & t);
    void record_stats(const task_stats & stats);  // feeds the metrics (call once per request)

    // GET /slots entry (there is one slot, id 0)
    json slot_json(bool only_metrics) const;
    bool is_processing() const { return processing_.load(std::memory_order_relaxed); }
    int  n_queued() const;
    engine_metrics metrics(bool reset_bucket);

    // POST /slots/0?action=erase: drops the KV cache and the prefix cache (runs on the inference thread)
    void erase_cache();

    // remember the assistant messages the SDK returned so a re-sent history reuses the exact cached ids
    void remember_reply(const zx::Message & msg);
    bool recall_reply(const std::string & text, zx::Message & out) const;

private:
    void worker_loop();
    void load_llm();
    void load_decision();
    void sdk_log(zx::LogLevel level, std::string_view msg);
    template <typename F> auto helper(F && f) -> decltype(f());

    server_params params_;
    std::unique_ptr<zx::LLM>      llm_;
    std::unique_ptr<zx::Decision> decision_;
    zx::ModelInfo    info_;
    zx::Capabilities caps_;
    std::string      model_name_;
    std::string      bos_str_ = "<|startoftext|>";
    std::string      eos_str_ = "<|im_end|>";
    int32_t          bos_token_ = -2;  // -2 = not resolved yet
    bool             loaded_ = false;

    // queue
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::shared_ptr<task>> queue_;
    std::shared_ptr<task> running_;
    std::atomic<bool> processing_{false};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    int next_task_id_ = 0;

    // helpers
    std::mutex helper_mutex_;

    // slot state (GET /slots)
    mutable std::mutex slot_mutex_;
    struct {
        int id_task = -1;
        int32_t n_prompt_tokens = 0;
        int32_t n_decoded = 0;
        int32_t n_remain = -1;
        bool has_new_line = false;
        bool has_next_token = false;
        json params = json::object();
        std::string prompt;
        std::string generated;
        task_stats last_stats;
    } slot_;

    // metrics
    mutable std::mutex metrics_mutex_;
    engine_metrics metrics_;

    // generated-record cache: text -> message (most recent first), bounded
    mutable std::mutex replies_mutex_;
    std::list<zx::Message> replies_;
    static constexpr size_t kMaxReplies = 64;
};

// the SDK status of a Response / DecisionResult as an API error
api_error status_to_api_error(zx::Status status, const std::string & message, int32_t n_prompt_tokens = 0, int32_t n_ctx = 0);
const char * finish_reason_oai(zx::FinishReason r);  // "stop" | "length" | ...

}  // namespace ixsrv
