# Mock Ixian Hexagon engine

`ixian_mock.cpp` implements the public SDK headers (`zetic_types.h`, `zetic_media.h`, `zetic_selection.h`,
`zetic_llm.h`, `zetic_decision.h`) **without an NPU**, so `ixian-server` and its test-suite build and run on
any Linux host (x86-64 included). It is a development double: deterministic, fast, and it reproduces the SDK's
error semantics. It is never shipped to the IQ9.

Build the server against it with `-DIXIAN_SERVER_MOCK_ENGINE=ON`. The headers still come from a real SDK
tarball (`IXIAN_SDK_DIR`, or `tools/server/.sdk/<sdk>`; see `scripts/fetch-sdk.sh`): the mock defines the
symbols, the SDK defines the ABI, and nothing is copied or edited.

## Mock model packages

A mock package is a directory holding one `manifest.json` per target (`<dir>/v73/manifest.json`), or a
single `manifest.json` directly in the directory (then it is the `v73` package). `LLM::fromPath` /
`Decision::fromPath` resolve the target like the SDK: `Options::target` (`"v73-1nsp"`), else `v73`.

```json
{
  "mock": true,
  "model_id": "LiquidAI/LFM3-Omni-2B",
  "revision": "mock",
  "kind": "llm",
  "capabilities": {"text": true, "image": true, "audio": true, "video": true, "speech_out": true},
  "max_context": 2048,
  "generation": {"temperature": 0.0, "top_k": 0, "top_p": 1.0, "repetition_penalty": 1.0},
  "audio_output": {"temperature": 0.4, "top_k": -1, "repetition_penalty": 1.2, "repetition_window": 64},
  "chat": {"think_off": true}
}
```

`kind` is `"llm"` or `"decision"`. The packages under `models/`:

| Directory | kind | capabilities |
|---|---|---|
| `models/lfm3-1.7b` | llm | text |
| `models/lfm3-vl-2b` | llm | text, image, video |
| `models/lfm3-audio-2b` | llm | text, audio, speech_out |
| `models/lfm3-omni-2b` | llm | text, image, audio, video, speech_out (also a `v73-1nsp` package) |
| `models/d1-2b` | decision | text, image |
| `models/d1-omni-2b` | decision | text, image, audio |

## Loading

- Missing manifest: `Error(PackageError, "cannot read <path>/manifest.json")`.
- A manifest that is not valid JSON: `Error(PackageError, "cannot parse <path>/manifest.json: <reason>")`.
- `LLM::fromPath` on a decision package: `Error(Unsupported, "<path> is a d1 decision package: load it with Decision::fromPath")`.
- `Decision::fromPath` on an llm package: `Error(Unsupported, "<path> is a generative package: load it with LLM::fromPath")`.
- `Options::target` names a target directory that is not there: `Error(PackageError, "no <target> package in <path>")`.
- `IXIAN_MOCK_FAIL_LOAD=1` in the environment: `Error(DeviceError, "DeviceError: hexrt_lib_open: map: 0x1", 0x1)`.
- Loading sleeps `IXIAN_MOCK_LOAD_MS` milliseconds (default 50) so a "503 Loading model" window exists.
- `fromModelKey` resolves `<cache_dir>/<model_name>/<target>/manifest.json` (`cache_dir` default
  `$HOME/.cache/zetic_ixian`) and otherwise throws `Error(Unsupported, ...)`, like the real SDK of this version.
- The `LogSink` (if set) gets one `Info` line at load: `mock: loaded <model_id> (<target>, <n> NSP)`.

`ModelInfo`: `model_id`, `revision` from the manifest; `target` = `"v73"` or `"v73-1nsp"`; `hexagon_arch` 73;
`nsp_count` = 2 for an llm `v73` package, 1 for `v73-1nsp` and for decision packages; `soc` = `"QCS9075"`;
`package_dir` = the resolved target directory; `package_version` = `"0.2.0"`; `sdk_version` =
`ZETIC_IXIAN_HEXAGON_VERSION`; `runtime_commit` = `"mock"`.

`Capabilities`: from the manifest; `max_context` = `LLMOptions::max_context` when > 0, else the manifest value
(2048 default); `input_sample_rate` 16000 when audio input; `speech_sample_rate` 24000 when `speech_out`;
`decision` = `kind == "decision"`; `multi_turn`, `media_in_followup_turns` true; `max_media_per_turn` -1.

## Tokenizer (deterministic, process-wide)

Fixed special-token ids: `<|startoftext|>` 1 (BOS), `<|im_start|>` 2, `<|im_end|>` 3, `<|endoftext|>` 4,
`<think>` 5, `</think>` 6, `<|vision_start|>` 7, `<|image|>` 8, `<|vision_end|>` 9, `<|video|>` 10,
`<|audio_start|>` 11, `<|audio_end|>` 12, `<|mixed_start|>` 13, `<|text_end|>` 14, `<|mixed_end|>` 15,
`<|audio|>` 16.

Regular pieces: a piece is one of
1. an optional run of spaces/tabs followed by a run of `[A-Za-z0-9_']` characters (so `" Hello"` and `"Hello"`
   are pieces, like a GPT tokenizer),
2. a single `\n`,
3. a run of spaces/tabs at the end of the text or before a `\n`,
4. any other single character (punctuation, one UTF-8 code point for non-ASCII).

Ids are assigned sequentially from 1000 the first time a piece is seen (a process-wide table behind a mutex),
so ids are stable within a process and `detokenize(tokenize(x)) == x`. Unknown ids detokenize to `""`.
`tokenize(text, parse_special)`: with `parse_special` the special-token strings above become their fixed ids,
without it they are split like any text. `tokenize` never adds BOS.

## Chat template (`applyChatTemplate`)

The LFM3 template, for the subset the SDK's `Message` can express:

```
<|startoftext|>
[when the first message is System]  <|im_start|>system\n<text><|im_end|>\n
for every other message:            <|im_start|><role>\n<content><|im_end|>\n
if add_generation_prompt:           <|im_start|>assistant\n   followed by <think> when enable_thinking, else <think></think>
```

Roles are `system`, `user`, `assistant`. Content is the concatenation of the parts in order: text as is; an
image part `<|vision_start|><|image|><|vision_end|>`; a video part `<|vision_start|><|video|><|vision_end|>`;
an audio part `<|vision_start|><|audio|><|vision_end|>`. An assistant message renders as
`<think></think>` + its text unless the text already contains `</think>`. An assistant message with a
`generated` record renders the record's text (same thing when the text is unchanged).

Packages whose template has no think block (`chat.think_off` false: `lfm3-audio-2b`) render neither `<think>` nor
`<think></think>`: the generation prompt ends with `<|im_start|>assistant\n` and an assistant message renders its text
as is; `enable_thinking` is ignored, as the header says.

Rows: every text token is one row; a media placeholder (`<|image|>`, `<|video|>`, `<|audio|>`) counts for the
media's rows instead: an image 256, a video 256 per temporal patch (2 frames per patch after sampling at
`GenerationConfig::video.fps`; an odd count repeats the last frame), an audio clip `ceil(seconds * 12.5)`
(a 30 s clip is 375). `inspect()` returns one `PromptInfo::Chunk` per text run and per media part with these
rows, `rows` = their sum, `cached_rows` as below, `room` = `max_context - rows`.

## Validation in `chat()` (returned in `Response::status`, never thrown)

In this order:
- empty `messages`: `InvalidArgument`, `"messages must not be empty"`.
- a media part on a package without that capability: `Unsupported`, `"this package takes no image input"`
  (`audio`, `video` likewise).
- an image that is not 512 x 512: `InvalidArgument`,
  `"an image is <W>x<H>; images must be exactly 512x512 RGB (resize or letterbox before passing it; the SDK does not resample)"`;
  a video frame likewise with `"a video frame is <W>x<H>; video frames must be exactly 512x512 RGB (resize or letterbox before passing it; the SDK does not resample)"`.
- an audio part longer than 30 s: `InvalidArgument`, `"an audio part is <s> s; one audio part holds up to 30 s"`.
- `Modality::Audio` requested on a package without `speech_out`: `Unsupported`,
  `"this package has no speech output (capabilities.speech_out false)"`.
- `logprobs` together with `Modality::Audio`: `InvalidArgument`, `"logprobs is not available with audio output"`.
- context: with `reserve` = `max_tokens` (or `LLMOptions::max_new_tokens` when -1) plus
  `ceil(max_audio_seconds * 12.5)` when speech is requested, `rows + reserve > max_context` gives
  `ContextExceeded`, `"the request needs <rows> prompt rows + <reserve> reply rows, the context holds <max_context>"`.
- `top_logprobs` outside 0..20: `InvalidArgument`.

## Prefix cache

Each `LLM` keeps `cache_ids`, the ids of the last prompt followed by the ids it generated and `<|im_end|>`.
`cached_rows` = the longest common prefix of `cache_ids` and the new prompt ids when `cache_prompt` is true,
else 0 (media rows count as cached when the media part has the same `id` / content hash). After a reply
`cache_ids` is replaced. `reset()` clears it. A `Message::generated` record holds the ids and the text of a
reply; when the message text is unchanged the record's ids are used instead of re-tokenizing.

## Generation

The reply is driven by the text of the last `User` message (text parts concatenated), with these exact
prefixes as **test directives**:

| User text | Reply |
|---|---|
| `echo: <text>` | exactly `<text>` |
| `repeat: <N> <word>` | `<word>` repeated N times, space separated |
| `think: <text>` | thinking `<text>` (only emitted when `enable_thinking`), then the answer `I thought about it.` |
| `tool: <name> <json object>` | when the system message contains `<tools>`: a tool call `<tool_call>\n<function=<name>>\n<parameter=<k>>\n<v>\n</parameter>\n...</function>\n</tool_call>` with one parameter per key (strings raw, other JSON values dumped); otherwise the text `no tools` |
| `json: <text>` | `{"answer": "<text>"}` |
| `sleep: <ms> <text>` | waits `<ms>` ms before the first token, then behaves like `echo: <text>` |
| `error: <Status>` | returns `Response::status` = that status (`Internal`, `DeviceError`, `Busy`, ...) with `error` = `"mock: <Status>"` |
| `stall` | never finishes on its own: emits one token per 50 ms until `max_tokens`, cancel or a stop sequence |
| anything else | a deterministic word stream (see below) |

Default stream: words from a fixed list of 64 common English words, chosen by a 64-bit LCG seeded with
`hash(rendered prompt) ^ seed` (`seed` 0 means a `std::random_device` seed, so two requests differ). The
reply starts with a capital, ends with `.` and finishes with `FinishReason::Stop` after `24 + (hash % 25)` words,
or earlier with `FinishReason::Length` when `max_tokens` tokens were produced. `temperature` does not change the
stream, but a different `seed` does.

- Tokens are produced one piece at a time through the `StreamCallback` as `StreamEvent::Type::Text` events
  (`text` = the piece, `token` = its id), with a pause of `IXIAN_MOCK_TOKEN_DELAY_MS` ms between tokens
  (default 1) so streams are observable and cancellation can be tested.
- `enable_thinking`: when the package's `chat.think_off` is true (all LFM3 packages but Audio), the thinking
  (`Let me think about this.` or the `think:` text) is streamed first as Text events with `reasoning = true`,
  then `</think>` (id 6, not streamed), then the answer. `Response::reasoning_content` holds the thinking,
  `Response::text` the answer only, `Response::tokens` every id including the thinking and the `</think>`. Thinking
  tokens and the `</think>` count toward `max_tokens` and `usage.completion_tokens`.
- Stop sequences: matched on the detokenized reply; the reply ends right before the match with
  `FinishReason::StopSequence`; the matched text is not streamed, and a piece that may be the start of a stop
  sequence is held back until it is resolved. When the match starts inside a piece, the text is cut there but the
  piece's id stays in `Response::tokens` (and in the cache), as with a real KV cache.
- `logprobs`: every Text event carries a `TokenLogprob` (`logprob` = `-0.05 * (1 + k % 7)`, `top` = up to
  `top_logprobs` pairs starting with the token itself); `Response::logprobs` holds them all.
- Speech (`modalities` contains `Audio`): a 440 Hz sine at 24 kHz, `frames` = `clamp(words * 3, 5, floor(max_audio_seconds * 12.5))`
  code frames of 80 ms (1920 samples). Audio events carry `first_chunk_frames` frames first, then
  `chunk_frames` per event (`AudioChunk::last` on the final one); the samples are collected into
  `Response::audio` when `AudioOutputConfig::collect`. With `{Text, Audio}` the text pieces and the audio
  chunks alternate. `usage.audio_frames` = frames, `timings.audio_seconds` = frames * 0.08,
  `timings.first_audio` and `timings.realtime_factor` (audio seconds / decode wall time) are set. With `{Audio}` alone
  no text is generated: `Response::text` / `tokens` are empty, `usage.completion_tokens` is 0 and `enable_thinking` is
  ignored. `timings.decode_steps` = text tokens + audio frames - 1.
- The callback returning `false`, or `cancel()` from another thread, ends the reply at the next step with
  `FinishReason::Cancelled`; the partial reply stays in the cache and in the response. A `cancel()` while no call is
  in flight is ignored (the flag is cleared when `chat()` starts).
- A second `chat()` while one is running waits when `Options::wait_if_busy` (a mutex), else returns `Busy`.
- A `Done` event with `response` set is sent last, for failed requests (`status` != `Ok`, `Busy` included) too, but
  never after the callback returned `false`.
- `Response::message` = `Message::Assistant(text)` carrying the `generated` record; `usage.prompt_rows`,
  `usage.cached_rows`, `usage.completion_tokens` (answer + thinking tokens); `timings`: `tokenize` 0.1,
  `template_render` 0.05, `prefill` = `5 + 1.5 * (prompt_rows - cached_rows)` ms, `encode` = 20 per media,
  `ttft` = their sum plus the directive sleep, `decode` = wall time of the token loop, `decode_steps` =
  tokens - 1, `tokens_per_second` = `decode_steps / ((decode - callbacks) / 1000)`, `callbacks` = time spent in
  the user callback. `dsp_*` = the same numbers.
- `generate(user_prompt)` appends to an internal conversation (with `Options::system_prompt` first) and
  returns the reply text; `setStreamSink` receives every piece. `Conversation` is a thin wrapper, as the
  header says.
- `reload()` is a no-op; `reset()` drops the cache and the `generate()` conversation.

## Decision (`systemOne`)

- Media: a `Part::Vid` is `Unsupported`, `"d1 takes no video input (images and audio only)"`; an image that is
  not 512 x 512 gives the same `InvalidArgument` message as above; audio on a package without the capability
  is `Unsupported`, `"this package takes no audio input"`. An audio part over 30 s gives the same `InvalidArgument`
  message as `chat()`. An empty question list returns `Ok` with no answers and `passes` 0.
- Rows: state tokens + media rows (image 256, audio `ceil(seconds * 12.5)`) + the question's instruction tokens
  and one row per option; a question over 2048 rows is `ContextExceeded`,
  `"rows: question <i> with its state and media is <N> rows, a d1 pass holds 2048"`.
- Scores: for each option label a deterministic logit in [-3, 3] from a hash of (state text, instructions,
  label); `probs` = softmax, `raw_scores` = the logits, `labels` = the option labels in order.
  - `Noul`: labels `yes`, `no`; `noul` = P(yes); `confidence` = max.
  - `Choice`: `choice` = the argmax label, `confidence` = its probability; `multiple`: `choices` = labels over 0.5.
  - `Score`: `score` = the first level whose cumulative probability reaches 0.5 (the median), `legend` = that
    level's label, `confidence` = max.
- `input_tokens` = state tokens + every question's tokens; `passes` = `ceil(n_questions / set_size)` with
  `set_size` = `Options::set_size` or 16. `timings.encode` = 20 per media, `prefill` = 5 per pass, `ttft` = sum.
- `systemOneBatch` runs the requests one after another; `probabilities()` returns the `probs` of each answer;
  `runRaw` returns `read_ids[i].size()` zeros per slot; `cancel()` is a no-op.
- `State::Json` text is the serialized document (it is tokenized as text).

## Media constructors

- `Image::fromRgb` copies; `Image::fromFile` reads binary PPM (`P6`) only and throws
  `Error(InvalidArgument, "mock: cannot read <path> (PPM only)")` for anything else.
- `Audio::fromPcm` mixes the channels to mono and resamples to 16 kHz (linear interpolation);
  `Audio::fromWav` reads PCM16 / float32 WAV and throws `Error(InvalidArgument, "cannot read <path>")` otherwise.
- `Video::fromFrames` as the header; `Part::*` and `Message::*` factories as the header; `Message::text()`
  concatenates the text parts.

## Self-test

`mock_selftest.cpp` exercises load, tokenizer round-trip, template, streaming with cancel, stop sequences,
thinking, speech, the context error, the decision path, and the `Busy` behaviour. Build and run it with:

```bash
g++ -std=c++17 -O1 -I<sdk>/include -I../vendor/nlohmann -o /tmp/mock_selftest mock_selftest.cpp ixian_mock.cpp -lpthread && /tmp/mock_selftest
```
