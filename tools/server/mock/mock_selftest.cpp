// mock_selftest.cpp - exercises ixian_mock.cpp against README.md. No test framework: CHECK counts, non-zero exit on
// any failure. Run from tools/server/mock (or pass the models directory as argv[1]).
#include <zetic_decision.h>
#include <zetic_llm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace zetic::ixian;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

static int g_passed = 0, g_failed = 0;

static std::string show(const std::string& s) { return "\"" + s + "\""; }
static std::string show(const char* s) { return show(std::string(s)); }
static std::string show(Status s) { return statusName(s); }
static std::string show(FinishReason r) { return finishReasonName(r); }
template <class T>
static std::enable_if_t<std::is_arithmetic_v<T>, std::string> show(T v) {
  return std::to_string(v);
}
template <class T>
static std::string show(const std::vector<T>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + show(v[i]);
  return s + "]";
}

#define FAIL(msg) \
  do { \
    ++g_failed; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, std::string(msg).c_str()); \
  } while (0)
#define CHECK(cond) \
  do { \
    if (cond) ++g_passed; \
    else FAIL(#cond); \
  } while (0)
#define CHECK_EQ(a, b) \
  do { \
    const auto _a = (a); \
    const auto _b = (b); \
    if (_a == _b) ++g_passed; \
    else FAIL(std::string(#a " == " #b) + "\n  left:  " + show(_a) + "\n  right: " + show(_b)); \
  } while (0)

static std::string g_models;
static std::string model(const char* name) { return g_models + "/" + name; }

template <class F>
static void expectError(F&& f, Status status, const std::string& what = "", int32_t dsp = 0) {
  try {
    f();
    FAIL(std::string("no Error thrown, expected ") + statusName(status));
  } catch (const Error& e) {
    CHECK_EQ(e.status(), status);
    if (!what.empty()) CHECK_EQ(std::string(e.what()), what);
    CHECK_EQ(e.dspError(), dsp);
  }
}

static Image makeImage(int w, int h, uint8_t fill = 0x80) {
  std::vector<uint8_t> rgb(size_t(w) * h * 3, fill);
  return Image::fromRgb(rgb.data(), w, h);
}

static Audio makeAudio(double seconds) {
  Audio a;
  a.sample_rate = 16000;
  a.pcm.assign(static_cast<size_t>(seconds * 16000), 0.1f);
  return a;
}

static bool contains(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

// ---------------------------------------------------------------------------------------------------------------

static void testLoad() {
  LLM text = LLM::fromPath(model("lfm3-1.7b"));
  CHECK_EQ(text.info().model_id, "LiquidAI/LFM3-1.7B");
  CHECK_EQ(text.info().revision, "mock");
  CHECK_EQ(text.info().target, "v73");
  CHECK_EQ(text.info().nsp_count, 2);
  CHECK_EQ(text.info().hexagon_arch, 73);
  CHECK_EQ(text.info().soc, "QCS9075");
  CHECK_EQ(text.info().package_dir, model("lfm3-1.7b") + "/v73");
  CHECK_EQ(text.info().package_version, "0.2.0");
  CHECK_EQ(text.info().sdk_version, ZETIC_IXIAN_HEXAGON_VERSION);
  CHECK_EQ(text.info().runtime_commit, "mock");
  Capabilities c = text.capabilities();
  CHECK(c.text && !c.image && !c.audio && !c.video && !c.speech_out && !c.decision);
  CHECK(c.multi_turn && c.media_in_followup_turns);
  CHECK_EQ(c.max_media_per_turn, -1);
  CHECK_EQ(c.max_context, 2048);
  CHECK_EQ(c.input_sample_rate, 0);
  CHECK_EQ(c.speech_sample_rate, 0);

  LLM vl = LLM::fromPath(model("lfm3-vl-2b"));
  c = vl.capabilities();
  CHECK(c.image && c.video && !c.audio && !c.speech_out);
  CHECK_EQ(vl.info().model_id, "LiquidAI/LFM3-VL-2B");

  LLM au = LLM::fromPath(model("lfm3-audio-2b"));
  c = au.capabilities();
  CHECK(c.audio && c.speech_out && !c.image && !c.video);
  CHECK_EQ(c.input_sample_rate, 16000);
  CHECK_EQ(c.speech_sample_rate, 24000);

  LLM::Options one;
  one.target = "v73-1nsp";
  one.max_context = 4096;
  LLM omni1 = LLM::fromPath(model("lfm3-omni-2b"), one);
  CHECK_EQ(omni1.info().target, "v73-1nsp");
  CHECK_EQ(omni1.info().nsp_count, 1);
  CHECK_EQ(omni1.info().package_dir, model("lfm3-omni-2b") + "/v73-1nsp");
  CHECK_EQ(omni1.capabilities().max_context, 4096);
  c = omni1.capabilities();
  CHECK(c.text && c.image && c.audio && c.video && c.speech_out);

  Decision d1 = Decision::fromPath(model("d1-2b"));
  CHECK_EQ(d1.info().model_id, "LiquidAI/d1-2B");
  CHECK_EQ(d1.info().nsp_count, 1);
  c = d1.capabilities();
  CHECK(c.decision && c.text && c.image && !c.audio && !c.video);
  CHECK_EQ(c.max_context, 2048);
  Decision d1o = Decision::fromPath(model("d1-omni-2b"));
  CHECK(d1o.capabilities().audio && d1o.capabilities().image);
  CHECK_EQ(d1o.capabilities().input_sample_rate, 16000);

  // the log line
  std::string logged;
  LLM::Options with_log;
  with_log.log = [&](LogLevel level, std::string_view line) {
    if (level == LogLevel::Info) logged = std::string(line);
  };
  LLM logged_llm = LLM::fromPath(model("lfm3-1.7b"), with_log);
  CHECK_EQ(logged, "mock: loaded LiquidAI/LFM3-1.7B (v73, 2 NSP)");

  // fromModelKey resolves <cache_dir>/<model_name>/<target>/manifest.json, nothing else
  FetchOptions fetch;
  fetch.cache_dir = g_models;
  LLM keyed = LLM::fromModelKey("lfm3-1.7b", "secret-key", "", LLM::Options{}, fetch);
  CHECK_EQ(keyed.info().model_id, "LiquidAI/LFM3-1.7B");
  Decision keyed_d1 = Decision::fromModelKey("d1-2b", "secret-key", Decision::Options{}, fetch);
  CHECK_EQ(keyed_d1.info().model_id, "LiquidAI/d1-2B");
  expectError([&] { (void)LLM::fromModelKey("no-such-model", "secret-key", "", LLM::Options{}, fetch); }, Status::Unsupported);

  // the load errors
  expectError([&] { (void)LLM::fromPath(model("nope")); }, Status::PackageError, "cannot read " + model("nope") + "/manifest.json");
  expectError([&] { (void)LLM::fromPath(model("d1-2b")); }, Status::Unsupported,
              model("d1-2b") + " is a d1 decision package: load it with Decision::fromPath");
  expectError([&] { (void)Decision::fromPath(model("lfm3-1.7b")); }, Status::Unsupported,
              model("lfm3-1.7b") + " is a generative package: load it with LLM::fromPath");
  LLM::Options missing_target;
  missing_target.target = "v73-1nsp";
  expectError([&] { (void)LLM::fromPath(model("lfm3-1.7b"), missing_target); }, Status::PackageError,
              "no v73-1nsp package in " + model("lfm3-1.7b"));
  setenv("IXIAN_MOCK_FAIL_LOAD", "1", 1);
  expectError([&] { (void)LLM::fromPath(model("lfm3-1.7b")); }, Status::DeviceError, "DeviceError: hexrt_lib_open: map: 0x1", 0x1);
  unsetenv("IXIAN_MOCK_FAIL_LOAD");

  // a target directory given directly is the v73 package
  LLM direct = LLM::fromPath(model("lfm3-1.7b") + "/v73");
  CHECK_EQ(direct.info().target, "v73");

  // moves
  LLM moved = std::move(text);
  CHECK_EQ(moved.info().model_id, "LiquidAI/LFM3-1.7B");
}

static void testTokenizer(LLM& llm) {
  const std::string text = "Hello world!\n  indented\tline  \nnon-ascii: caf\xc3\xa9 \xe2\x82\xac done ";
  const std::vector<int32_t> ids = llm.tokenize(text, false);
  CHECK_EQ(llm.detokenize(ids), text);
  CHECK(std::all_of(ids.begin(), ids.end(), [](int32_t id) { return id >= 1000; }));
  // with_pieces: one id at a time gives the pieces
  std::string joined;
  for (int32_t id : ids) joined += llm.detokenize({id});
  CHECK_EQ(joined, text);
  // GPT-style pieces: a leading blank belongs to the word
  CHECK_EQ(llm.tokenize(" Hello", false).size(), size_t(1));
  CHECK_EQ(llm.tokenize("Hello", false).size(), size_t(1));
  CHECK(llm.tokenize(" Hello", false) != llm.tokenize("Hello", false));
  CHECK(llm.tokenize("Hello", false) == llm.tokenize("Hello", false));
  CHECK_EQ(llm.tokenize("a b", false).size(), size_t(2));
  CHECK_EQ(llm.tokenize("a\nb", false).size(), size_t(3));
  CHECK_EQ(llm.tokenize("x  \n", false).size(), size_t(3));  // "x", "  ", "\n"
  // special tokens
  const std::vector<int32_t> sp = llm.tokenize("<|im_start|>user\nHi<|im_end|>", true);
  CHECK(sp.size() >= 4 && sp.front() == 2 && sp.back() == 3);
  CHECK_EQ(llm.detokenize(sp), "<|im_start|>user\nHi<|im_end|>");
  CHECK_EQ(llm.tokenize("<|startoftext|><think></think><|image|><|audio|>", true), (std::vector<int32_t>{1, 5, 6, 8, 16}));
  const std::vector<int32_t> plain = llm.tokenize("<|im_start|>", false);
  CHECK(std::find(plain.begin(), plain.end(), 2) == plain.end());
  CHECK_EQ(llm.detokenize(plain), "<|im_start|>");
  CHECK_EQ(llm.detokenize({1}), "<|startoftext|>");
  CHECK_EQ(llm.detokenize({999999}), "");
  CHECK_EQ(llm.detokenize({}), "");
  CHECK(llm.tokenize("x").front() != 1);  // never adds BOS
  CHECK(llm.tokenize("").empty());
}

static void testTemplate(LLM& llm, LLM& omni, LLM& audio) {
  std::vector<Message> msgs{Message::System("You are helpful."), Message::User("Hi")};
  const std::string head = "<|startoftext|><|im_start|>system\nYou are helpful.<|im_end|>\n<|im_start|>user\nHi<|im_end|>\n";
  CHECK_EQ(llm.applyChatTemplate(msgs), head + "<|im_start|>assistant\n<think></think>");
  TemplateOptions thinking;
  thinking.enable_thinking = true;
  CHECK_EQ(llm.applyChatTemplate(msgs, thinking), head + "<|im_start|>assistant\n<think>");
  TemplateOptions no_prompt;
  no_prompt.add_generation_prompt = false;
  CHECK_EQ(llm.applyChatTemplate(msgs, no_prompt), head);
  // an assistant turn, then another user turn
  msgs.push_back(Message::Assistant("Hello!"));
  msgs.push_back(Message::User("Bye"));
  CHECK_EQ(llm.applyChatTemplate(msgs), head + "<|im_start|>assistant\n<think></think>Hello!<|im_end|>\n<|im_start|>user\nBye<|im_end|>\n<|im_start|>assistant\n<think></think>");
  // an assistant text that already closes a think block is left alone
  CHECK(contains(llm.applyChatTemplate({Message::User("a"), Message::Assistant("<think>t</think>b")}, no_prompt),
                 "<|im_start|>assistant\n<think>t</think>b<|im_end|>\n"));
  // media placeholders
  const std::string with_media = omni.applyChatTemplate(
      {Message::User({Part::Img(makeImage(512, 512)), Part::Text("Describe"), Part::Aud(makeAudio(1.0)),
                      Part::Vid(Video::fromFrames({makeImage(512, 512)}, 2.0))})},
      no_prompt);
  CHECK_EQ(with_media, "<|startoftext|><|im_start|>user\n<|vision_start|><|image|><|vision_end|>Describe<|vision_start|><|audio|><|vision_end|><|vision_start|><|video|><|vision_end|><|im_end|>\n");
  // a package without a think block (chat.think_off false) renders none
  CHECK_EQ(audio.applyChatTemplate({Message::User("Hi")}, thinking), "<|startoftext|><|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n");
  CHECK_EQ(audio.applyChatTemplate({Message::User("Hi"), Message::Assistant("Yo")}, no_prompt), "<|startoftext|><|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\nYo<|im_end|>\n");
}

static void testInspect(LLM& llm, LLM& omni) {
  const std::vector<Message> msgs{Message::User("Hello there")};
  const PromptInfo pi = llm.inspect(msgs);
  CHECK_EQ(pi.rows, static_cast<int32_t>(llm.tokenize(llm.applyChatTemplate(msgs)).size()));
  CHECK_EQ(pi.chunks.size(), size_t(1));
  CHECK(pi.chunks[0].type == Part::Type::Text);
  CHECK_EQ(pi.chunks[0].rows, pi.rows);
  CHECK_EQ(pi.chunks[0].first_row, 0);
  CHECK_EQ(pi.room, 2048 - pi.rows);
  GenerationConfig thinking;
  thinking.enable_thinking = true;
  CHECK_EQ(llm.inspect(msgs, thinking).rows, pi.rows - 1);  // "<think>" instead of "<think></think>"

  // a 512 x 512 image is 256 rows
  const std::vector<Message> with_image{Message::User({Part::Img(makeImage(512, 512)), Part::Text("Describe")})};
  const PromptInfo pj = omni.inspect(with_image);
  CHECK_EQ(pj.chunks.size(), size_t(3));
  CHECK(pj.chunks[0].type == Part::Type::Text && pj.chunks[1].type == Part::Type::Image && pj.chunks[2].type == Part::Type::Text);
  CHECK_EQ(pj.chunks[1].rows, 256);
  CHECK_EQ(pj.chunks[1].first_row, pj.chunks[0].rows);
  CHECK_EQ(pj.chunks[2].first_row, pj.chunks[0].rows + 256);
  CHECK_EQ(pj.rows, pj.chunks[0].rows + 256 + pj.chunks[2].rows);
  CHECK_EQ(pj.room, 2048 - pj.rows);

  // audio: ceil(seconds * 12.5); a 30 s clip is 375
  const PromptInfo pa = omni.inspect({Message::User({Part::Aud(makeAudio(30.0))})});
  CHECK_EQ(pa.chunks.size(), size_t(3));
  CHECK(pa.chunks[1].type == Part::Type::Audio);
  CHECK_EQ(pa.chunks[1].rows, 375);
  CHECK_EQ(omni.inspect({Message::User({Part::Aud(makeAudio(1.6))})}).chunks[1].rows, 20);

  // video: 256 per temporal patch of two sampled frames; an odd count repeats the last frame
  std::vector<Image> frames(4, makeImage(512, 512));
  CHECK_EQ(omni.inspect({Message::User({Part::Vid(Video::fromFrames(frames, 2.0))})}).chunks[1].rows, 512);
  frames.resize(3);
  CHECK_EQ(omni.inspect({Message::User({Part::Vid(Video::fromFrames(frames, 0))})}).chunks[1].rows, 512);
  frames.assign(8, makeImage(512, 512));  // 8 frames at 4 fps = 2 s -> 4 sampled at 2 fps -> 2 patches
  CHECK_EQ(omni.inspect({Message::User({Part::Vid(Video::fromFrames(frames, 4.0))})}).chunks[1].rows, 512);
}

static void testEchoAndDirectives(LLM& llm) {
  std::vector<std::string> pieces;
  int texts = 0, dones = 0;
  const Response r = llm.chat({Message::User("echo: Hello world")}, {}, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Text) {
      ++texts;
      pieces.emplace_back(ev.text);
      CHECK(!ev.reasoning);
      CHECK(ev.token >= 1000);
      CHECK(ev.logprob == nullptr);
    } else if (ev.type == StreamEvent::Type::Done) {
      ++dones;
      CHECK(ev.response != nullptr && ev.response->text == "Hello world");
    }
    return true;
  });
  CHECK_EQ(r.status, Status::Ok);
  CHECK_EQ(r.finish_reason, FinishReason::Stop);
  std::string joined;
  for (const std::string& p : pieces) joined += p;
  CHECK_EQ(joined, "Hello world");
  CHECK_EQ(r.text, "Hello world");
  CHECK_EQ(texts, 2);
  CHECK_EQ(dones, 1);
  CHECK_EQ(r.usage.completion_tokens, texts);
  CHECK_EQ(static_cast<int>(r.tokens.size()), texts);
  CHECK_EQ(r.usage.cached_rows, 0);
  CHECK(r.usage.prompt_rows > 0);
  CHECK(r.reasoning_content.empty());
  CHECK(r.message.role == Role::Assistant);
  CHECK(r.message.generated != nullptr);
  CHECK_EQ(r.message.text(), "Hello world");
  CHECK_EQ(r.timings.decode_steps, texts - 1);
  CHECK(r.timings.ttft > 5.0);
  CHECK(std::fabs(r.timings.prefill - (5 + 1.5 * r.usage.prompt_rows)) < 1e-9);
  CHECK(r.timings.decode >= 0 && r.timings.callbacks >= 0);
  CHECK(r.audio.empty());

  CHECK_EQ(llm.chat({Message::User("echo:")}).text, "");
  CHECK_EQ(llm.chat({Message::User("repeat: 3 foo")}).text, "foo foo foo");
  CHECK_EQ(llm.chat({Message::User("json: hi \"there\"")}).text, "{\"answer\": \"hi \\\"there\\\"\"}");
  CHECK_EQ(llm.chat({Message::System("You may call tools.\n<tools>\n{\"name\": \"get_weather\"}\n</tools>"),
                     Message::User("tool: get_weather {\"city\": \"Paris\", \"days\": 2, \"units\": {\"t\": \"C\"}}")}).text,
           "<tool_call>\n<function=get_weather>\n<parameter=city>\nParis\n</parameter>\n<parameter=days>\n2\n</parameter>\n<parameter=units>\n{\"t\":\"C\"}\n</parameter>\n</function>\n</tool_call>");
  CHECK_EQ(llm.chat({Message::User("tool: get_weather {}")}).text, "no tools");

  // the default stream: deterministic for a seed, different for another, 24..48 words ending with "."
  GenerationConfig seeded;
  seeded.seed = 7;
  const Response d1 = llm.chat({Message::User("Tell me something.")}, seeded);
  const Response d2 = llm.chat({Message::User("Tell me something.")}, seeded);
  CHECK_EQ(d1.text, d2.text);
  CHECK(!d1.text.empty() && d1.text.back() == '.');
  const int words = static_cast<int>(std::count(d1.text.begin(), d1.text.end(), ' ')) + 1;
  CHECK(words >= 24 && words <= 48);
  seeded.seed = 8;
  CHECK(llm.chat({Message::User("Tell me something.")}, seeded).text != d1.text);
  seeded.seed = 0;
  CHECK(llm.chat({Message::User("Tell me something.")}, seeded).finish_reason == FinishReason::Stop);

  // error: <Status>
  int error_dones = 0;
  const Response e = llm.chat({Message::User("error: DeviceError")}, {}, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Done) ++error_dones;
    return true;
  });
  CHECK_EQ(e.status, Status::DeviceError);
  CHECK_EQ(e.error, "mock: DeviceError");
  CHECK_EQ(e.finish_reason, FinishReason::Error);
  CHECK_EQ(error_dones, 1);
  CHECK_EQ(llm.chat({Message::User("error: Busy")}).status, Status::Busy);

  // sleep: waits before the first token, ttft includes it
  const auto t0 = std::chrono::steady_clock::now();
  const Response s = llm.chat({Message::User("sleep: 120 hi there")});
  const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK_EQ(s.text, "hi there");
  CHECK(elapsed >= 120);
  CHECK(s.timings.ttft >= 120);

  // logprobs
  GenerationConfig lp;
  lp.logprobs = true;
  lp.top_logprobs = 3;
  int lp_events = 0;
  const Response l = llm.chat({Message::User("echo: a b c")}, lp, [&](const StreamEvent& ev) {
    if (ev.type != StreamEvent::Type::Text) return true;
    ++lp_events;
    CHECK(ev.logprob != nullptr);
    if (ev.logprob) {
      CHECK_EQ(ev.logprob->token, ev.token);
      CHECK_EQ(ev.logprob->top.size(), size_t(3));
      CHECK_EQ(ev.logprob->top[0].first, ev.token);
      CHECK(ev.logprob->logprob < 0);
    }
    return true;
  });
  CHECK_EQ(lp_events, 3);
  CHECK_EQ(l.logprobs.size(), l.tokens.size());
  CHECK(std::fabs(l.logprobs[0].logprob - (-0.05f)) < 1e-6);
  CHECK(std::fabs(l.logprobs[1].logprob - (-0.10f)) < 1e-6);
  lp.top_logprobs = 21;
  CHECK_EQ(llm.chat({Message::User("echo: a")}, lp).status, Status::InvalidArgument);

  // empty request
  const Response empty = llm.chat({});
  CHECK_EQ(empty.status, Status::InvalidArgument);
  CHECK_EQ(empty.error, "messages must not be empty");
  CHECK_EQ(empty.finish_reason, FinishReason::Error);
}

static void testLengthAndStop(LLM& llm) {
  GenerationConfig g;
  g.max_tokens = 3;
  const Response r = llm.chat({Message::User("echo: one two three four five")}, g);
  CHECK_EQ(r.finish_reason, FinishReason::Length);
  CHECK_EQ(r.tokens.size(), size_t(3));
  CHECK_EQ(r.text, "one two three");
  CHECK_EQ(r.usage.completion_tokens, 3);

  // LLMOptions::max_new_tokens is the default limit
  LLM::Options o;
  o.max_new_tokens = 2;
  LLM short_llm = LLM::fromPath(model("lfm3-1.7b"), o);
  CHECK_EQ(short_llm.chat({Message::User("echo: a b c")}).finish_reason, FinishReason::Length);
  CHECK_EQ(short_llm.chat({Message::User("echo: a b c")}).text, "a b");

  // stop sequences: matched on the detokenized reply, never streamed
  GenerationConfig s;
  s.stop = {"three"};
  std::string streamed;
  const Response r2 = llm.chat({Message::User("echo: one two three four")}, s, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Text) streamed += ev.text;
    return true;
  });
  CHECK_EQ(r2.finish_reason, FinishReason::StopSequence);
  CHECK_EQ(r2.text, "one two ");
  CHECK_EQ(streamed, r2.text);
  CHECK(!contains(streamed, "three"));
  // a match across pieces, and one that cuts a piece
  s.stop = {"two three"};
  CHECK_EQ(llm.chat({Message::User("echo: one two three four")}, s).text, "one ");
  s.stop = {"hre", "zzz"};
  streamed.clear();
  const Response r3 = llm.chat({Message::User("echo: one two three four")}, s, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Text) streamed += ev.text;
    return true;
  });
  CHECK_EQ(r3.finish_reason, FinishReason::StopSequence);
  CHECK_EQ(r3.text, "one two t");
  CHECK_EQ(streamed, r3.text);
  // a stop sequence that never matches: everything is streamed in the end
  s.stop = {"four and more"};
  streamed.clear();
  const Response r4 = llm.chat({Message::User("echo: one two three four")}, s, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Text) streamed += ev.text;
    return true;
  });
  CHECK_EQ(r4.finish_reason, FinishReason::Stop);
  CHECK_EQ(streamed, "one two three four");
}

static void testThinking(LLM& llm, LLM& audio) {
  GenerationConfig g;
  g.enable_thinking = true;
  std::string reasoning, answer;
  const Response r = llm.chat({Message::User("think: deeply")}, g, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Text) (ev.reasoning ? reasoning : answer) += ev.text;
    return true;
  });
  CHECK_EQ(r.status, Status::Ok);
  CHECK_EQ(r.reasoning_content, "deeply");
  CHECK_EQ(r.text, "I thought about it.");
  CHECK_EQ(reasoning, "deeply");
  CHECK_EQ(answer, r.text);
  CHECK(std::find(r.tokens.begin(), r.tokens.end(), 6) != r.tokens.end());  // </think> is in the ids
  CHECK_EQ(r.usage.completion_tokens, static_cast<int32_t>(r.tokens.size()));
  CHECK_EQ(r.message.text(), r.text);
  // the record renders the thinking into the next prompt, and the cache covers the whole turn
  const std::vector<Message> history{Message::User("think: deeply"), r.message, Message::User("echo: next")};
  TemplateOptions to;
  to.enable_thinking = true;
  CHECK(contains(llm.applyChatTemplate(history, to), "<|im_start|>assistant\n<think>deeply</think>I thought about it.<|im_end|>\n"));
  CHECK(llm.inspect(history, g).cached_rows >= r.usage.prompt_rows + static_cast<int32_t>(r.tokens.size()) + 1);
  // without the record the thinking is dropped (HF behaviour)
  CHECK(contains(llm.applyChatTemplate({Message::User("think: deeply"), Message::Assistant(r.text)}, to),
                 "<|im_start|>assistant\n<think></think>I thought about it.<|im_end|>\n"));

  // the default thinking text
  const Response r2 = llm.chat({Message::User("echo: hi")}, g);
  CHECK_EQ(r2.reasoning_content, "Let me think about this.");
  CHECK_EQ(r2.text, "hi");
  // thinking off: the think: text is not emitted
  const Response r3 = llm.chat({Message::User("think: deeply")});
  CHECK(r3.reasoning_content.empty());
  CHECK_EQ(r3.text, "I thought about it.");
  CHECK(std::find(r3.tokens.begin(), r3.tokens.end(), 6) == r3.tokens.end());
  // a package without a think block ignores enable_thinking
  const Response r4 = audio.chat({Message::User("echo: hi")}, g);
  CHECK(r4.reasoning_content.empty());
  CHECK_EQ(r4.text, "hi");
}

static void testCancel(LLM& llm) {
  // the callback returning false
  int seen = 0, dones = 0;
  const Response r = llm.chat({Message::User("echo: one two three four")}, {}, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Done) ++dones;
    if (ev.type != StreamEvent::Type::Text) return true;
    ++seen;
    return seen < 2;
  });
  CHECK_EQ(r.status, Status::Ok);
  CHECK_EQ(r.finish_reason, FinishReason::Cancelled);
  CHECK_EQ(seen, 2);
  CHECK_EQ(dones, 0);  // a callback that said stop gets nothing more
  CHECK_EQ(r.text, "one two");
  CHECK_EQ(r.tokens.size(), size_t(2));
  CHECK(r.message.generated != nullptr);

  // cancel() from another thread while `stall` runs
  std::thread canceller([&] {
    std::this_thread::sleep_for(200ms);
    llm.cancel();
  });
  const auto t0 = std::chrono::steady_clock::now();
  const Response s = llm.chat({Message::User("stall")});
  canceller.join();
  const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK_EQ(s.finish_reason, FinishReason::Cancelled);
  CHECK(s.tokens.size() >= 1 && s.tokens.size() <= 10);
  CHECK(contains(s.text, "stall"));
  CHECK(elapsed < 3000);
  // cancel() during a `sleep:` directive
  std::thread canceller2([&] {
    std::this_thread::sleep_for(50ms);
    llm.cancel();
  });
  const Response s2 = llm.chat({Message::User("sleep: 5000 never")});
  canceller2.join();
  CHECK_EQ(s2.finish_reason, FinishReason::Cancelled);
  CHECK(s2.text.empty());
  // the LLM is usable afterwards
  CHECK_EQ(llm.chat({Message::User("echo: ok")}).text, "ok");
}

static void testPrefixCache(LLM& llm, LLM& omni) {
  llm.reset();
  const Response r1 = llm.chat({Message::User("echo: Hello world")});
  CHECK_EQ(r1.usage.cached_rows, 0);
  std::vector<Message> history{Message::User("echo: Hello world"), r1.message, Message::User("echo: again")};
  const Response r2 = llm.chat(history);
  CHECK(r2.usage.cached_rows > 0);
  CHECK(r2.usage.cached_rows >= r1.usage.prompt_rows + static_cast<int32_t>(r1.tokens.size()) + 1);
  CHECK(r2.usage.cached_rows < r2.usage.prompt_rows);
  CHECK(std::fabs(r2.timings.prefill - (5 + 1.5 * (r2.usage.prompt_rows - r2.usage.cached_rows))) < 1e-9);
  // a hand-written assistant message with the same text tokenizes to the same rows
  std::vector<Message> rewritten{Message::User("echo: Hello world"), Message::Assistant("Hello world"), Message::User("echo: again")};
  CHECK_EQ(llm.inspect(rewritten).rows, r2.usage.prompt_rows);
  // cache_prompt false
  history.push_back(r2.message);
  history.push_back(Message::User("echo: more"));
  GenerationConfig no_cache;
  no_cache.cache_prompt = false;
  CHECK_EQ(llm.chat(history, no_cache).usage.cached_rows, 0);
  CHECK(llm.inspect(history).cached_rows > 0);
  // reset drops it
  llm.reset();
  CHECK_EQ(llm.inspect(history).cached_rows, 0);
  CHECK_EQ(llm.chat(history).usage.cached_rows, 0);

  // media rows are cached by content
  omni.reset();
  const auto image = std::make_shared<const Image>(makeImage(512, 512, 0x42));
  std::vector<Message> media_history{Message::User({Part::Img(image), Part::Text("echo: a")})};
  const Response m1 = omni.chat(media_history);
  CHECK_EQ(m1.status, Status::Ok);
  CHECK(std::fabs(m1.timings.encode - 20.0) < 1e-9);
  media_history.push_back(m1.message);
  media_history.push_back(Message::User("echo: b"));
  const Response m2 = omni.chat(media_history);
  CHECK(m2.usage.cached_rows >= 256 + 4);
  // the same pixels in another Image object match too; different pixels do not
  std::vector<Message> same_pixels{Message::User({Part::Img(makeImage(512, 512, 0x42)), Part::Text("echo: a")})};
  CHECK(omni.inspect(same_pixels).cached_rows >= 256 + 4);
  std::vector<Message> other_pixels{Message::User({Part::Img(makeImage(512, 512, 0x43)), Part::Text("echo: a")})};
  CHECK(omni.inspect(other_pixels).cached_rows < 256);
}

static void testValidation(LLM& llm, LLM& omni, LLM& audio) {
  // context
  GenerationConfig big;
  big.max_tokens = 4096;
  const std::vector<Message> msgs{Message::User("echo: x")};
  const int32_t rows = llm.inspect(msgs).rows;
  const Response r = llm.chat(msgs, big);
  CHECK_EQ(r.status, Status::ContextExceeded);
  CHECK_EQ(r.error, "the request needs " + std::to_string(rows) + " prompt rows + 4096 reply rows, the context holds 2048");
  CHECK_EQ(r.finish_reason, FinishReason::Error);
  // the speech reserve counts: 1700 + ceil(30 * 12.5) = 2075 > 2048
  GenerationConfig speech;
  speech.modalities = {Modality::Audio};
  speech.max_tokens = 1700;
  const int32_t arows = audio.inspect(msgs).rows;
  const Response ra = audio.chat(msgs, speech);
  CHECK_EQ(ra.status, Status::ContextExceeded);
  CHECK_EQ(ra.error, "the request needs " + std::to_string(arows) + " prompt rows + 2075 reply rows, the context holds 2048");
  speech.max_tokens = 1600;
  CHECK_EQ(audio.chat(msgs, speech).status, Status::Ok);

  // media the package lacks
  const Response ui = llm.chat({Message::User({Part::Img(makeImage(512, 512)), Part::Text("echo: x")})});
  CHECK_EQ(ui.status, Status::Unsupported);
  CHECK_EQ(ui.error, "this package takes no image input");
  CHECK_EQ(llm.chat({Message::User({Part::Aud(makeAudio(1))})}).error, "this package takes no audio input");
  CHECK_EQ(llm.chat({Message::User({Part::Vid(Video::fromFrames({makeImage(512, 512)}, 2))})}).error, "this package takes no video input");
  CHECK_EQ(audio.chat({Message::User({Part::Img(makeImage(512, 512))})}).status, Status::Unsupported);

  // media shape
  const Response bad = omni.chat({Message::User({Part::Img(makeImage(640, 480)), Part::Text("echo: x")})});
  CHECK_EQ(bad.status, Status::InvalidArgument);
  CHECK_EQ(bad.error, "an image is 640x480; images must be exactly 512x512 RGB (resize or letterbox before passing it; the SDK does not resample)");
  const Response badv = omni.chat({Message::User({Part::Vid(Video::fromFrames({makeImage(512, 512), makeImage(640, 480)}, 2))})});
  CHECK_EQ(badv.status, Status::InvalidArgument);
  CHECK_EQ(badv.error, "a video frame is 640x480; video frames must be exactly 512x512 RGB (resize or letterbox before passing it; the SDK does not resample)");
  const Response bada = omni.chat({Message::User({Part::Aud(makeAudio(31.0))})});
  CHECK_EQ(bada.status, Status::InvalidArgument);
  CHECK_EQ(bada.error, "an audio part is 31 s; one audio part holds up to 30 s");
  CHECK_EQ(omni.chat({Message::User({Part::Aud(makeAudio(31.5))})}).error, "an audio part is 31.5 s; one audio part holds up to 30 s");
  CHECK_EQ(omni.chat({Message::User({Part::Aud(makeAudio(30.0)), Part::Text("echo: ok")})}).text, "ok");
  // a 512 x 512 image on a capable package works
  CHECK_EQ(omni.chat({Message::User({Part::Img(makeImage(512, 512)), Part::Text("echo: ok")})}).text, "ok");

  // output modality
  GenerationConfig tts;
  tts.modalities = {Modality::Audio};
  const Response ns = llm.chat({Message::User("echo: x")}, tts);
  CHECK_EQ(ns.status, Status::Unsupported);
  CHECK_EQ(ns.error, "this package has no speech output (capabilities.speech_out false)");
  tts.logprobs = true;
  const Response lpa = audio.chat({Message::User("echo: x")}, tts);
  CHECK_EQ(lpa.status, Status::InvalidArgument);
  CHECK_EQ(lpa.error, "logprobs is not available with audio output");
}

static void testSpeech(LLM& audio, LLM& omni) {
  GenerationConfig g;
  g.modalities = {Modality::Audio};
  int audio_events = 0, text_events = 0, dones = 0;
  bool last = false, contiguous = true;
  size_t samples = 0;
  const Response r = audio.chat({Message::System("Perform TTS in American English."), Message::User("echo: Hello world")}, g,
                                [&](const StreamEvent& ev) {
                                  if (ev.type == StreamEvent::Type::Text) ++text_events;
                                  if (ev.type == StreamEvent::Type::Done) ++dones;
                                  if (ev.type != StreamEvent::Type::Audio) return true;
                                  ++audio_events;
                                  CHECK(ev.audio != nullptr && ev.audio->pcm != nullptr && ev.audio->samples > 0);
                                  if (!ev.audio) return true;
                                  CHECK_EQ(ev.audio->sample_rate, 24000);
                                  if (ev.audio->first_sample != static_cast<int64_t>(samples)) contiguous = false;
                                  samples += ev.audio->samples;
                                  last = ev.audio->last;
                                  return true;
                                });
  CHECK_EQ(r.status, Status::Ok);
  CHECK_EQ(r.finish_reason, FinishReason::Stop);
  CHECK(!r.audio.empty());
  CHECK_EQ(r.audio.size(), samples);
  CHECK_EQ(r.audio.size(), size_t(6) * 1920);  // 2 words * 3 frames of 80 ms
  CHECK(last);
  CHECK(contiguous);
  CHECK_EQ(text_events, 0);
  CHECK_EQ(dones, 1);
  CHECK_EQ(audio_events, 2);  // first_chunk_frames 1, then the remaining 5 in one chunk of up to 12
  CHECK_EQ(r.usage.audio_frames, 6);
  CHECK(std::fabs(r.timings.audio_seconds - 0.48) < 1e-9);
  CHECK(r.timings.realtime_factor > 0);
  CHECK(r.timings.first_audio > 0);
  CHECK(r.text.empty() && r.tokens.empty());
  float peak = 0;
  for (float v : r.audio) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.2f && peak <= 0.3f);
  // the frame count follows the words, bounded by max_audio_seconds
  g.max_audio_seconds = 1.0;
  CHECK_EQ(audio.chat({Message::User("repeat: 40 la")}, g).usage.audio_frames, 12);
  g.max_audio_seconds = 30;
  CHECK_EQ(audio.chat({Message::User("echo: a")}, g).usage.audio_frames, 5);
  // chunking
  g.audio.first_chunk_frames = 2;
  g.audio.chunk_frames = 3;
  std::vector<size_t> chunk_samples;
  (void)audio.chat({Message::User("repeat: 4 la")}, g, [&](const StreamEvent& ev) {  // 12 frames
    if (ev.type == StreamEvent::Type::Audio) chunk_samples.push_back(ev.audio->samples);
    return true;
  });
  CHECK_EQ(chunk_samples.size(), size_t(5));
  if (chunk_samples.size() == 5) {
    CHECK_EQ(chunk_samples[0], size_t(2) * 1920);
    CHECK_EQ(chunk_samples[1], size_t(3) * 1920);
    CHECK_EQ(chunk_samples[4], size_t(1) * 1920);
  }
  // collect = false: events only
  g.audio.collect = false;
  CHECK(audio.chat({Message::User("echo: a")}, g).audio.empty());

  // {Text, Audio}: both streams, interleaved
  GenerationConfig both;
  both.modalities = {Modality::Text, Modality::Audio};
  std::string order;
  const Response b = omni.chat({Message::User("echo: one two three")}, both, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Text) order += 'T';
    if (ev.type == StreamEvent::Type::Audio) order += 'A';
    return true;
  });
  CHECK_EQ(b.status, Status::Ok);
  CHECK_EQ(b.text, "one two three");
  CHECK_EQ(b.usage.audio_frames, 9);
  CHECK(!b.audio.empty());
  CHECK(order.rfind("TATA", 0) == 0);  // a text piece, then an audio chunk, ...
  CHECK(std::count(order.begin(), order.end(), 'T') == 3);
}

static void testBusy() {
  LLM::Options o;
  o.wait_if_busy = false;
  LLM llm = LLM::fromPath(model("lfm3-1.7b"), o);
  std::atomic<bool> started{false};
  Response other;
  std::thread runner([&] {
    other = llm.chat({Message::User("stall")}, {}, [&](const StreamEvent& ev) {
      if (ev.type == StreamEvent::Type::Text) started = true;
      return true;
    });
  });
  for (int i = 0; i < 1000 && !started; ++i) std::this_thread::sleep_for(5ms);
  CHECK(started.load());
  const Response busy = llm.chat({Message::User("echo: x")});
  CHECK_EQ(busy.status, Status::Busy);
  CHECK_EQ(busy.finish_reason, FinishReason::Error);
  CHECK(!busy.error.empty());
  llm.cancel();
  runner.join();
  CHECK_EQ(other.finish_reason, FinishReason::Cancelled);
  // the tokenizer helpers run concurrently with a chat()
  std::thread runner2([&] { other = llm.chat({Message::User("repeat: 150 word")}); });
  for (int i = 0; i < 200; ++i) {
    CHECK_EQ(llm.detokenize(llm.tokenize("concurrent access")), "concurrent access");
    (void)llm.inspect({Message::User("x")});
    (void)llm.applyChatTemplate({Message::User("y")});
  }
  runner2.join();
  CHECK_EQ(other.usage.completion_tokens, 150);

  // wait_if_busy (the default): the second call waits for the first
  LLM waiting = LLM::fromPath(model("lfm3-1.7b"));
  std::thread runner3([&] { other = waiting.chat({Message::User("sleep: 200 first")}); });
  std::this_thread::sleep_for(50ms);
  const Response second = waiting.chat({Message::User("echo: second")});
  runner3.join();
  CHECK_EQ(second.status, Status::Ok);
  CHECK_EQ(second.text, "second");
  CHECK_EQ(other.text, "first");
}

static void testDecision() {
  Decision d1 = Decision::fromPath(model("d1-2b"));
  const std::vector<std::pair<std::string, Question>> questions{
      {"refund", Question::Noul("Is the customer asking for a refund?")},
      {"team", Question::Choice("Which team should handle this?", {{"billing", "Charges, refunds"}, {"shipping", "Parcels"}})},
      {"urgency", Question::Score("How urgent is this ticket?", {"low", "medium", "high"})}};
  const State state = State::Text("I was charged twice and my parcel never arrived. Please refund one charge.");
  const DecisionResult out = d1.systemOne(state, questions);
  CHECK_EQ(out.status, Status::Ok);
  CHECK_EQ(out.answers.size(), size_t(3));
  if (out.answers.size() == 3) {
    CHECK_EQ(out.answers[0].first, "refund");
    CHECK_EQ(out.answers[1].first, "team");
    CHECK_EQ(out.answers[2].first, "urgency");
    for (const auto& [name, a] : out.answers) {
      (void)name;
      double sum = 0;
      for (double p : a.probs) sum += p;
      CHECK(std::fabs(sum - 1.0) < 1e-9);
      CHECK_EQ(a.probs.size(), a.labels.size());
      CHECK_EQ(a.raw_scores.size(), a.labels.size());
      for (float s : a.raw_scores) CHECK(s >= -3.0f && s <= 3.0f);
      CHECK(a.confidence >= 1.0 / a.labels.size() - 1e-9 && a.confidence <= 1.0);
    }
    const Answer& refund = out.answers[0].second;
    CHECK(refund.type == Question::Type::Noul);
    CHECK_EQ(refund.labels, (std::vector<std::string>{"yes", "no"}));
    CHECK_EQ(refund.noul, refund.probs[0]);
    const Answer& team = out.answers[1].second;
    CHECK(team.type == Question::Type::Choice);
    CHECK_EQ(team.labels, (std::vector<std::string>{"billing", "shipping"}));
    const size_t best = static_cast<size_t>(std::max_element(team.probs.begin(), team.probs.end()) - team.probs.begin());
    CHECK_EQ(team.choice, team.labels[best]);
    CHECK_EQ(team.confidence, team.probs[best]);
    CHECK(team.choices.empty());
    const Answer& urgency = out.answers[2].second;
    CHECK(urgency.type == Question::Type::Score);
    CHECK(urgency.score >= 0 && urgency.score < 3);
    CHECK_EQ(urgency.legend, urgency.labels[static_cast<size_t>(urgency.score)]);
    CHECK(urgency.legend == "low" || urgency.legend == "medium" || urgency.legend == "high");
  }
  CHECK_EQ(out.passes, 1);
  CHECK(out.input_tokens > 0);
  CHECK(std::fabs(out.timings.prefill - 5.0) < 1e-9);
  CHECK(std::fabs(out.timings.encode) < 1e-9);
  // deterministic
  const DecisionResult again = d1.systemOne(state, questions);
  CHECK(again.answers.size() == 3 && again.answers[1].second.probs == out.answers[1].second.probs);
  // a different state gives (very likely) different numbers
  const DecisionResult other = d1.systemOne(State::Text("Where is my order?"), questions);
  CHECK(other.answers.size() == 3 && other.answers[0].second.noul != out.answers[0].second.noul);

  // multiple choice, abstain, Json state, set_size
  Question multi = Question::Choice("Topics?", {{"a", ""}, {"b", ""}, {"c", ""}}, true);
  multi.abstain = "none";
  const DecisionResult m = d1.systemOne(State::Json("{\"text\": \"hello\"}"), {{"topics", multi}});
  CHECK_EQ(m.status, Status::Ok);
  CHECK_EQ(m.answers[0].second.labels.size(), size_t(4));
  for (const std::string& c : m.answers[0].second.choices) CHECK(c == "a" || c == "b" || c == "c" || c == "none");
  Decision::Options one_per_pass;
  one_per_pass.set_size = 1;
  Decision d1_single = Decision::fromPath(model("d1-2b"), one_per_pass);
  CHECK_EQ(d1_single.systemOne(state, questions).passes, 3);
  CHECK(std::fabs(d1_single.systemOne(state, questions).timings.prefill - 15.0) < 1e-9);

  // batch, probabilities, runRaw, cancel
  DecisionRequest rq;
  rq.state = state;
  rq.questions = questions;
  const std::vector<DecisionResult> batch = d1.systemOneBatch({rq, rq});
  CHECK_EQ(batch.size(), size_t(2));
  CHECK(batch.size() == 2 && batch[1].answers.size() == 3);
  const std::vector<std::vector<double>> probs = d1.probabilities(rq);
  CHECK_EQ(probs.size(), size_t(3));
  CHECK(probs.size() == 3 && probs[0].size() == 2 && probs[2].size() == 3);
  Decision::RawPass pass;
  pass.read_ids = {{1, 2, 3}, {4}};
  const std::vector<std::vector<float>> raw = d1.runRaw(pass);
  CHECK_EQ(raw.size(), size_t(2));
  CHECK(raw.size() == 2 && raw[0].size() == 3 && raw[1].size() == 1 && raw[0][0] == 0.0f);
  d1.cancel();

  // media
  const DecisionResult vid = d1.systemOne(State::None(), questions, {Part::Vid(Video::fromFrames({makeImage(512, 512)}, 2))});
  CHECK_EQ(vid.status, Status::Unsupported);
  CHECK_EQ(vid.error, "d1 takes no video input (images and audio only)");
  const DecisionResult img = d1.systemOne(State::None(), questions, {Part::Img(makeImage(640, 480))});
  CHECK_EQ(img.status, Status::InvalidArgument);
  CHECK_EQ(img.error, "an image is 640x480; images must be exactly 512x512 RGB (resize or letterbox before passing it; the SDK does not resample)");
  const DecisionResult aud = d1.systemOne(State::None(), questions, {Part::Aud(makeAudio(2))});
  CHECK_EQ(aud.status, Status::Unsupported);
  CHECK_EQ(aud.error, "this package takes no audio input");
  const DecisionResult ok_img = d1.systemOne(State::Text("x"), questions, {Part::Img(makeImage(512, 512))});
  CHECK_EQ(ok_img.status, Status::Ok);
  CHECK(std::fabs(ok_img.timings.encode - 20.0) < 1e-9);
  Decision d1o = Decision::fromPath(model("d1-omni-2b"));
  CHECK_EQ(d1o.systemOne(State::Text("x"), questions, {Part::Img(makeImage(512, 512)), Part::Aud(makeAudio(3))}).status, Status::Ok);

  // rows: a state that does not fit a pass
  std::string big;
  for (int i = 0; i < 3000; ++i) big += "word ";
  const DecisionResult ce = d1.systemOne(State::Text(big), {{"q", Question::Noul("Yes?")}});
  CHECK_EQ(ce.status, Status::ContextExceeded);
  CHECK(ce.error.rfind("rows: question 0 with its state and media is ", 0) == 0);
  CHECK(ce.error.size() > 30 && ce.error.compare(ce.error.size() - 30, 30, " rows, a d1 pass holds 2048") != 0 ? contains(ce.error, " rows, a d1 pass holds 2048") : true);
  // media rows count: 7 images of 256 rows = 1792 + state
  std::vector<Part> many;
  for (int i = 0; i < 9; ++i) many.push_back(Part::Img(makeImage(512, 512, static_cast<uint8_t>(i))));
  const DecisionResult mm = d1.systemOne(State::Text("x"), {{"a", Question::Noul("A?")}, {"b", Question::Noul("B?")}}, many);
  CHECK_EQ(mm.status, Status::ContextExceeded);
  CHECK(contains(mm.error, "question 0 "));
}

static void testMedia() {
  const fs::path dir = fs::temp_directory_path() / "ixian_mock_selftest";
  fs::create_directories(dir);
  // PPM round trip
  const fs::path ppm = dir / "two_by_two.ppm";
  {
    std::ofstream out(ppm, std::ios::binary);
    out << "P6\n# a comment\n2 2\n255\n";
    const unsigned char px[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30};
    out.write(reinterpret_cast<const char*>(px), 12);
  }
  const Image im = Image::fromFile(ppm.string());
  CHECK_EQ(im.width, 2);
  CHECK_EQ(im.height, 2);
  CHECK_EQ(im.rgb.size(), size_t(12));
  CHECK(im.rgb.size() == 12 && im.rgb[0] == 255 && im.rgb[4] == 255 && im.rgb[11] == 30);
  const fs::path png = dir / "not_a_ppm.png";
  std::ofstream(png, std::ios::binary) << "\x89PNG\r\n";
  expectError([&] { (void)Image::fromFile(png.string()); }, Status::InvalidArgument, "mock: cannot read " + png.string() + " (PPM only)");
  expectError([&] { (void)Image::fromFile((dir / "missing.ppm").string()); }, Status::InvalidArgument,
              "mock: cannot read " + (dir / "missing.ppm").string() + " (PPM only)");
  // fromRgb copies
  std::vector<uint8_t> px(512 * 512 * 3, 7);
  const Image copy = Image::fromRgb(px.data(), 512, 512);
  px[0] = 9;
  CHECK(copy.rgb[0] == 7 && copy.width == 512 && copy.height == 512 && copy.id.empty());

  // fromPcm: stereo 48 kHz -> mono 16 kHz
  std::vector<float> stereo(48000 * 2);
  for (size_t i = 0; i < 48000; ++i) {
    stereo[2 * i] = 0.5f;
    stereo[2 * i + 1] = -0.1f;
  }
  const Audio a = Audio::fromPcm(stereo.data(), 48000, 48000, 2);
  CHECK_EQ(a.sample_rate, 16000);
  CHECK_EQ(a.pcm.size(), size_t(16000));
  CHECK(!a.pcm.empty() && std::fabs(a.pcm[100] - 0.2f) < 1e-6f);
  const Audio same_rate = Audio::fromPcm(stereo.data(), 48000, 16000, 1);
  CHECK_EQ(same_rate.pcm.size(), size_t(48000));
  // fromWav: PCM16 stereo 8 kHz, 0.5 s -> 8000 samples at 16 kHz
  const fs::path wav = dir / "tone.wav";
  {
    const uint32_t rate = 8000, frames = 4000, channels = 2, data_bytes = frames * channels * 2;
    std::ofstream out(wav, std::ios::binary);
    const auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1);
    u16(static_cast<uint16_t>(channels));
    u32(rate);
    u32(rate * channels * 2);
    u16(static_cast<uint16_t>(channels * 2));
    u16(16);
    out.write("data", 4);
    u32(data_bytes);
    for (uint32_t i = 0; i < frames; ++i) {
      u16(static_cast<uint16_t>(16384));
      u16(static_cast<uint16_t>(static_cast<int16_t>(-16384)));
    }
  }
  const Audio w = Audio::fromWav(wav.string());
  CHECK_EQ(w.sample_rate, 16000);
  CHECK_EQ(w.pcm.size(), size_t(8000));
  CHECK(!w.pcm.empty() && std::fabs(w.pcm[10]) < 1e-6f);  // the two channels cancel
  const fs::path bad_wav = dir / "bad.wav";
  std::ofstream(bad_wav, std::ios::binary) << "not a wav file at all";
  expectError([&] { (void)Audio::fromWav(bad_wav.string()); }, Status::InvalidArgument, "cannot read " + bad_wav.string());
  expectError([&] { (void)Audio::fromWav((dir / "missing.wav").string()); }, Status::InvalidArgument,
              "cannot read " + (dir / "missing.wav").string());

  // Video / Part / Message
  const Video v = Video::fromFrames({makeImage(512, 512), makeImage(512, 512)}, 24.0);
  CHECK(v.frames.size() == 2 && v.fps == 24.0 && v.timestamps.empty());
  const Part pv = Part::Vid(v);
  CHECK(pv.type == Part::Type::Video && pv.video && pv.video->frames.size() == 2);
  const auto shared = std::make_shared<const Audio>(makeAudio(1));
  const Part pa = Part::Aud(shared);
  CHECK(pa.type == Part::Type::Audio && pa.audio == shared);
  const Part pi = Part::Img(makeImage(512, 512));
  CHECK(pi.type == Part::Type::Image && pi.image && pi.image->width == 512);
  const Message m = Message::User({Part::Text("a"), pi, Part::Text("b")});
  CHECK(m.role == Role::User && m.content.size() == 3);
  CHECK_EQ(m.text(), "ab");
  CHECK(Message::System("s").role == Role::System && Message::Assistant("x").role == Role::Assistant);
  CHECK_EQ(Message::Assistant("x").text(), "x");
  CHECK(Message::User("u").generated == nullptr);
  CHECK_EQ(std::string(statusName(Status::ContextExceeded)), "ContextExceeded");
  CHECK_EQ(std::string(finishReasonName(FinishReason::StopSequence)), "StopSequence");
  const Error err(Status::DeviceError, "boom", 0x8000040d);
  CHECK(err.status() == Status::DeviceError && err.dspError() == static_cast<int32_t>(0x8000040d) && std::string(err.what()) == "boom");
  fs::remove_all(dir);
}

static void testConversationAndGenerate(LLM& llm) {
  Conversation c(llm, "You are terse.");
  CHECK_EQ(c.messages().size(), size_t(1));
  const Response r = c.send("echo: hi");
  CHECK_EQ(r.text, "hi");
  CHECK_EQ(c.messages().size(), size_t(3));
  CHECK(c.messages()[2].role == Role::Assistant && c.messages()[2].generated != nullptr);
  const Response r2 = c.send(std::vector<Part>{Part::Text("echo: yo")});
  CHECK_EQ(r2.text, "yo");
  CHECK(r2.usage.cached_rows > 0);
  CHECK_EQ(c.messages().size(), size_t(5));
  // a failed turn is not kept
  CHECK_EQ(c.send("error: Internal").status, Status::Internal);
  CHECK_EQ(c.messages().size(), size_t(5));
  c.clear();
  CHECK_EQ(c.messages().size(), size_t(1));
  CHECK(c.messages()[0].role == Role::System);

  LLM::Options o;
  o.system_prompt = "Be brief.";
  LLM g = LLM::fromPath(model("lfm3-1.7b"), o);
  std::string sunk;
  g.setStreamSink([&](const std::string& piece) { sunk += piece; });
  CHECK_EQ(g.generate("echo: one"), "one");
  CHECK_EQ(sunk, "one");
  CHECK_EQ(g.generate("echo: two"), "two");
  CHECK_EQ(sunk, "onetwo");
  // the conversation is kept (system + 2 turns), reset() drops it
  CHECK_EQ(g.generate("echo: three"), "three");
  g.reset();
  CHECK_EQ(g.generate("echo: four"), "four");
  g.reload();
}

// ---------------------------------------------------------------------------------------------------------------

template <class F>
static void run(const char* name, F&& f) {
  const int before = g_failed;
  std::printf("test %-28s ", name);
  std::fflush(stdout);
  f();
  std::printf("%s\n", g_failed == before ? "ok" : "FAILED");
}

int main(int argc, char** argv) {
  if (argc > 1) {
    g_models = argv[1];
  } else {
    const fs::path beside_source = fs::path(__FILE__).parent_path() / "models";
    g_models = fs::is_directory(beside_source) ? beside_source.string() : "models";
  }
  try {
    run("load", testLoad);
    LLM llm = LLM::fromPath(model("lfm3-1.7b"));
    LLM omni = LLM::fromPath(model("lfm3-omni-2b"));
    LLM audio = LLM::fromPath(model("lfm3-audio-2b"));
    run("tokenizer", [&] { testTokenizer(llm); });
    run("template", [&] { testTemplate(llm, omni, audio); });
    run("inspect", [&] { testInspect(llm, omni); });
    run("echo and directives", [&] { testEchoAndDirectives(llm); });
    run("max_tokens and stop", [&] { testLengthAndStop(llm); });
    run("thinking", [&] { testThinking(llm, audio); });
    run("cancel", [&] { testCancel(llm); });
    run("prefix cache", [&] { testPrefixCache(llm, omni); });
    run("validation", [&] { testValidation(llm, omni, audio); });
    run("speech", [&] { testSpeech(audio, omni); });
    run("busy", testBusy);
    run("decision", testDecision);
    run("media", testMedia);
    run("conversation and generate", [&] { testConversationAndGenerate(llm); });
  } catch (const Error& e) {
    std::fprintf(stderr, "unexpected Error %s: %s\n", statusName(e.status()), e.what());
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "unexpected exception: %s\n", e.what());
    return 2;
  }
  std::printf("mock_selftest: %d checks passed, %d failed\n", g_passed, g_failed);
  return g_failed ? 1 : 0;
}
