# ixian-server: the llama-server API on the Ixian Hexagon SDK

`ixian-server` serves the LFM3 and d1 models of the [Ixian Hexagon SDK](../../README.md) over HTTP with the API of
[llama.cpp's `llama-server`](../../reference/llama.cpp/tools/server/README.md): the same paths, the same request and
response JSON, the same streaming format, the same command-line flags and environment variables. A client written for
`llama-server` (the OpenAI SDKs, the Anthropic SDK, Open WebUI, LangChain, curl scripts, ...) talks to the Dragonwing IQ9's
NPU without a change. The engine underneath is ZETIC's Hexagon runtime through the SDK's public API only
(`zetic_llm.h`, `zetic_decision.h`); the server adds no inference code of its own.

```
                 HTTP (cpp-httplib)                     one inference thread
 clients  ───►  routes: OpenAI chat / completions /    ───►  zetic::ixian::LLM::chat(...)   ───►  Hexagon NPU
                responses / audio, Anthropic messages,        zetic::ixian::Decision::systemOne
                /v1/systemone (d1), tokenize, props,          (one request at a time, a FIFO of tasks,
                metrics, slots, the web page                    streamed back through a channel)
```

## What works

| Endpoint | llama-server | ixian-server | Notes |
|---|---|---|---|
| `POST /v1/chat/completions`, `/chat/completions` | yes | **yes** | text, images, audio, video in; text and **speech out** (`modalities`, `audio`); tools; thinking; logprobs; `n`; stop; SSE |
| `POST /completion`, `/completions`, `/v1/completions` | yes | **yes** | the prompt becomes one user turn (see [Differences](#differences-from-llama-server)); token arrays, `multimodal_data`, batches of prompts |
| `POST /v1/responses`, `/responses` | yes | **yes** | converted to chat completions, streamed as Responses events |
| `POST /v1/messages`, `/v1/messages/count_tokens` | yes | **yes** | Anthropic Messages API: system, images, tools, thinking, streaming |
| `POST /v1/audio/transcriptions` | yes | **yes** | multipart `file` (WAV / MP3 / FLAC, any rate), `language`, `prompt`, `json` / `text` / `verbose_json`, streaming |
| `POST /v1/audio/speech`, `/tts` | yes | **yes** | LFM3-Audio-2B / Omni-2B: 24 kHz WAV or PCM, streamed WAV with an unknown size, voices `female` / `male` |
| `POST /v1/systemone` | yes | **yes** | d1 decisions: `choice` / `score` / `noul`, JSON states, images and (d1-Omni) audio, chat-message states |
| `POST /tokenize`, `/detokenize`, `/apply-template` | yes | **yes** | `add_special`, `parse_special`, `with_pieces` |
| `POST /v1/chat/completions/input_tokens`, `/v1/responses/input_tokens` | yes | **yes** | counted with the SDK's `inspect` (media rows included) |
| `GET /health`, `/v1/health`, `/props`, `/models`, `/v1/models`, `/metrics`, `/slots` | yes | **yes** | one slot; `/metrics` keeps the `llamacpp:` names and adds `ixian:` counters |
| `POST /slots/0?action=erase` | yes | **yes** | drops the KV and prefix caches |
| `POST /slots/0?action=save\|restore` | yes | 501 | the NPU KV cache cannot be saved |
| `POST /embeddings`, `/v1/embeddings`, `/rerank`, `/infill` | yes | 501 | not in the SDK |
| `GET/POST /lora-adapters` | yes | `[]` / 501 | no adapters |
| `WS /v1/realtime` | yes | 501 | planned; voice chat works through `/v1/chat/completions` with `input_audio` and `modalities` |
| `GET /` (web page) | yes | **yes** | a small built-in chat page; `--path DIR` serves your own files (for example the llama.cpp web UI build) |

Everything else of the request body is accepted with llama-server's semantics: `max_tokens` / `n_predict`, `temperature`,
`top_k`, `top_p`, `min_p`, `repeat_penalty`, `repeat_last_n`, `seed`, `stop`, `logprobs` / `top_logprobs` / `n_probs`,
`post_sampling_probs`, `stream_options.include_usage`, `timings_per_token`, `return_progress`, `return_tokens`,
`response_fields`, `cache_prompt`, `chat_template_kwargs`, `reasoning_format`, `reasoning_effort`, `tools`, `tool_choice`,
`parallel_tool_calls`, `response_format` / `json_schema` (see below), `add_generation_prompt`. Parameters the NPU runtime
cannot act on (`presence_penalty`, `logit_bias`, `mirostat`, `dry_*`, `xtc_*`, ...) are accepted and ignored (a debug log
line says so), so existing clients keep working.

## Build

The SDK is binary-only for the IQ9 (aarch64). Three ways to build:

**On the IQ9** (the board has `g++`; `sudo apt install -y cmake` once), against the SDK tarball you extracted:

```bash
git clone --depth 1 https://github.com/zetic-ai/IxianHexagon
cmake -S IxianHexagon/tools/server -B build-server -DIXIAN_SDK_DIR=$PWD/ixian-hexagon-0.2.2-linux-arm64-qcom-iq9
cmake --build build-server -j
build-server/bin/ixian-server -m models/lfm3-omni-2b --host 0.0.0.0 --port 8080
```

`IXIAN_SDK_DIR` can be left out when the SDK sits in `tools/server/.sdk/` (`tools/server/scripts/fetch-sdk.sh` downloads
and verifies a release there). The binary needs `libzetic_ixian_hexagon.so` and `libhexrt_v73_skel.so` at run time: it
looks next to itself and in `../lib`, else set `LD_LIBRARY_PATH=<sdk>/lib`, or copy the two libraries next to the binary.

**Cross-compiling** on an x86-64 Debian / Ubuntu host (`sudo apt install g++-aarch64-linux-gnu`):

```bash
cmake -S tools/server -B build-aarch64 \
    -DCMAKE_TOOLCHAIN_FILE=tools/server/cmake/aarch64-linux-gnu.toolchain.cmake \
    -DIXIAN_SDK_DIR=tools/server/.sdk/ixian-hexagon-0.2.2-linux-arm64-qcom-iq9
cmake --build build-aarch64 -j
scp build-aarch64/bin/ixian-server iq9:ixh/ixian-hexagon-0.2.2-linux-arm64-qcom-iq9/bin/
```

The link uses a stub `libcdsprpc.so.1` (Qualcomm's FastRPC library only exists on the board); the real one is used at run time.

**On any host, without an NPU**, against the mock engine (`tools/server/mock/`, which implements the SDK headers for
development and for the test-suite):

```bash
tools/server/scripts/fetch-sdk.sh                     # the headers come from the real SDK tarball
cmake -S tools/server -B tools/server/build-mock -DIXIAN_SERVER_MOCK_ENGINE=ON
cmake --build tools/server/build-mock -j
tools/server/build-mock/bin/ixian-server -m tools/server/mock/models/lfm3-omni-2b
```

Options: `-DIXIAN_SERVER_OPENSSL=ON` enables HTTPS (`--ssl-key-file`, `--ssl-cert-file`) and https media URLs when OpenSSL
is installed; `-DIXIAN_SERVER_BUILD_TESTS=OFF` skips the C++ self-tests.

## Run

```bash
ixian-server -m models/lfm3-omni-2b                       # 127.0.0.1:9931, like llama-server
ixian-server -m models/lfm3-omni-2b --host 0.0.0.0 --port 8080 --api-key sk-local -c 8192 --metrics
ixian-server -m models/lfm3-1.7b --target v73-1nsp        # the one-NSP package
ixian-server -m models/d1-omni-2b                         # a d1 package: /v1/systemone (detected; or --model-type decision)
```

The model directory is the Hugging Face download of the SDK README (one subdirectory per target, `v73/`). `ixian-server
--help` lists every flag; the llama-server names apply (`-m`, `-c`, `-n`, `--temp`, `--host`, `--port`, `--api-key`,
`--alias`, `--metrics`, `--slots`, `--no-ui`, `--path`, `--cors-origins`, `--timeout`, `--sse-ping-interval`,
`--reasoning`, `--reasoning-format`, `--chat-template-kwargs`, `--media-path`, `--media-allow-private-urls`, ...) and
their `LLAMA_ARG_*` environment variables. Flags for GPU layers, batch sizes, draft models, LoRA, KV cache types and the
like are accepted and ignored with a note, so a launch script written for `llama-server` runs unchanged.

Flags of this server only:

| Flag | Meaning |
|---|---|
| `--target v73\|v73-1nsp` | the package target (default: detect, both NSPs) |
| `--model-type auto\|llm\|decision` | what the package is (default: detect) |
| `--image-fit letterbox\|stretch\|crop` | how an image of another size reaches the model's 512 x 512 (default: letterbox, black bars) |
| `--native-lib-dir DIR` | where `libhexrt_v73_skel.so` lives (default: next to the SDK library) |
| `--verify` | check every package file's SHA-256 at load |
| `--warmup` / `--no-warmup` | the first-run DSP setup at load (default: on) |
| `--host-affinity MASK` | CPU affinity of the SDK's host threads |
| `--serialize-helpers` | run `/tokenize`, `/apply-template` and token counting on the inference thread instead of concurrently |

### Examples

```bash
# chat, streamed
curl -N localhost:9931/v1/chat/completions -H 'Content-Type: application/json' -d '{
  "messages": [{"role": "user", "content": "What is the capital of France?"}], "stream": true, "max_tokens": 64}'

# an image (any size: the server letterboxes it to 512 x 512)
curl localhost:9931/v1/chat/completions -H 'Content-Type: application/json' -d "{
  \"messages\": [{\"role\": \"user\", \"content\": [
    {\"type\": \"image_url\", \"image_url\": {\"url\": \"data:image/png;base64,$(base64 -w0 samples/cats.png)\"}},
    {\"type\": \"text\", \"text\": \"How many cats?\"}]}]}"

# a spoken question (WAV / MP3 / FLAC; clips over 30 s are split)
curl localhost:9931/v1/chat/completions -H 'Content-Type: application/json' -d "{
  \"messages\": [{\"role\": \"user\", \"content\": [
    {\"type\": \"input_audio\", \"input_audio\": {\"data\": \"$(base64 -w0 samples/question.wav)\", \"format\": \"wav\"}}]}]}"

# speech output (LFM3-Audio-2B / Omni-2B): OpenAI's audio/speech, 24 kHz WAV
curl localhost:9931/v1/audio/speech -H 'Content-Type: application/json' \
  -d '{"input": "The quick brown fox jumps over the lazy dog.", "voice": "female"}' -o reply.wav

# voice chat: text and speech together, the speech as base64 in message.audio (or delta.audio when streamed)
curl localhost:9931/v1/chat/completions -H 'Content-Type: application/json' -d '{
  "messages": [{"role": "user", "content": "Tell me a one-line joke."}],
  "modalities": ["text", "audio"], "audio": {"voice": "male", "format": "wav"}}'

# transcription
curl localhost:9931/v1/audio/transcriptions -F file=@samples/jfk.wav -F language=en

# thinking (off by default on the NPU; per request or with --reasoning on)
curl localhost:9931/v1/chat/completions -H 'Content-Type: application/json' -d '{
  "messages": [{"role": "user", "content": "Is 97 prime?"}], "chat_template_kwargs": {"enable_thinking": true}}'

# d1 decisions (a d1 package): the samples/requests.jsonl lines are valid bodies
curl localhost:9931/v1/systemone -H 'Content-Type: application/json' -d '{
  "state": "I was charged twice and my parcel never arrived. Please refund one charge.",
  "questions": {
    "refund":  {"type": "noul",   "instructions": "Is the customer asking for a refund?"},
    "team":    {"type": "choice", "instructions": "Which team should handle this?", "criteria": {"billing": "Charges, refunds, invoices", "shipping": "Delivery and parcels"}},
    "urgency": {"type": "score",  "instructions": "How urgent is this ticket?", "criteria": ["low", "medium", "high"]}}}'

# Anthropic Messages API (the Anthropic SDK works with base_url=http://localhost:9931)
curl localhost:9931/v1/messages -H 'Content-Type: application/json' -d '{
  "model": "x", "max_tokens": 64, "messages": [{"role": "user", "content": "Hello"}]}'
```

Python, with the OpenAI client:

```python
from openai import OpenAI
client = OpenAI(base_url="http://iq9:9931/v1", api_key="none")
for chunk in client.chat.completions.create(model="lfm3-omni-2b", stream=True,
        messages=[{"role": "user", "content": "Describe the Dragonwing IQ9 in one sentence."}]):
    print(chunk.choices[0].delta.content or "", end="", flush=True)
```

## Differences from llama-server

The NPU runtime shapes a few behaviours; they are documented here, nothing is silent.

- **One request at a time.** The SDK holds one model and one KV cache per process. `--parallel` is accepted but the server
  has one slot; other requests wait in order (`/slots` and `/metrics` show the queue as `requests_deferred`). Prefix caching
  works across requests like llama-server's `cache_prompt`, and a re-sent history whose assistant replies came from this
  server reuses the exact cached tokens (the SDK's `generated` record is kept for the last 64 replies).
- **No grammar sampler.** The DSP samples on its own (greedy argmax, or the package's sampler), so `grammar` returns 501.
  `response_format` / `json_schema` are honoured at the prompt level: the schema is rendered into the system prompt and the
  model is asked for JSON only. `tool_choice: required` / a named tool are prompt-level instructions too.
- **Tool calls** follow the LFM3 template's XML form (`<tool_call><function=name><parameter=k>v</parameter>...`). The server
  renders `<tools>` into the system prompt, assistant `tool_calls` and `tool` results into the turns, and parses the model's
  blocks back into OpenAI `tool_calls` (arguments typed from the tool's JSON schema). Tool results are rendered as a user
  turn holding `<tool_response>`, since the SDK's messages have the system / user / assistant roles only.
- **No assistant prefill.** The runtime always opens a new assistant turn, so a trailing assistant message is a complete turn
  (`--no-prefill-assistant` behaviour) and `continue_final_message` returns 501.
- **`/completion` is a chat turn.** The SDK has no raw-prompt API: the prompt text (or the detokenized token array) becomes a
  user message under the chat template. The `prompt` field of the native response shows what was rendered.
- **Thinking is off by default** (`--reasoning auto` = off): LFM3 reasons when asked, with `chat_template_kwargs.enable_thinking`,
  `--reasoning on` or an Anthropic `thinking` block. `--reasoning-format` works as in llama-server; `reasoning_budget_tokens`
  is accepted without effect.
- **Images are resized by the server.** The SDK takes exactly 512 x 512 RGB. `--image-fit` picks letterbox (default), stretch
  or center-crop; video frames are letterboxed. Video containers are decoded with `ffmpeg` (`--video-ffmpeg-dir`), or send
  `{"type": "video_frames", "frames": [...image urls...], "fps": 2}`.
- **Audio in** is decoded (WAV, MP3, FLAC) and mixed to mono; clips over 30 s are split into several audio parts, as the SDK
  README recommends.
- **Speech out** is 24 kHz mono: `wav` or `pcm` (`pcm16`) in `/v1/audio/speech`; `wav`, `pcm16` or `pcm32` in chat completions
  (`audio.format`). `modalities: ["audio"]` alone speaks the user text (the SDK's TTS mode); `["text", "audio"]` is a voice reply.
  Voices are `female` and `male`; `audio.language` / `--tts-lang` name the language. `logprobs` is not available with audio.
- **`timings`** carry the SDK's stage times (`ttft_ms`, `encode_ms`, `audio_frames`, `realtime_factor`) next to llama-server's
  fields. `predicted_per_second` is the SDK's decode rate with callback time excluded.
- **Embeddings, reranking, infill, LoRA, slot save / restore, the realtime WebSocket** are not available (501).

## Tests

The test-suite is llama.cpp's server test style (pytest), run against the mock engine so it needs no board:

```bash
pip install -r tools/server/tests/requirements.txt
cmake -S tools/server -B tools/server/build-mock -DIXIAN_SERVER_MOCK_ENGINE=ON && cmake --build tools/server/build-mock -j
tools/server/tests/tests.sh                 # or: cd tools/server/tests && pytest -v
```

The suite drives the mock through test directives (a user message `echo: ...` is echoed back, `tool: name {...}` makes a
tool call, ...), so it checks the server, not a model. On the IQ9, `scripts/smoke-test.sh http://127.0.0.1:9931` sends one
real request per capability of the loaded package (chat, streaming, image, audio, transcription, speech, d1) and checks the
answers' shape. The C++ self-tests (`ixsrv-media-selftest`, `ixsrv-mock-selftest`) build with `-DIXIAN_SERVER_BUILD_TESTS=ON` (default).

To ship a build to the board: `cmake --install build-aarch64 --prefix ixian-server-dist` gives `bin/ixian-server`,
`bin/smoke-test.sh`, this README and the licenses; copy the directory next to the SDK (or set `LD_LIBRARY_PATH=<sdk>/lib`).

## Source layout

| File | Content |
|---|---|
| `src/main.cpp` | start the HTTP server (503 while loading), load the package, run |
| `src/server-args.*` | llama-server compatible flags and `LLAMA_ARG_*` environment |
| `src/server-engine.*` | the SDK object, the inference thread, the task queue, slot state, metrics, the reply cache |
| `src/server-chat.*` | OpenAI / Anthropic / Responses bodies to SDK messages, media loading, LFM3 tool rendering and parsing |
| `src/server-task.*` | request parameters and every response shape (final and streamed) |
| `src/server-routes.*` | the endpoints |
| `src/server-decision.*` | `/v1/systemone` |
| `src/server-http.*` | cpp-httplib wiring: middleware (CORS, API key, loading state), the page, chunked streaming |
| `src/server-media.*` | base64, data URLs, image decode and resize (stb_image), WAV / MP3 / FLAC decode (miniaudio), WAV encode |
| `mock/` | the development engine implementing the SDK headers, with mock model packages |
| `tests/` | pytest suite and C++ self-tests |
| `vendor/` | cpp-httplib (MIT), nlohmann/json (MIT), stb_image (public domain / MIT), miniaudio (public domain / MIT-0) |

Parts of the HTTP layer (CORS and host checks, the response and error shapes, the test harness) follow llama.cpp
(MIT, `reference/llama.cpp/LICENSE`).
