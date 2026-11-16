// ixian_mock.cpp - a host-side double of the Ixian Hexagon SDK (the contract is README.md in this directory).
//
// Defines every symbol of zetic_types.h, zetic_media.h, zetic_selection.h, zetic_llm.h and zetic_decision.h without
// an NPU: a deterministic process-wide tokenizer, the LFM3 chat template, a scripted token stream driven by test
// directives in the last user message, speech as a 440 Hz sine, and d1 answers from a hash. Thread-safety as the
// headers promise: cancel() from any thread, the tokenizer / template helpers concurrently with chat().
//
// Layout: types -> tokenizer -> package manifests -> prompt rendering -> media / messages -> LLM -> Decision.
#include <zetic_decision.h>
#include <zetic_llm.h>
#include <zetic_media.h>
#include <zetic_selection.h>
#include <zetic_types.h>

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace zetic::ixian {
inline namespace hx1 {

// ---------------------------------------------------------------------------------------------------------------
// zetic_types.h
// ---------------------------------------------------------------------------------------------------------------

const char* statusName(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::Unsupported: return "Unsupported";
    case Status::ContextExceeded: return "ContextExceeded";
    case Status::Busy: return "Busy";
    case Status::PackageError: return "PackageError";
    case Status::DeviceError: return "DeviceError";
    case Status::Internal: return "Internal";
  }
  return "Unknown";
}

const char* finishReasonName(FinishReason r) noexcept {
  switch (r) {
    case FinishReason::Stop: return "Stop";
    case FinishReason::Length: return "Length";
    case FinishReason::StopSequence: return "StopSequence";
    case FinishReason::Cancelled: return "Cancelled";
    case FinishReason::ContextFull: return "ContextFull";
    case FinishReason::Error: return "Error";
  }
  return "Unknown";
}

Error::Error(Status s, std::string what, int32_t dsp_error)
    : std::runtime_error(std::move(what)), status_(s), dsp_error_(dsp_error) {}
Status Error::status() const noexcept { return status_; }
int32_t Error::dspError() const noexcept { return dsp_error_; }

namespace detail {
// The record Response::message carries: the ids a reply put into the cache and the text they stand for.
struct Generated {
  std::vector<int32_t> ids;   // every generated id: thinking, </think>, answer (the end-of-turn token excluded)
  std::string text;           // Response::text when the record was made (the "text unchanged" check)
  std::string think_prefix;   // what the generation prompt ended with: "<think>", "<think></think>" or ""
};
}  // namespace detail

namespace {

// ---------------------------------------------------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ULL) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}
uint64_t fnv1a(std::string_view s, uint64_t h = 1469598103934665603ULL) { return fnv1a(s.data(), s.size(), h); }

int envInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  char* end = nullptr;
  const long n = std::strtol(v, &end, 10);
  return (end && *end == '\0') ? static_cast<int>(n) : fallback;
}

void sleepMs(double ms) {
  if (ms > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

// "30", "31.5": seconds with one decimal, a trailing ".0" dropped.
std::string fmtSeconds(double s) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", s);
  std::string r = buf;
  if (r.size() > 2 && r.compare(r.size() - 2, 2, ".0") == 0) r.erase(r.size() - 2);
  return r;
}

bool startsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

int countWords(std::string_view s) {
  int n = 0;
  bool in_word = false;
  for (char c : s) {
    const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
    if (!space && !in_word) ++n;
    in_word = !space;
  }
  return n;
}

// ---------------------------------------------------------------------------------------------------------------
// the tokenizer: fixed special ids, pieces numbered from 1000 in order of first appearance, process-wide
// ---------------------------------------------------------------------------------------------------------------

struct SpecialToken {
  const char* text;
  int32_t id;
};
constexpr SpecialToken kSpecials[] = {
    {"<|startoftext|>", 1}, {"<|im_start|>", 2},    {"<|im_end|>", 3},      {"<|endoftext|>", 4},
    {"<think>", 5},         {"</think>", 6},        {"<|vision_start|>", 7}, {"<|image|>", 8},
    {"<|vision_end|>", 9},  {"<|video|>", 10},      {"<|audio_start|>", 11}, {"<|audio_end|>", 12},
    {"<|mixed_start|>", 13}, {"<|text_end|>", 14},  {"<|mixed_end|>", 15},   {"<|audio|>", 16}};
constexpr int32_t kImEnd = 3, kThinkEnd = 6;
constexpr int32_t kFirstPieceId = 1000;

class Tokenizer {
 public:
  static Tokenizer& instance() {
    static Tokenizer t;
    return t;
  }

  // parse_special: the special-token strings become their fixed ids; the plain text between them is split into
  // pieces independently (so a blank run before a special token is one piece, like one at the end of the text).
  std::vector<int32_t> encode(std::string_view text, bool parse_special) {
    std::vector<int32_t> out;
    std::lock_guard<std::mutex> lock(mu_);
    if (!parse_special) {
      encodePlain(text, out);
      return out;
    }
    size_t seg = 0;
    for (size_t i = 0; i < text.size();) {
      const SpecialToken* sp = text[i] == '<' ? matchSpecial(text, i) : nullptr;
      if (!sp) {
        ++i;
        continue;
      }
      encodePlain(text.substr(seg, i - seg), out);
      out.push_back(sp->id);
      i += std::strlen(sp->text);
      seg = i;
    }
    encodePlain(text.substr(seg), out);
    return out;
  }

  std::string decode(const std::vector<int32_t>& ids) {
    std::string out;
    std::lock_guard<std::mutex> lock(mu_);
    for (int32_t id : ids) out += pieceLocked(id);
    return out;
  }

  std::string piece(int32_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    return std::string(pieceLocked(id));
  }

  // The id of one piece (registered when new). Used for pieces the generator produces directly.
  int32_t idOf(std::string_view piece) {
    std::lock_guard<std::mutex> lock(mu_);
    return idLocked(piece);
  }

 private:
  static const SpecialToken* matchSpecial(std::string_view text, size_t i) {
    for (const auto& sp : kSpecials) {
      const size_t n = std::strlen(sp.text);
      if (text.size() - i >= n && text.compare(i, n, sp.text) == 0) return &sp;
    }
    return nullptr;
  }
  static bool isWord(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '\'';
  }
  static bool isBlank(char c) { return c == ' ' || c == '\t'; }
  static size_t utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 6) return 2;
    if ((c >> 4) == 14) return 3;
    if ((c >> 3) == 30) return 4;
    return 1;  // a stray continuation byte: one byte on its own
  }

  // The piece rules of README.md "Tokenizer".
  void encodePlain(std::string_view s, std::vector<int32_t>& out) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
      size_t j = i;
      while (j < n && isBlank(s[j])) ++j;
      if (j > i) {
        if (j == n || s[j] == '\n') {  // rule 3: blanks at the end of the text or before a newline
          out.push_back(idLocked(s.substr(i, j - i)));
          i = j;
          continue;
        }
        if (isWord(s[j])) {  // rule 1: blanks followed by a word
          size_t k = j;
          while (k < n && isWord(s[k])) ++k;
          out.push_back(idLocked(s.substr(i, k - i)));
          i = k;
          continue;
        }
        out.push_back(idLocked(s.substr(i, 1)));  // rule 4: a blank before punctuation stands alone
        ++i;
        continue;
      }
      if (isWord(s[i])) {  // rule 1 without blanks
        size_t k = i;
        while (k < n && isWord(s[k])) ++k;
        out.push_back(idLocked(s.substr(i, k - i)));
        i = k;
        continue;
      }
      if (s[i] == '\n') {  // rule 2
        out.push_back(idLocked("\n"));
        ++i;
        continue;
      }
      const size_t len = std::min(utf8Len(static_cast<unsigned char>(s[i])), n - i);  // rule 4
      out.push_back(idLocked(s.substr(i, len)));
      i += len;
    }
  }

  int32_t idLocked(std::string_view piece) {
    const auto it = ids_.find(std::string(piece));
    if (it != ids_.end()) return it->second;
    const int32_t id = kFirstPieceId + static_cast<int32_t>(pieces_.size());
    pieces_.emplace_back(piece);
    ids_.emplace(pieces_.back(), id);
    return id;
  }
  std::string_view pieceLocked(int32_t id) const {
    for (const auto& sp : kSpecials)
      if (sp.id == id) return sp.text;
    if (id >= kFirstPieceId && static_cast<size_t>(id - kFirstPieceId) < pieces_.size()) return pieces_[id - kFirstPieceId];
    return {};
  }

  std::mutex mu_;
  std::unordered_map<std::string, int32_t> ids_;
  std::vector<std::string> pieces_;
};

// ---------------------------------------------------------------------------------------------------------------
// mock packages: <dir>/<target>/manifest.json, or <dir>/manifest.json (the v73 package)
// ---------------------------------------------------------------------------------------------------------------

struct Package {
  std::string dir;         // the resolved target directory
  std::string target;      // "v73" or "v73-1nsp"
  std::string model_id, revision, kind;
  bool text = true, image = false, audio = false, video = false, speech_out = false;
  int32_t max_context = 2048;
  bool think_off = true;   // the template has a think block (false: LFM3-Audio-2B, enable_thinking is ignored)
  // The sampling defaults of the manifest: read for completeness, they do not change the mock's stream.
  float temperature = 0.1f, top_p = 1.0f, repetition_penalty = 1.1f;
  int32_t top_k = 50;
  float audio_temperature = -1, audio_repetition_penalty = -1;
  int32_t audio_top_k = -1, audio_repetition_window = -1;
};

Package loadPackage(const std::string& model_path, const std::string& opt_target) {
  const fs::path root(model_path);
  const std::string want = opt_target.empty() ? "v73" : opt_target;
  std::error_code ec;
  fs::path dir;
  if (fs::is_directory(root / want, ec)) dir = root / want;
  else if (want == "v73") dir = root;  // a single manifest.json directly in the directory is the v73 package
  else throw Error(Status::PackageError, "no " + want + " package in " + model_path);

  const fs::path manifest_path = dir / "manifest.json";
  std::ifstream in(manifest_path, std::ios::binary);
  if (!in) throw Error(Status::PackageError, "cannot read " + manifest_path.string());
  json m;
  try {
    in >> m;
  } catch (const std::exception& e) {
    throw Error(Status::PackageError, "cannot parse " + manifest_path.string() + ": " + e.what());
  }
  if (!m.is_object()) throw Error(Status::PackageError, "cannot parse " + manifest_path.string() + ": not a JSON object");

  Package p;
  p.dir = dir.string();
  p.target = want;
  p.model_id = m.value("model_id", std::string("mock/unknown"));
  p.revision = m.value("revision", std::string("mock"));
  p.kind = m.value("kind", std::string("llm"));
  p.max_context = m.value("max_context", 2048);
  if (m.contains("capabilities") && m["capabilities"].is_object()) {
    const json& c = m["capabilities"];
    p.text = c.value("text", true);
    p.image = c.value("image", false);
    p.audio = c.value("audio", false);
    p.video = c.value("video", false);
    p.speech_out = c.value("speech_out", false);
  }
  if (m.contains("generation") && m["generation"].is_object()) {
    const json& g = m["generation"];
    p.temperature = g.value("temperature", p.temperature);
    p.top_k = g.value("top_k", p.top_k);
    p.top_p = g.value("top_p", p.top_p);
    p.repetition_penalty = g.value("repetition_penalty", p.repetition_penalty);
  }
  if (m.contains("audio_output") && m["audio_output"].is_object()) {
    const json& a = m["audio_output"];
    p.audio_temperature = a.value("temperature", -1.0f);
    p.audio_top_k = a.value("top_k", -1);
    p.audio_repetition_penalty = a.value("repetition_penalty", -1.0f);
    p.audio_repetition_window = a.value("repetition_window", -1);
  }
  if (m.contains("chat") && m["chat"].is_object()) p.think_off = m["chat"].value("think_off", true);
  return p;
}

// The part of loading that happens after the manifest: the "503 Loading model" window, the optional DSP failure
// and the one log line.
void finishLoad(const Package& p, const LogSink& log, int32_t nsp) {
  sleepMs(std::max(0, envInt("IXIAN_MOCK_LOAD_MS", 50)));
  if (envInt("IXIAN_MOCK_FAIL_LOAD", 0) == 1) throw Error(Status::DeviceError, "DeviceError: hexrt_lib_open: map: 0x1", 0x1);
  if (log) log(LogLevel::Info, "mock: loaded " + p.model_id + " (" + p.target + ", " + std::to_string(nsp) + " NSP)");
}

ModelInfo makeInfo(const Package& p, int32_t nsp) {
  ModelInfo i;
  i.model_id = p.model_id;
  i.revision = p.revision;
  i.target = p.target;
  i.hexagon_arch = 73;
  i.nsp_count = nsp;
  i.soc = "QCS9075";
  i.package_dir = p.dir;
  i.package_version = "0.2.0";
  i.sdk_version = ZETIC_IXIAN_HEXAGON_VERSION;
  i.runtime_commit = "mock";
  return i;
}

Capabilities makeCaps(const Package& p, int32_t max_context) {
  Capabilities c;
  c.text = p.text;
  c.image = p.image;
  c.audio = p.audio;
  c.video = p.video;
  c.speech_out = p.speech_out;
  c.decision = p.kind == "decision";
  c.multi_turn = true;
  c.media_in_followup_turns = true;
  c.max_media_per_turn = -1;
  c.max_context = max_context;
  c.input_sample_rate = p.audio ? 16000 : 0;
  c.speech_sample_rate = p.speech_out ? 24000 : 0;
  return c;
}

std::string cacheRoot(const std::string& cache_dir) {
  if (!cache_dir.empty()) return cache_dir;
  const char* home = std::getenv("HOME");
  return std::string(home && *home ? home : ".") + "/.cache/zetic_ixian";
}

// ---------------------------------------------------------------------------------------------------------------
// media rows and cache keys
// ---------------------------------------------------------------------------------------------------------------

constexpr int32_t kImageRows = 256;

// ceil(seconds * 12.5) in integer arithmetic (a 30 s clip is 375).
int32_t audioRows(const Audio& a) {
  if (a.sample_rate <= 0 || a.pcm.empty()) return 0;
  const uint64_t num = uint64_t(a.pcm.size()) * 25, den = uint64_t(a.sample_rate) * 2;
  return static_cast<int32_t>((num + den - 1) / den);
}
double audioSeconds(const Audio& a) { return a.sample_rate > 0 ? double(a.pcm.size()) / a.sample_rate : 0.0; }

// Temporal patches: the frames sampled at cfg.fps, two per patch, an odd count repeating the last frame.
int32_t videoPatches(const Video& v, const VideoConfig& cfg) {
  const int32_t n = static_cast<int32_t>(v.frames.size());
  if (n == 0) return 0;
  int32_t sampled = n;
  double src_fps = v.fps;
  if (v.timestamps.size() == v.frames.size() && n > 1) {
    const double span = v.timestamps.back() - v.timestamps.front();
    if (span > 0) src_fps = (n - 1) / span;
  }
  if (src_fps > 0 && cfg.fps > 0) {
    sampled = static_cast<int32_t>(std::floor(n / src_fps * cfg.fps));
    sampled = std::max(sampled, std::min(cfg.min_frames, n));
    sampled = std::min(std::max(sampled, 1), n);
  }
  int32_t patches = (sampled + 1) / 2;
  if (cfg.max_groups > 0) patches = std::min(patches, cfg.max_groups);
  return patches;
}

uint64_t imageKey(const Image& im) {
  if (!im.id.empty()) return fnv1a(im.id);
  uint64_t h = fnv1a(&im.width, sizeof im.width);
  h = fnv1a(&im.height, sizeof im.height, h);
  return fnv1a(im.rgb.data(), im.rgb.size(), h);
}
uint64_t audioKey(const Audio& a) {
  if (!a.id.empty()) return fnv1a(a.id);
  return fnv1a(a.pcm.data(), a.pcm.size() * sizeof(float), fnv1a(&a.sample_rate, sizeof a.sample_rate));
}
uint64_t videoKey(const Video& v) {
  if (!v.id.empty()) return fnv1a(v.id);
  uint64_t h = fnv1a(&v.fps, sizeof v.fps);
  for (const Image& f : v.frames) {
    const uint64_t k = imageKey(f);
    h = fnv1a(&k, sizeof k, h);
  }
  return h;
}
// A media row in the cache: negative, so it never collides with a text id; the same media gives the same cells.
int64_t mediaCell(uint64_t key) { return -static_cast<int64_t>(key >> 1) - 1; }

// ---------------------------------------------------------------------------------------------------------------
// the LFM3 chat template (README.md "Chat template")
// ---------------------------------------------------------------------------------------------------------------

// One piece of a rendered prompt: a text run (with exact ids when it comes from a generated record) or a media part.
struct PromptPiece {
  Part::Type type = Part::Type::Text;
  std::string text;              // the text run, or the media placeholder (<|image|> ...) for applyChatTemplate
  bool has_ids = false;
  std::vector<int32_t> ids;      // has_ids: the exact ids of the run
  const Part* part = nullptr;    // media
};

struct RenderOptions {
  bool add_generation_prompt = true;
  bool enable_thinking = false;
  bool think_off = true;         // the package's chat.think_off
};

const char* roleName(Role r) {
  switch (r) {
    case Role::System: return "system";
    case Role::User: return "user";
    case Role::Assistant: return "assistant";
  }
  return "user";
}

const char* mediaPlaceholder(Part::Type t) {
  switch (t) {
    case Part::Type::Image: return "<|image|>";
    case Part::Type::Video: return "<|video|>";
    case Part::Type::Audio: return "<|audio|>";
    case Part::Type::Text: break;
  }
  return "";
}

class PromptBuilder {
 public:
  void text(std::string_view s) {
    if (s.empty()) return;
    if (!pieces_.empty() && pieces_.back().type == Part::Type::Text && !pieces_.back().has_ids) {
      pieces_.back().text += s;
      return;
    }
    PromptPiece p;
    p.text = std::string(s);
    pieces_.push_back(std::move(p));
  }
  void ids(const std::vector<int32_t>& ids) {
    PromptPiece p;
    p.has_ids = true;
    p.ids = ids;
    p.text = Tokenizer::instance().decode(ids);
    pieces_.push_back(std::move(p));
  }
  void parts(const std::vector<Part>& parts) {
    for (const Part& part : parts) {
      if (part.type == Part::Type::Text) {
        text(part.text);
        continue;
      }
      text("<|vision_start|>");
      PromptPiece p;
      p.type = part.type;
      p.text = mediaPlaceholder(part.type);
      p.part = &part;
      pieces_.push_back(std::move(p));
      text("<|vision_end|>");
    }
  }
  std::vector<PromptPiece> take() { return std::move(pieces_); }

 private:
  std::vector<PromptPiece> pieces_;
};

std::vector<PromptPiece> renderPrompt(const std::vector<Message>& msgs, const RenderOptions& o) {
  PromptBuilder b;
  b.text("<|startoftext|>");
  for (const Message& m : msgs) {
    b.text(std::string("<|im_start|>") + roleName(m.role) + "\n");
    if (m.role == Role::Assistant) {
      const std::string text = m.text();
      if (m.generated && m.generated->text == text) {
        // the turn the SDK produced, unchanged: its exact ids, so the prefix cache matches
        b.text(m.generated->think_prefix);
        b.ids(m.generated->ids);
      } else {
        if (o.think_off && text.find("</think>") == std::string::npos) b.text("<think></think>");
        b.parts(m.content);
      }
    } else {
      b.parts(m.content);
    }
    b.text("<|im_end|>\n");
  }
  if (o.add_generation_prompt) {
    b.text("<|im_start|>assistant\n");
    if (o.think_off) b.text(o.enable_thinking ? "<think>" : "<think></think>");
  }
  return b.take();
}

// The rows of a rendered prompt: one cell per row (a text id, or the media key repeated per media row).
struct PromptRows {
  std::vector<int64_t> cells;
  PromptInfo info;               // chunks and rows; cached_rows / room are the caller's
  std::string text;              // the rendered template
  int32_t n_media = 0;
};

PromptRows layoutPrompt(const std::vector<PromptPiece>& pieces, const VideoConfig& vcfg) {
  PromptRows r;
  Tokenizer& tk = Tokenizer::instance();
  for (const PromptPiece& p : pieces) {
    r.text += p.text;
    PromptInfo::Chunk c;
    c.type = p.type;
    c.first_row = static_cast<int32_t>(r.cells.size());
    if (p.type == Part::Type::Text) {
      const std::vector<int32_t> ids = p.has_ids ? p.ids : tk.encode(p.text, true);
      for (int32_t id : ids) r.cells.push_back(id);
      c.rows = static_cast<int32_t>(ids.size());
      // a record following template text is the same text run
      if (!r.info.chunks.empty() && r.info.chunks.back().type == Part::Type::Text) r.info.chunks.back().rows += c.rows;
      else r.info.chunks.push_back(c);
      continue;
    }
    ++r.n_media;
    int32_t rows = 0;
    uint64_t key = 0;
    switch (p.type) {
      case Part::Type::Image:
        if (p.part->image) {
          rows = kImageRows;
          key = imageKey(*p.part->image);
        }
        break;
      case Part::Type::Video:
        if (p.part->video) {
          rows = kImageRows * videoPatches(*p.part->video, vcfg);
          key = videoKey(*p.part->video);
        }
        break;
      case Part::Type::Audio:
        if (p.part->audio) {
          rows = audioRows(*p.part->audio);
          key = audioKey(*p.part->audio);
        }
        break;
      case Part::Type::Text: break;
    }
    r.cells.insert(r.cells.end(), static_cast<size_t>(rows), mediaCell(key));
    c.rows = rows;
    r.info.chunks.push_back(c);
  }
  r.info.rows = static_cast<int32_t>(r.cells.size());
  return r;
}

int32_t commonPrefix(const std::vector<int64_t>& a, const std::vector<int64_t>& b) {
  const size_t n = std::min(a.size(), b.size());
  size_t i = 0;
  while (i < n && a[i] == b[i]) ++i;
  return static_cast<int32_t>(i);
}

// ---------------------------------------------------------------------------------------------------------------
// test directives (README.md "Generation")
// ---------------------------------------------------------------------------------------------------------------

enum class Directive { None, Echo, Repeat, Think, Tool, Json, Sleep, Error, Stall };

struct Plan {
  Directive kind = Directive::None;
  std::string text;              // echo / json / think text, the tool name, the status name
  std::string arg;               // tool: the JSON object; repeat: the word
  int n = 0;                     // repeat: the count; sleep: the milliseconds
  Status error_status = Status::Internal;
};

// The text after "<key>:" with one leading blank dropped.
std::string afterKey(std::string_view s, size_t key_len) {
  std::string_view rest = s.substr(key_len);
  if (!rest.empty() && rest[0] == ' ') rest.remove_prefix(1);
  return std::string(rest);
}

Plan parseDirective(const std::string& user) {
  Plan p;
  if (trim(user) == "stall") {
    p.kind = Directive::Stall;
  } else if (startsWith(user, "echo:")) {
    p.kind = Directive::Echo;
    p.text = afterKey(user, 5);
  } else if (startsWith(user, "repeat:")) {
    std::istringstream in(afterKey(user, 7));
    long n = 0;
    std::string word;
    if (in >> n >> word && n >= 0) {
      p.kind = Directive::Repeat;
      p.n = static_cast<int>(std::min<long>(n, 100000));
      p.arg = word;
    }
  } else if (startsWith(user, "think:")) {
    p.kind = Directive::Think;
    p.text = afterKey(user, 6);
  } else if (startsWith(user, "tool:")) {
    const std::string rest = afterKey(user, 5);
    const size_t sp = rest.find_first_of(" \t\n");
    p.kind = Directive::Tool;
    p.text = sp == std::string::npos ? rest : rest.substr(0, sp);
    p.arg = sp == std::string::npos ? std::string() : trim(rest.substr(sp));
  } else if (startsWith(user, "json:")) {
    p.kind = Directive::Json;
    p.text = afterKey(user, 5);
  } else if (startsWith(user, "sleep:")) {
    const std::string rest = afterKey(user, 6);
    std::istringstream in(rest);
    long ms = 0;
    if (in >> ms && ms >= 0) {
      p.kind = Directive::Sleep;
      p.n = static_cast<int>(std::min<long>(ms, 600000));
      std::string tail;
      std::getline(in, tail);
      if (!tail.empty() && tail[0] == ' ') tail.erase(0, 1);
      p.text = tail;
    }
  } else if (startsWith(user, "error:")) {
    p.kind = Directive::Error;
    p.text = trim(afterKey(user, 6));
    p.error_status = Status::Internal;
    for (int s = 0; s <= static_cast<int>(Status::Internal); ++s)
      if (p.text == statusName(static_cast<Status>(s))) p.error_status = static_cast<Status>(s);
  }
  return p;
}

// The 64 words of the default stream.
constexpr const char* kWords[64] = {
    "the",   "of",    "and",  "to",    "in",    "is",    "you",  "that",  "it",    "he",   "was",   "for",  "on",
    "are",   "as",    "with", "his",   "they",  "at",    "be",   "this",  "from",  "have", "or",    "one",  "had",
    "by",    "word",  "but",  "not",   "what",  "all",   "were", "we",    "when",  "your", "can",   "said", "there",
    "use",   "an",    "each", "which", "she",   "do",    "how",  "their", "if",    "will", "up",    "other", "about",
    "out",   "many",  "then", "them",  "these", "so",    "some", "her",   "would", "make", "like",  "him"};

uint64_t randomSeed() {
  std::random_device rd;
  return (uint64_t(rd()) << 32) ^ rd();
}

// "<tool_call>\n<function=name>\n<parameter=k>\nv\n</parameter>\n...</function>\n</tool_call>"
std::string renderToolCall(const std::string& name, const std::string& args_json) {
  std::string s = "<tool_call>\n<function=" + name + ">\n";
  const json args = json::parse(args_json, nullptr, false);
  if (args.is_object()) {
    for (const auto& [k, v] : args.items())
      s += "<parameter=" + k + ">\n" + (v.is_string() ? v.get<std::string>() : v.dump()) + "\n</parameter>\n";
  }
  s += "</function>\n</tool_call>";
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// zetic_media.h
// ---------------------------------------------------------------------------------------------------------------

Image Image::fromRgb(const uint8_t* rgb, int32_t width, int32_t height) {
  Image im;
  im.width = width;
  im.height = height;
  if (rgb && width > 0 && height > 0) im.rgb.assign(rgb, rgb + size_t(width) * size_t(height) * 3);
  return im;
}

// Binary PPM (P6, maxval up to 255) only: the server decodes PNG / JPEG itself with stb_image.
Image Image::fromFile(const std::string& path) {
  const auto bad = [&] { return Error(Status::InvalidArgument, "mock: cannot read " + path + " (PPM only)"); };
  std::ifstream in(path, std::ios::binary);
  if (!in) throw bad();
  std::string magic;
  if (!(in >> magic) || magic != "P6") throw bad();
  const auto readInt = [&](int& v) {
    for (;;) {  // blanks and '#' comments between the header fields
      const int c = in.peek();
      if (c == EOF) return false;
      if (std::isspace(c)) {
        in.get();
        continue;
      }
      if (c == '#') {
        std::string line;
        std::getline(in, line);
        continue;
      }
      break;
    }
    return static_cast<bool>(in >> v);
  };
  int w = 0, h = 0, maxval = 0;
  if (!readInt(w) || !readInt(h) || !readInt(maxval)) throw bad();
  if (w <= 0 || h <= 0 || maxval <= 0 || maxval > 255 || size_t(w) * size_t(h) > (1u << 28)) throw bad();
  in.get();  // the single blank after maxval
  Image im;
  im.width = w;
  im.height = h;
  im.rgb.resize(size_t(w) * size_t(h) * 3);
  if (!in.read(reinterpret_cast<char*>(im.rgb.data()), static_cast<std::streamsize>(im.rgb.size()))) throw bad();
  if (maxval != 255)
    for (uint8_t& v : im.rgb) v = static_cast<uint8_t>(int(v) * 255 / maxval);
  return im;
}

Audio Audio::fromPcm(const float* samples, size_t frames, int32_t sample_rate, int32_t channels) {
  if (sample_rate <= 0) throw Error(Status::InvalidArgument, "mock: sample_rate must be positive");
  if (channels <= 0) channels = 1;
  std::vector<float> mono(frames);
  for (size_t i = 0; i < frames; ++i) {
    double s = 0;
    for (int32_t c = 0; c < channels; ++c) s += samples[i * channels + c];
    mono[i] = static_cast<float>(s / channels);
  }
  Audio a;
  a.sample_rate = 16000;
  if (sample_rate == 16000 || frames == 0) {
    a.pcm = std::move(mono);
    return a;
  }
  const size_t out_n = static_cast<size_t>(std::llround(double(frames) * 16000.0 / sample_rate));
  a.pcm.resize(out_n);
  const double step = double(sample_rate) / 16000.0;
  for (size_t i = 0; i < out_n; ++i) {  // linear interpolation
    const double pos = i * step;
    const size_t j = std::min(static_cast<size_t>(pos), frames - 1);
    const size_t k = std::min(j + 1, frames - 1);
    const double frac = pos - static_cast<double>(j);
    a.pcm[i] = static_cast<float>(mono[j] + (mono[k] - mono[j]) * frac);
  }
  return a;
}

// RIFF / WAVE with PCM16 or float32 samples (also behind WAVE_FORMAT_EXTENSIBLE).
Audio Audio::fromWav(const std::string& path) {
  const auto bad = [&] { return Error(Status::InvalidArgument, "cannot read " + path); };
  std::ifstream in(path, std::ios::binary);
  if (!in) throw bad();
  const std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) throw bad();
  const auto u16 = [&](size_t o) { return uint32_t(buf[o]) | uint32_t(buf[o + 1]) << 8; };
  const auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
  uint32_t fmt = 0, channels = 0, rate = 0, bits = 0;
  size_t data_off = 0, data_len = 0;
  bool have_data = false;
  for (size_t pos = 12; pos + 8 <= buf.size();) {
    const size_t body = pos + 8;
    size_t size = u32(pos + 4);
    if (body + size > buf.size()) size = buf.size() - body;  // a truncated last chunk: take what is there
    if (std::memcmp(buf.data() + pos, "fmt ", 4) == 0 && size >= 16) {
      fmt = u16(body);
      channels = u16(body + 2);
      rate = u32(body + 4);
      bits = u16(body + 14);
      if (fmt == 0xFFFE && size >= 26) fmt = u16(body + 24);  // extensible: the sub-format's first two bytes
    } else if (std::memcmp(buf.data() + pos, "data", 4) == 0 && !have_data) {
      data_off = body;
      data_len = size;
      have_data = true;
    }
    pos = body + size + (size & 1);
  }
  if (!have_data || channels == 0 || rate == 0 || rate > 1000000) throw bad();
  std::vector<float> samples;
  if (fmt == 1 && bits == 16) {
    samples.resize(data_len / 2);
    for (size_t i = 0; i < samples.size(); ++i)
      samples[i] = static_cast<int16_t>(u16(data_off + 2 * i)) / 32768.0f;
  } else if (fmt == 3 && bits == 32) {
    samples.resize(data_len / 4);
    if (!samples.empty()) std::memcpy(samples.data(), buf.data() + data_off, samples.size() * 4);
  } else {
    throw bad();
  }
  return fromPcm(samples.data(), samples.size() / channels, static_cast<int32_t>(rate), static_cast<int32_t>(channels));
}

Video Video::fromFrames(std::vector<Image> frames, double fps) {
  Video v;
  v.frames = std::move(frames);
  v.fps = fps;
  return v;
}

Part Part::Text(std::string t) {
  Part p;
  p.type = Type::Text;
  p.text = std::move(t);
  return p;
}
Part Part::Img(Image im) { return Img(std::make_shared<const Image>(std::move(im))); }
Part Part::Img(std::shared_ptr<const Image> im) {
  Part p;
  p.type = Type::Image;
  p.image = std::move(im);
  return p;
}
Part Part::Aud(Audio a) { return Aud(std::make_shared<const Audio>(std::move(a))); }
Part Part::Aud(std::shared_ptr<const Audio> a) {
  Part p;
  p.type = Type::Audio;
  p.audio = std::move(a);
  return p;
}
Part Part::Vid(Video v) { return Vid(std::make_shared<const Video>(std::move(v))); }
Part Part::Vid(std::shared_ptr<const Video> v) {
  Part p;
  p.type = Type::Video;
  p.video = std::move(v);
  return p;
}

Message Message::System(std::string text) {
  Message m;
  m.role = Role::System;
  m.content.push_back(Part::Text(std::move(text)));
  return m;
}
Message Message::User(std::string text) {
  Message m;
  m.role = Role::User;
  m.content.push_back(Part::Text(std::move(text)));
  return m;
}
Message Message::User(std::vector<Part> parts) {
  Message m;
  m.role = Role::User;
  m.content = std::move(parts);
  return m;
}
Message Message::Assistant(std::string text) {
  Message m;
  m.role = Role::Assistant;
  m.content.push_back(Part::Text(std::move(text)));
  return m;
}
std::string Message::text() const {
  std::string s;
  for (const Part& p : content)
    if (p.type == Part::Type::Text) s += p.text;
  return s;
}

// ---------------------------------------------------------------------------------------------------------------
// zetic_llm.h
// ---------------------------------------------------------------------------------------------------------------

struct LLM::Impl {
  Package pkg;
  ModelInfo info;
  Capabilities caps;
  LLMOptions opts;
  std::mutex run_mutex;                       // one chat() at a time
  std::atomic<bool> cancel{false};            // cancel(): read at every generation step
  std::mutex state_mutex;                     // the fields below
  std::vector<int64_t> cache_cells;           // the prefix cache: last prompt + generated ids + <|im_end|>
  std::vector<Message> conversation;          // generate()'s history
  std::function<void(const std::string&)> sink;

  PromptRows layout(const std::vector<Message>& messages, bool enable_thinking, const VideoConfig& video) const {
    RenderOptions ro;
    ro.add_generation_prompt = true;
    ro.enable_thinking = enable_thinking;
    ro.think_off = pkg.think_off;
    return layoutPrompt(renderPrompt(messages, ro), video);
  }
  Response chat(const std::vector<Message>& messages, const GenerationConfig& config, const StreamCallback& cb);
};

LLM::LLM(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LLM::~LLM() = default;
LLM::LLM(LLM&&) noexcept = default;
LLM& LLM::operator=(LLM&&) noexcept = default;

LLM LLM::fromPath(const std::string& model_path, const Options& options) {
  Package p = loadPackage(model_path, options.target);
  if (p.kind == "decision")
    throw Error(Status::Unsupported, model_path + " is a d1 decision package: load it with Decision::fromPath");
  const int32_t nsp = p.target == "v73-1nsp" ? 1 : 2;
  finishLoad(p, options.log, nsp);
  auto impl = std::make_unique<Impl>();
  impl->opts = options;
  impl->info = makeInfo(p, nsp);
  impl->caps = makeCaps(p, options.max_context > 0 ? options.max_context : p.max_context);
  impl->pkg = std::move(p);
  return LLM(std::move(impl));
}

LLM LLM::fromModelKey(const std::string& model_name, const std::string& personal_key, const std::string& native_lib_dir) {
  return fromModelKey(model_name, personal_key, native_lib_dir, Options{}, FetchOptions{});
}
LLM LLM::fromModelKey(const std::string& model_name, const std::string& personal_key, const std::string& native_lib_dir,
                      const Options& options) {
  return fromModelKey(model_name, personal_key, native_lib_dir, options, FetchOptions{});
}
LLM LLM::fromModelKey(const std::string& model_name, const std::string& personal_key, const std::string& native_lib_dir,
                      const Options& options, const FetchOptions& fetch) {
  (void)personal_key;  // never logged, never stored
  Options o = options;
  if (o.native_lib_dir.empty()) o.native_lib_dir = native_lib_dir;
  const std::string root = cacheRoot(fetch.cache_dir) + "/" + model_name;
  const std::string target = o.target.empty() ? "v73" : o.target;
  std::error_code ec;
  if (!fs::is_regular_file(fs::path(root) / target / "manifest.json", ec))
    throw Error(Status::Unsupported, "fromModelKey: " + model_name + " is not in " + root + "/" + target +
                                         "; this version loads from the cache only (download not implemented)");
  return fromPath(root, o);
}

const ModelInfo& LLM::info() const { return impl_->info; }
Capabilities LLM::capabilities() const { return impl_->caps; }

void LLM::cancel() { impl_->cancel.store(true); }

void LLM::reset() {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  impl_->cache_cells.clear();
  impl_->conversation.clear();
}

void LLM::reload() {}

std::string LLM::applyChatTemplate(const std::vector<Message>& messages, const TemplateOptions& o) const {
  RenderOptions ro;
  ro.add_generation_prompt = o.add_generation_prompt;
  ro.enable_thinking = o.enable_thinking;
  ro.think_off = impl_->pkg.think_off;
  std::string out;
  for (const PromptPiece& p : renderPrompt(messages, ro)) out += p.text;
  return out;
}

std::vector<int32_t> LLM::tokenize(std::string_view text, bool parse_special) const {
  return Tokenizer::instance().encode(text, parse_special);
}

std::string LLM::detokenize(const std::vector<int32_t>& ids) const { return Tokenizer::instance().decode(ids); }

PromptInfo LLM::inspect(const std::vector<Message>& messages, const GenerationConfig& config) const {
  const PromptRows pr = impl_->layout(messages, config.enable_thinking, config.video);
  PromptInfo info = pr.info;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    info.cached_rows = config.cache_prompt ? commonPrefix(impl_->cache_cells, pr.cells) : 0;
  }
  info.room = impl_->caps.max_context - info.rows;
  return info;
}

Response LLM::chat(const std::vector<Message>& messages, const GenerationConfig& config, const StreamCallback& on_event) {
  return impl_->chat(messages, config, on_event);
}

// ---------------------------------------------------------------------------------------------------------------
// LLM::chat: validation (README.md "Validation in chat()"), the prompt, then the scripted token loop
// ---------------------------------------------------------------------------------------------------------------

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int32_t kSamplesPerFrame = 1920;  // one 80 ms code frame at 24 kHz

Status checkImage(const Image& im, const char* what, const char* plural, std::string& why) {
  if (im.width != 512 || im.height != 512) {
    why = std::string(what) + " is " + std::to_string(im.width) + "x" + std::to_string(im.height) + "; " + plural +
          " must be exactly 512x512 RGB (resize or letterbox before passing it; the SDK does not resample)";
    return Status::InvalidArgument;
  }
  if (im.rgb.size() != size_t(512) * 512 * 3) {
    why = std::string(what) + " has " + std::to_string(im.rgb.size()) + " bytes of RGB; 512x512 RGB is 786432";
    return Status::InvalidArgument;
  }
  return Status::Ok;
}

// The request checks that need no prompt: empty request, media the package lacks, media shape, output modality.
Status validateRequest(const Package& pkg, const std::vector<Message>& messages, const GenerationConfig& config,
                       std::string& why) {
  if (messages.empty()) {
    why = "messages must not be empty";
    return Status::InvalidArgument;
  }
  for (const Message& m : messages)
    for (const Part& p : m.content) {
      if (p.type == Part::Type::Image && !pkg.image) return (why = "this package takes no image input", Status::Unsupported);
      if (p.type == Part::Type::Audio && !pkg.audio) return (why = "this package takes no audio input", Status::Unsupported);
      if (p.type == Part::Type::Video && !pkg.video) return (why = "this package takes no video input", Status::Unsupported);
    }
  for (const Message& m : messages)
    for (const Part& p : m.content) {
      if (p.type == Part::Type::Image) {
        if (!p.image) return (why = "an image part has no image", Status::InvalidArgument);
        if (Status s = checkImage(*p.image, "an image", "images", why); s != Status::Ok) return s;
      } else if (p.type == Part::Type::Video) {
        if (!p.video || p.video->frames.empty()) return (why = "a video part has no frames", Status::InvalidArgument);
        for (const Image& f : p.video->frames)
          if (Status s = checkImage(f, "a video frame", "video frames", why); s != Status::Ok) return s;
      }
    }
  for (const Message& m : messages)
    for (const Part& p : m.content) {
      if (p.type != Part::Type::Audio) continue;
      if (!p.audio) return (why = "an audio part has no audio", Status::InvalidArgument);
      const double sec = audioSeconds(*p.audio);
      if (sec > 30.0) {
        why = "an audio part is " + fmtSeconds(sec) + " s; one audio part holds up to 30 s";
        return Status::InvalidArgument;
      }
    }
  bool want_audio = false;
  for (Modality md : config.modalities) want_audio = want_audio || md == Modality::Audio;
  if (want_audio && !pkg.speech_out) {
    why = "this package has no speech output (capabilities.speech_out false)";
    return Status::Unsupported;
  }
  if (want_audio && config.logprobs) return (why = "logprobs is not available with audio output", Status::InvalidArgument);
  if (want_audio && !(config.max_audio_seconds > 0)) return (why = "max_audio_seconds must be positive", Status::InvalidArgument);
  return Status::Ok;
}

}  // namespace

Response LLM::Impl::chat(const std::vector<Message>& messages, const GenerationConfig& config, const StreamCallback& cb) {
  const auto t_entry = Clock::now();
  Response r;
  r.message = Message::Assistant("");
  const bool has_cb = static_cast<bool>(cb);
  bool cb_stopped = false;  // the callback returned false: it gets no further event
  double callbacks_ms = 0;
  const auto emit = [&](const StreamEvent& ev) {
    if (!has_cb || cb_stopped) return;
    const auto t0 = Clock::now();
    if (!cb(ev)) cb_stopped = true;
    callbacks_ms += msSince(t0);
  };
  // Done closes every reply the callback still listens to, failed ones included.
  const auto done = [&] {
    StreamEvent ev;
    ev.type = StreamEvent::Type::Done;
    ev.response = &r;
    emit(ev);
  };
  const auto fail = [&](Status s, std::string why) {
    r.status = s;
    r.error = std::move(why);
    r.finish_reason = FinishReason::Error;
    done();
  };

  std::unique_lock<std::mutex> run(run_mutex, std::defer_lock);
  if (opts.wait_if_busy) {
    run.lock();
  } else if (!run.try_lock()) {
    fail(Status::Busy, "another call is in flight on this object (Options::wait_if_busy is false)");
    return r;
  }
  cancel.store(false);

  // ---- validation; nothing is thrown
  std::string why;
  if (const Status s = validateRequest(pkg, messages, config, why); s != Status::Ok) {
    fail(s, why);
    return r;
  }
  bool want_text = false, want_audio = false;
  for (Modality md : config.modalities) (md == Modality::Audio ? want_audio : want_text) = true;
  if (!want_text && !want_audio) want_text = true;

  const PromptRows pr = layout(messages, config.enable_thinking, config.video);
  const int32_t max_tokens = config.max_tokens > 0 ? config.max_tokens : (opts.max_new_tokens > 0 ? opts.max_new_tokens : 256);
  const int32_t audio_reserve = want_audio ? static_cast<int32_t>(std::ceil(double(config.max_audio_seconds) * 12.5)) : 0;
  const int32_t reserve = max_tokens + audio_reserve;
  if (pr.info.rows + reserve > caps.max_context) {
    fail(Status::ContextExceeded, "the request needs " + std::to_string(pr.info.rows) + " prompt rows + " +
                                      std::to_string(reserve) + " reply rows, the context holds " +
                                      std::to_string(caps.max_context));
    return r;
  }
  if (config.top_logprobs < 0 || config.top_logprobs > 20) {
    fail(Status::InvalidArgument, "top_logprobs must be between 0 and 20");
    return r;
  }

  int32_t cached = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (config.cache_prompt) cached = commonPrefix(cache_cells, pr.cells);
  }
  r.usage.prompt_rows = pr.info.rows;
  r.usage.cached_rows = cached;

  // ---- the directive in the last user message
  std::string last_user;
  for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    if (it->role == Role::User) {
      last_user = it->text();
      break;
    }
  const Plan plan = parseDirective(last_user);
  if (plan.kind == Directive::Error) {
    fail(plan.error_status, "mock: " + plan.text);
    return r;
  }

  Timings& t = r.timings;
  t.tokenize = 0.1;
  t.template_render = 0.05;
  t.prefill = 5 + 1.5 * (pr.info.rows - cached);
  t.encode = 20.0 * pr.n_media;
  t.dsp_prefill = t.prefill;
  t.dsp_encode = t.encode;
  const int sleep_ms = plan.kind == Directive::Sleep ? plan.n : 0;
  t.ttft = t.tokenize + t.template_render + t.prefill + t.encode + sleep_ms;
  t.ttft_module = t.ttft - t.tokenize - t.template_render;

  // ---- the reply text
  Tokenizer& tk = Tokenizer::instance();
  const bool thinking = want_text && config.enable_thinking && pkg.think_off;
  const std::string think_prefix = pkg.think_off ? (config.enable_thinking ? "<think>" : "<think></think>") : "";
  std::string thinking_text, answer_text;
  bool stall = false;
  switch (plan.kind) {
    case Directive::Echo:
    case Directive::Sleep: answer_text = plan.text; break;
    case Directive::Repeat:
      for (int i = 0; i < plan.n; ++i) {
        if (i) answer_text += ' ';
        answer_text += plan.arg;
      }
      break;
    case Directive::Think:
      thinking_text = plan.text;
      answer_text = "I thought about it.";
      break;
    case Directive::Tool: {
      bool tools = false;
      for (const Message& m : messages)
        if (m.role == Role::System && m.text().find("<tools>") != std::string::npos) tools = true;
      answer_text = tools ? renderToolCall(plan.text, plan.arg) : "no tools";
      break;
    }
    case Directive::Json: answer_text = "{\"answer\": " + json(plan.text).dump() + "}"; break;
    case Directive::Stall: stall = true; break;
    case Directive::Error: break;  // handled above
    case Directive::None: {        // the default stream: 24 + hash % 25 words from the LCG, then "."
      const uint64_t h = fnv1a(pr.text);
      uint64_t x = h ^ (config.seed ? config.seed : randomSeed());
      const int n_words = 24 + static_cast<int>(h % 25);
      for (int i = 0; i < n_words; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        std::string w = kWords[(x >> 33) % 64];
        if (i == 0) w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
        else answer_text += ' ';
        answer_text += w;
      }
      answer_text += '.';
      break;
    }
  }
  if (thinking && thinking_text.empty()) thinking_text = "Let me think about this.";
  if (!thinking) thinking_text.clear();
  const int spoken_words = countWords(answer_text);
  if (!want_text) {  // {Audio}: speech only, no text tokens
    answer_text.clear();
    stall = false;
  }

  struct Tok {
    int32_t id = -1;
    std::string piece;
    size_t k = 0;  // its index among the generated tokens
  };
  const auto toks = [&](const std::string& text) {
    std::vector<Tok> v;
    for (int32_t id : tk.encode(text, false)) v.push_back(Tok{id, tk.piece(id), 0});
    return v;
  };
  const std::vector<Tok> think_toks = toks(thinking_text), answer_toks = toks(answer_text);
  const Tok stall_tok{tk.idOf(" stall"), " stall", 0};

  // ---- speech: a 440 Hz sine, first_chunk_frames then chunk_frames per Audio event
  int32_t frames = 0;
  if (want_audio) {
    const int32_t max_frames = static_cast<int32_t>(std::floor(double(config.max_audio_seconds) * 12.5));
    frames = std::max(std::min(spoken_words * 3, max_frames), std::min(5, max_frames));
  }
  std::vector<float> audio_all;
  int32_t frames_sent = 0;
  int64_t samples_sent = 0;
  double first_audio_ms = -1;
  const auto emitAudio = [&]() -> bool {  // one chunk; false when the clip is complete
    if (frames_sent >= frames) return false;
    const int32_t want = frames_sent == 0 ? config.audio.first_chunk_frames : config.audio.chunk_frames;
    const int32_t n = std::clamp(want, 1, frames - frames_sent);
    std::vector<float> pcm(size_t(n) * kSamplesPerFrame);
    for (size_t i = 0; i < pcm.size(); ++i)
      pcm[i] = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * double(samples_sent + int64_t(i)) / 24000.0));
    if (config.audio.collect) audio_all.insert(audio_all.end(), pcm.begin(), pcm.end());
    AudioChunk ch;
    ch.pcm = pcm.data();
    ch.samples = pcm.size();
    ch.sample_rate = 24000;
    ch.first_sample = samples_sent;
    ch.last = frames_sent + n == frames;
    frames_sent += n;
    samples_sent += static_cast<int64_t>(pcm.size());
    if (first_audio_ms < 0) first_audio_ms = msSince(t_entry);
    StreamEvent ev;
    ev.type = StreamEvent::Type::Audio;
    ev.audio = &ch;
    emit(ev);
    return true;
  };

  const auto logprobOf = [&](const Tok& tok) {
    TokenLogprob lp;
    lp.token = tok.id;
    lp.piece = tok.piece;
    lp.logprob = -0.05f * static_cast<float>(1 + tok.k % 7);
    for (int32_t j = 0; j < config.top_logprobs; ++j)
      lp.top.emplace_back(j == 0 ? tok.id : tk.idOf(std::string(" ") + kWords[(tok.k + size_t(j)) % 64]),
                          lp.logprob - 0.5f * static_cast<float>(j));
    return lp;
  };
  // One Text event (the whole piece, or the part of it before a stop sequence); with {Text, Audio} an audio chunk
  // follows every answer piece.
  const auto emitText = [&](const Tok& tok, std::string_view text, bool reasoning) {
    StreamEvent ev;
    ev.type = StreamEvent::Type::Text;
    ev.text = text;
    ev.token = tok.id;
    ev.reasoning = reasoning;
    TokenLogprob lp;
    if (config.logprobs) {
      lp = logprobOf(tok);
      r.logprobs.push_back(lp);
      ev.logprob = &lp;
    }
    emit(ev);
    if (want_audio && !reasoning) emitAudio();
  };

  // ---- stop sequences: generated pieces wait in `pending` while they may start a match
  struct Held {
    Tok tok;
    bool reasoning;
  };
  std::vector<Held> pending;
  std::string pending_text;
  std::string phase_text;  // the thinking or the answer so far, pending included
  const auto flushPending = [&](size_t n) {
    size_t bytes = 0;
    for (size_t i = 0; i < n; ++i) {
      emitText(pending[i].tok, pending[i].tok.piece, pending[i].reasoning);
      bytes += pending[i].tok.piece.size();
    }
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n));
    pending_text.erase(0, bytes);
  };
  const auto checkStops = [&]() -> bool {  // true: a stop sequence matched, the reply ends right before it
    size_t match = std::string::npos;
    for (const std::string& s : config.stop) {
      if (s.empty()) continue;
      const size_t pos = pending_text.find(s);
      if (pos != std::string::npos && pos < match) match = pos;
    }
    if (match != std::string::npos) {
      size_t off = 0;
      for (const Held& h : pending) {
        const size_t end = off + h.tok.piece.size();
        if (end <= match) {
          emitText(h.tok, h.tok.piece, h.reasoning);
        } else {
          if (off < match) emitText(h.tok, std::string_view(pending_text).substr(off, match - off), h.reasoning);
          break;
        }
        off = end;
      }
      phase_text.erase(phase_text.size() - (pending_text.size() - match));
      pending.clear();
      pending_text.clear();
      return true;
    }
    size_t hold = 0;  // the longest tail of pending_text that begins a stop sequence
    for (const std::string& s : config.stop) {
      if (s.empty()) continue;
      for (size_t len = std::min(s.size() - 1, pending_text.size()); len > hold; --len)
        if (pending_text.compare(pending_text.size() - len, len, s, 0, len) == 0) {
          hold = len;
          break;
        }
    }
    const size_t keep_from = pending_text.size() - hold;
    size_t n = 0, off = 0;
    while (n < pending.size() && off + pending[n].tok.piece.size() <= keep_from) off += pending[n++].tok.piece.size();
    flushPending(n);
    return false;
  };

  // ---- the directive sleep, in slices so cancel() is seen
  if (sleep_ms > 0) {
    const auto until = Clock::now() + std::chrono::milliseconds(sleep_ms);
    while (!cancel.load() && Clock::now() < until)
      sleepMs(std::min(10.0, std::chrono::duration<double, std::milli>(until - Clock::now()).count()));
  }

  // ---- the token loop
  enum class Phase { Think, Answer };
  Phase phase = thinking ? Phase::Think : Phase::Answer;
  std::string thinking_out, answer_out;
  std::vector<int32_t> tokens;
  FinishReason finish = FinishReason::Stop;
  size_t ti = 0, ai = 0, k = 0;
  const int delay_ms = std::max(0, envInt("IXIAN_MOCK_TOKEN_DELAY_MS", 1));
  bool first = true;
  const auto t_loop = Clock::now();
  for (;;) {
    if (static_cast<int32_t>(k) >= max_tokens) {
      finish = FinishReason::Length;
      break;
    }
    if (cancel.load() || cb_stopped) {
      finish = FinishReason::Cancelled;
      break;
    }
    Tok tok;
    bool reasoning = false;
    if (phase == Phase::Think) {
      if (ti < think_toks.size()) {
        tok = think_toks[ti++];
        reasoning = true;
      } else {  // the block closes: what is held is streamed, </think> itself is not
        flushPending(pending.size());
        thinking_out = phase_text;
        phase_text.clear();
        tokens.push_back(kThinkEnd);
        ++k;
        phase = Phase::Answer;
        continue;
      }
    } else if (stall) {
      tok = stall_tok;
    } else if (ai < answer_toks.size()) {
      tok = answer_toks[ai++];
    } else {
      finish = FinishReason::Stop;
      break;
    }
    if (!first) sleepMs(stall ? 50 : delay_ms);
    first = false;
    tok.k = k;
    tokens.push_back(tok.id);
    ++k;
    pending.push_back(Held{tok, reasoning});
    pending_text += tok.piece;
    phase_text += tok.piece;
    if (checkStops()) {
      finish = FinishReason::StopSequence;
      break;
    }
  }
  if (finish != FinishReason::StopSequence) flushPending(pending.size());
  (phase == Phase::Think ? thinking_out : answer_out) = phase_text;

  // the audio the text did not interleave ({Audio}: all of it)
  while (want_audio && !cancel.load() && !cb_stopped && emitAudio()) sleepMs(delay_ms);
  if (want_audio && frames_sent < frames) finish = FinishReason::Cancelled;

  // ---- timings and the response
  t.decode = msSince(t_loop);
  t.decode_steps = std::max(0, static_cast<int32_t>(tokens.size()) + frames_sent - 1);
  t.callbacks = callbacks_ms;
  const double busy_ms = t.decode - t.callbacks;
  t.tokens_per_second = busy_ms > 0 ? t.decode_steps / (busy_ms / 1000.0) : 0;
  t.dsp_decode = t.decode;
  if (want_audio) {
    t.first_audio = first_audio_ms < 0 ? 0 : first_audio_ms;
    t.audio_seconds = frames_sent * 0.08;
    t.realtime_factor = t.decode > 0 ? t.audio_seconds / (t.decode / 1000.0) : 0;
    r.usage.audio_frames = frames_sent;
  }
  r.finish_reason = finish;
  r.text = answer_out;
  r.reasoning_content = thinking_out;
  r.tokens = tokens;
  r.audio = std::move(audio_all);
  r.usage.completion_tokens = static_cast<int32_t>(tokens.size());
  auto gen = std::make_shared<detail::Generated>();
  gen->ids = tokens;
  gen->text = r.text;
  gen->think_prefix = think_prefix;
  r.message = Message::Assistant(r.text);
  r.message.generated = gen;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    cache_cells = pr.cells;
    cache_cells.insert(cache_cells.end(), tokens.begin(), tokens.end());
    cache_cells.push_back(kImEnd);
  }
  done();
  return r;
}

// ---------------------------------------------------------------------------------------------------------------
// generate() / Conversation
// ---------------------------------------------------------------------------------------------------------------

std::string LLM::generate(const std::string& user_prompt) {
  std::vector<Message> history;
  std::function<void(const std::string&)> sink;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->conversation.empty() && !impl_->opts.system_prompt.empty())
      impl_->conversation.push_back(Message::System(impl_->opts.system_prompt));
    impl_->conversation.push_back(Message::User(user_prompt));
    history = impl_->conversation;
    sink = impl_->sink;
  }
  StreamCallback cb;
  if (sink)
    cb = [&sink](const StreamEvent& ev) {
      if (ev.type == StreamEvent::Type::Text) sink(std::string(ev.text));
      return true;
    };
  const Response r = chat(history, GenerationConfig{}, cb);
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  if (r.status == Status::Ok) impl_->conversation.push_back(r.message);
  else if (!impl_->conversation.empty()) impl_->conversation.pop_back();  // the turn that failed
  return r.text;
}

void LLM::setStreamSink(std::function<void(const std::string& piece)> sink) {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  impl_->sink = std::move(sink);
}

Conversation::Conversation(LLM& llm, std::string system) : llm_(&llm) {
  if (!system.empty()) messages_.push_back(Message::System(std::move(system)));
}

Response Conversation::send(std::vector<Part> user, const GenerationConfig& config, const StreamCallback& on_event) {
  messages_.push_back(Message::User(std::move(user)));
  Response r = llm_->chat(messages_, config, on_event);
  if (r.status == Status::Ok) messages_.push_back(r.message);
  else messages_.pop_back();
  return r;
}

Response Conversation::send(std::string user_text, const GenerationConfig& config, const StreamCallback& on_event) {
  return send(std::vector<Part>{Part::Text(std::move(user_text))}, config, on_event);
}

const std::vector<Message>& Conversation::messages() const { return messages_; }

void Conversation::clear() {
  std::vector<Message> fresh;
  if (!messages_.empty() && messages_[0].role == Role::System) fresh.push_back(messages_[0]);
  messages_ = std::move(fresh);
}

// ---------------------------------------------------------------------------------------------------------------
// zetic_decision.h
// ---------------------------------------------------------------------------------------------------------------

Question Question::Noul(std::string instructions) {
  Question q;
  q.type = Type::Noul;
  q.instructions = std::move(instructions);
  return q;
}
Question Question::Choice(std::string instructions, std::vector<std::pair<std::string, std::string>> criteria,
                          bool multiple) {
  Question q;
  q.type = Type::Choice;
  q.instructions = std::move(instructions);
  q.criteria = std::move(criteria);
  q.multiple = multiple;
  return q;
}
Question Question::Score(std::string instructions, std::vector<std::string> levels) {
  Question q;
  q.type = Type::Score;
  q.instructions = std::move(instructions);
  for (std::string& l : levels) q.criteria.emplace_back(std::move(l), std::string());
  return q;
}

State State::None() { return State{}; }
State State::Text(std::string s) {
  State st;
  st.kind = Kind::Text;
  st.value = std::move(s);
  return st;
}
State State::Json(std::string json_text) {
  State st;
  st.kind = Kind::Json;
  st.value = std::move(json_text);
  return st;
}

namespace {

std::vector<std::string> optionLabels(const Question& q) {
  if (q.type == Question::Type::Noul) return {"yes", "no"};
  std::vector<std::string> labels;
  for (const auto& c : q.criteria) labels.push_back(c.first);
  if (q.type == Question::Type::Choice && !q.abstain.empty()) labels.push_back(q.abstain);
  return labels;
}

// A deterministic logit in [-3, 3) per option from (state text, instructions, label); probs = softmax.
Answer answerFor(const std::string& state_text, const Question& q, const std::vector<std::string>& labels) {
  Answer a;
  a.type = q.type;
  a.labels = labels;
  std::vector<double> logits;
  for (const std::string& label : labels) {
    const uint64_t h = fnv1a(state_text + '\x1f' + q.instructions + '\x1f' + label);
    logits.push_back(-3.0 + 6.0 * double(h >> 11) / 9007199254740992.0);
  }
  if (logits.empty()) return a;
  const double mx = *std::max_element(logits.begin(), logits.end());
  double sum = 0;
  for (double l : logits) sum += std::exp(l - mx);
  size_t best = 0;
  for (size_t i = 0; i < logits.size(); ++i) {
    a.probs.push_back(std::exp(logits[i] - mx) / sum);
    a.raw_scores.push_back(static_cast<float>(logits[i]));
    if (a.probs[i] > a.probs[best]) best = i;
  }
  a.confidence = a.probs[best];
  switch (q.type) {
    case Question::Type::Noul: a.noul = a.probs[0]; break;
    case Question::Type::Choice:
      a.choice = labels[best];
      if (q.multiple)
        for (size_t i = 0; i < labels.size(); ++i)
          if (a.probs[i] > 0.5) a.choices.push_back(labels[i]);
      break;
    case Question::Type::Score: {  // the median level
      double cum = 0;
      a.score = static_cast<int32_t>(labels.size()) - 1;
      for (size_t i = 0; i < labels.size(); ++i) {
        cum += a.probs[i];
        if (cum >= 0.5) {
          a.score = static_cast<int32_t>(i);
          break;
        }
      }
      a.legend = labels[static_cast<size_t>(a.score)];
      break;
    }
  }
  return a;
}

}  // namespace

struct Decision::Impl {
  Package pkg;
  ModelInfo info;
  Capabilities caps;
  DecisionOptions opts;
  std::mutex run_mutex;
};

Decision::Decision(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Decision::~Decision() = default;
Decision::Decision(Decision&&) noexcept = default;
Decision& Decision::operator=(Decision&&) noexcept = default;

Decision Decision::fromPath(const std::string& model_path, const Options& options) {
  Package p = loadPackage(model_path, options.target);
  if (p.kind != "decision") throw Error(Status::Unsupported, model_path + " is a generative package: load it with LLM::fromPath");
  finishLoad(p, options.log, 1);
  auto impl = std::make_unique<Impl>();
  impl->opts = options;
  impl->info = makeInfo(p, 1);
  impl->caps = makeCaps(p, p.max_context);
  impl->pkg = std::move(p);
  return Decision(std::move(impl));
}

Decision Decision::fromModelKey(const std::string& model_name, const std::string& personal_key, const Options& options,
                                const FetchOptions& fetch) {
  (void)personal_key;  // never logged, never stored
  const std::string root = cacheRoot(fetch.cache_dir) + "/" + model_name;
  const std::string target = options.target.empty() ? "v73" : options.target;
  std::error_code ec;
  if (!fs::is_regular_file(fs::path(root) / target / "manifest.json", ec))
    throw Error(Status::Unsupported, "fromModelKey: " + model_name + " is not in " + root + "/" + target +
                                         "; this version loads from the cache only (download not implemented)");
  return fromPath(root, options);
}

const ModelInfo& Decision::info() const { return impl_->info; }
Capabilities Decision::capabilities() const { return impl_->caps; }

DecisionResult Decision::systemOne(const State& state, const std::vector<std::pair<std::string, Question>>& questions,
                                   const std::vector<Part>& media) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> run(im.run_mutex);
  DecisionResult out;
  const auto fail = [&](Status s, std::string why) {
    out.status = s;
    out.error = std::move(why);
    return out;
  };

  // ---- media: images (512 x 512) and, on d1-Omni, audio; never video
  int32_t media_rows = 0, n_media = 0;
  for (const Part& p : media) {
    switch (p.type) {
      case Part::Type::Video: return fail(Status::Unsupported, "d1 takes no video input (images and audio only)");
      case Part::Type::Image: {
        if (!im.pkg.image) return fail(Status::Unsupported, "this package takes no image input");
        if (!p.image) return fail(Status::InvalidArgument, "an image part has no image");
        std::string why;
        if (const Status s = checkImage(*p.image, "an image", "images", why); s != Status::Ok) return fail(s, why);
        media_rows += kImageRows;
        ++n_media;
        break;
      }
      case Part::Type::Audio: {
        if (!im.pkg.audio) return fail(Status::Unsupported, "this package takes no audio input");
        if (!p.audio) return fail(Status::InvalidArgument, "an audio part has no audio");
        const double sec = audioSeconds(*p.audio);
        if (sec > 30.0)
          return fail(Status::InvalidArgument, "an audio part is " + fmtSeconds(sec) + " s; one audio part holds up to 30 s");
        media_rows += audioRows(*p.audio);
        ++n_media;
        break;
      }
      case Part::Type::Text: break;  // text belongs in the state
    }
  }

  // ---- rows: the state, the media and one question must fit a pass
  Tokenizer& tk = Tokenizer::instance();
  const std::string state_text = state.kind == State::Kind::None ? std::string() : state.value;
  const int32_t state_tokens = static_cast<int32_t>(tk.encode(state_text, false).size());
  int32_t input_tokens = state_tokens + media_rows;  // the rows the pass holds, media included
  std::vector<std::vector<std::string>> labels;
  for (size_t i = 0; i < questions.size(); ++i) {
    const Question& q = questions[i].second;
    labels.push_back(optionLabels(q));
    const int32_t q_tokens = static_cast<int32_t>(tk.encode(q.instructions, false).size() + labels.back().size());
    const int32_t rows = state_tokens + media_rows + q_tokens;
    if (rows > im.caps.max_context)
      return fail(Status::ContextExceeded, "rows: question " + std::to_string(i) + " with its state and media is " +
                                               std::to_string(rows) + " rows, a d1 pass holds " +
                                               std::to_string(im.caps.max_context));
    input_tokens += q_tokens;
  }
  for (size_t i = 0; i < questions.size(); ++i)
    out.answers.emplace_back(questions[i].first, answerFor(state_text, questions[i].second, labels[i]));

  const int32_t set_size = im.opts.set_size > 0 ? im.opts.set_size : 16;
  out.passes = questions.empty() ? 0 : static_cast<int32_t>((questions.size() + set_size - 1) / set_size);
  out.input_tokens = input_tokens;
  out.timings.encode = 20.0 * n_media;
  out.timings.prefill = 5.0 * out.passes;
  out.timings.ttft = out.timings.encode + out.timings.prefill;
  out.timings.ttft_module = out.timings.ttft;
  out.timings.dsp_encode = out.timings.encode;
  out.timings.dsp_prefill = out.timings.prefill;
  return out;
}

std::vector<DecisionResult> Decision::systemOneBatch(const std::vector<DecisionRequest>& requests) {
  std::vector<DecisionResult> out;
  out.reserve(requests.size());
  for (const DecisionRequest& rq : requests) out.push_back(systemOne(rq.state, rq.questions, rq.media));
  return out;
}

std::vector<std::vector<double>> Decision::probabilities(const DecisionRequest& request) {
  std::vector<std::vector<double>> out;
  for (const auto& [name, answer] : systemOne(request.state, request.questions, request.media).answers) {
    (void)name;
    out.push_back(answer.probs);
  }
  return out;
}

std::vector<std::vector<float>> Decision::runRaw(const RawPass& pass) {
  std::vector<std::vector<float>> out;
  for (const auto& ids : pass.read_ids) out.emplace_back(ids.size(), 0.0f);
  return out;
}

void Decision::cancel() {}

}  // namespace hx1
}  // namespace zetic::ixian
