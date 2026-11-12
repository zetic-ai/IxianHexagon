<p align="center">
  <img src=".github/assets/zetic-banner.png" alt="ZETIC: More tasks. Done right." width="100%">
</p>

# Ixian Hexagon SDK for the Dragonwing IQ9

[![Release](https://img.shields.io/github/v/release/zetic-ai/IxianHexagon?label=release&color=16867d)](https://github.com/zetic-ai/IxianHexagon/releases)
![Dragonwing IQ9 (QCS9075)](https://img.shields.io/badge/Dragonwing_IQ9_%28QCS9075%29-2b3137)
![Hexagon NPU](https://img.shields.io/badge/Hexagon_NPU-2b3137)
[![Models on Hugging Face](https://img.shields.io/badge/Models-Hugging_Face-FFD21E?logo=huggingface)](https://huggingface.co/collections/ZETIC-ai/lfm3-and-d1-on-dragonwing-iq9-6af400b9da045ed7cfa23b0e)

This repository distributes the Ixian Hexagon SDK for the Qualcomm Dragonwing IQ9 (QCS9075) as binaries.
The SDK runs the LFM3 family and the d1 decision models on the Hexagon NPU of the IQ9 with ZETIC's own Hexagon
runtime. Each SDK version is a GitHub Release; the model packages are on Hugging Face.

**[Releases](https://github.com/zetic-ai/IxianHexagon/releases) · [Models on Hugging Face](https://huggingface.co/collections/ZETIC-ai/lfm3-and-d1-on-dragonwing-iq9-6af400b9da045ed7cfa23b0e) · [zetic.ai](https://zetic.ai)**

| Device | SoC | Package target | OS |
|---|---|---|---|
| Dragonwing IQ9, both NSPs (default) | QCS9075 | `v73` | Ubuntu 24.04 |
| Dragonwing IQ9, one NSP | QCS9075 | `v73-1nsp` (`LLMOptions::target`, CLI `--target`) | the same |

> [!NOTE]
> The SDK runs on the IQ9 only. On any other device, `LLM::fromPath` / `Decision::fromPath` fail with
> `Status::Unsupported`. Packages for any other target or model are refused the same way.

## 1. Download the SDK

Download the two files from the [0.2.1 release](https://github.com/zetic-ai/IxianHexagon/releases/tag/0.2.1), or use
`curl` below. Neither route needs a GitHub account. The `gh release download` command requires `gh auth login` first,
even for this public repository.

```bash
curl -fL -O https://github.com/zetic-ai/IxianHexagon/releases/download/0.2.1/ixian-hexagon-0.2.1-linux-arm64-qcom-iq9.tar.gz
curl -fL -O https://github.com/zetic-ai/IxianHexagon/releases/download/0.2.1/ixian-hexagon-0.2.1-linux-arm64-qcom-iq9.tar.gz.sha256
sha256sum -c ixian-hexagon-0.2.1-linux-arm64-qcom-iq9.tar.gz.sha256      # must print OK
tar xzf ixian-hexagon-0.2.1-linux-arm64-qcom-iq9.tar.gz
```

> [!NOTE]
> 0.2.1 renames the SDK's code names; it has the same functionality as 0.2.0. Code written for 0.2.0 needs:
> `zetic::mlange` → `zetic::ixian`, `find_package(ZeticMlangeHexagon)` → `find_package(IxianHexagon)`,
> `zetic::mlange_hexagon` → `zetic::ixian_hexagon`, `libzetic_mlange_hexagon.so` → `libzetic_ixian_hexagon.so`,
> `ZETIC_MLANGE_HEXAGON_VERSION` → `ZETIC_IXIAN_HEXAGON_VERSION`; the default model cache moves from `~/.cache/zetic_mlange`
> to `~/.cache/zetic_ixian`. The model packages on Hugging Face are unchanged.

<details>
<summary>What's in the SDK</summary>

| Path in the SDK | Content |
|---|---|
| `lib/libzetic_ixian_hexagon.so` | the SDK library (public API only) |
| `lib/libhexrt_v73_skel.so` | the DSP library; the SDK adds its directory to `ADSP_LIBRARY_PATH` |
| `include/` | `zetic_llm.h`, `zetic_media.h`, `zetic_decision.h`, `zetic_selection.h`, `zetic_types.h` |
| `lib/cmake/IxianHexagon/` | CMake package config |
| `bin/zetic_lfm3_cli`, `bin/zetic_d1_cli`, `bin/zetic_quickstart` | test / demo CLIs |
| `examples/quickstart` | example source |
| `README.md` | the SDK guide (the sections below, in more detail) |
| `LICENSES/`, `PROVENANCE.json` | third-party licenses, build provenance (source commit, toolchain versions) |

</details>

The binaries run on the IQ9's Ubuntu (glibc 2.35 or newer) and use the board's
`/usr/lib/aarch64-linux-gnu/libcdsprpc.so.1`.

## 2. Download the models

One Hugging Face repository per model. Each holds one folder per package target (`v73/`, and `v73-1nsp/` for the LFM3
models), so the download is the model directory the SDK opens.

| Model | Hugging Face | Input | Output | API | Targets |
|---|---|---|---|---|---|
| LFM3-1.7B | [ZETIC-ai/LFM3-1.7B-IQ9](https://huggingface.co/ZETIC-ai/LFM3-1.7B-IQ9) | text | text | `LLM` | `v73`, `v73-1nsp` |
| LFM3-VL-2B | [ZETIC-ai/LFM3-VL-2B-IQ9](https://huggingface.co/ZETIC-ai/LFM3-VL-2B-IQ9) | text, image, video | text | `LLM` | `v73`, `v73-1nsp` |
| LFM3-Audio-2B | [ZETIC-ai/LFM3-Audio-2B-IQ9](https://huggingface.co/ZETIC-ai/LFM3-Audio-2B-IQ9) | text, audio | text, speech | `LLM` | `v73`, `v73-1nsp` |
| LFM3-Omni-2B | [ZETIC-ai/LFM3-Omni-2B-IQ9](https://huggingface.co/ZETIC-ai/LFM3-Omni-2B-IQ9) | text, image, audio, video | text, speech | `LLM` | `v73`, `v73-1nsp` |
| d1-2B | [ZETIC-ai/d1-2B-IQ9](https://huggingface.co/ZETIC-ai/d1-2B-IQ9) | text, image | decisions | `Decision` | `v73` |
| d1-Omni-2B | [ZETIC-ai/d1-Omni-2B-IQ9](https://huggingface.co/ZETIC-ai/d1-Omni-2B-IQ9) | text, image, audio | decisions | `Decision` | `v73` |

Use a Python virtual environment so installation also works on Ubuntu 24.04, where system-wide `pip install` is
blocked. On Ubuntu, install `python3-venv` first if it is missing (`sudo apt install -y python3-venv`).

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -U huggingface_hub             # provides the `hf` command
hf download ZETIC-ai/LFM3-1.7B-IQ9 --local-dir models/lfm3-1.7b
hf download ZETIC-ai/LFM3-VL-2B-IQ9 --local-dir models/lfm3-vl-2b
hf download ZETIC-ai/LFM3-Audio-2B-IQ9 --local-dir models/lfm3-audio-2b
hf download ZETIC-ai/LFM3-Omni-2B-IQ9 --local-dir models/lfm3-omni-2b
hf download ZETIC-ai/d1-2B-IQ9 --local-dir models/d1-2b
hf download ZETIC-ai/d1-Omni-2B-IQ9 --local-dir models/d1-omni-2b
```

- Only the two-NSP package: add `--include "v73/*"`. Sign in with `hf auth login` if your account needs access.
- Each package lists its files in `SHA256SUMS`; `info --verify` (below) checks them on the board.
- The model weights are under the LFM Open License v1.0 (Liquid AI); see the `LICENSE` file of each Hugging Face repository.
- All six complete model directories use about 26 GB. Check free space on both the download computer and IQ9 before
  copying them. If the board has less space, copy and test one model directory at a time.

## 3. Run on the board (no build)

In the commands below, `iq9` means your board's SSH destination. Replace it with, for example,
`ubuntu@<your-board-address>`, or configure an SSH alias named `iq9`. SSH access to the board must already work.

```bash
scp -r ixian-hexagon-0.2.1-linux-arm64-qcom-iq9 iq9:zsdk
scp -r models iq9:zsdk/
ssh iq9 'cd zsdk && LD_LIBRARY_PATH=lib bin/zetic_lfm3_cli info --model models/lfm3-1.7b --verify'
ssh iq9 'cd zsdk && LD_LIBRARY_PATH=lib bin/zetic_lfm3_cli chat --model models/lfm3-1.7b "What is the capital of France?"'
ssh iq9 'cd zsdk && LD_LIBRARY_PATH=lib bin/zetic_d1_cli demo --model models/d1-2b'
```

- The SDK picks `v73` (both NSPs) unless `--target v73-1nsp` / `LLMOptions::target` asks for the one-NSP package.
- `info --verify` (both CLIs) checks every package file against its `SHA256SUMS` and the target.
- Other commands: `chat -i` (multi-turn), `chat --media a.png+b.wav "prompt"` (media input), `chat --tts | --voice --out reply.wav`
  (speech output), `zetic_d1_cli info | demo | ask | seq`. Each prints its usage when run without arguments.

## 4. Build your app

Build on the IQ9 itself. Its Ubuntu already has `g++` and the board's `libcdsprpc.so.1`; install CMake once:

```bash
ssh iq9
sudo apt install -y cmake
cd zsdk
cmake -S examples/quickstart -B build-quickstart -DCMAKE_PREFIX_PATH=$PWD/lib/cmake
cmake --build build-quickstart
build-quickstart/quickstart models/lfm3-1.7b "What is the capital of France?"
```

In your own `CMakeLists.txt`:

```cmake
find_package(IxianHexagon 0.2 CONFIG REQUIRED)   # cmake -DCMAKE_PREFIX_PATH=<sdk>/lib/cmake
target_link_libraries(my_app PRIVATE zetic::ixian_hexagon)
```

```cpp
#include <zetic_llm.h>
using namespace zetic::ixian;

LLM llm = LLM::fromPath("/home/user/zsdk/models/lfm3-1.7b");      // picks v73 on the IQ9
std::vector<Message> history{Message::User("What is the capital of France?")};
Response r = llm.chat(history, {}, [](const StreamEvent& ev) {
  if (ev.type == StreamEvent::Type::Text) fwrite(ev.text.data(), 1, ev.text.size(), stdout);
  return true;                                                      // false stops the reply
});
history.push_back(r.message);                                       // follow-ups reuse the cached prefix
history.push_back(Message::User("And its population?"));
Response r2 = llm.chat(history);
```

Keep `lib/libhexrt_v73_skel.so` next to `libzetic_ixian_hexagon.so`, or pass its directory as `LLMOptions::native_lib_dir`
(`Decision::Options::native_lib_dir` for d1). A full example is in `examples/quickstart` of the SDK.

- **Default sampling:** the model's generation config (temperature 0.1, top-k 50, repetition penalty 1.1), sampled on the CPU.
- **Greedy:** set `GenerationConfig::temperature = 0`.
- **Thinking:** `GenerationConfig::enable_thinking` per request (default off). On returns the thinking in
  `Response::reasoning_content` and the answer in `Response::text`.

### d1 decisions

```cpp
#include <zetic_decision.h>
using namespace zetic::ixian;

Decision d1 = Decision::fromPath("/home/user/zsdk/models/d1-2b");
DecisionResult out = d1.systemOne(State::Text("I was charged twice and my parcel never arrived. Please refund one charge."), {
    {"refund",  Question::Noul("Is the customer asking for a refund?")},
    {"team",    Question::Choice("Which team should handle this?", {{"billing", "Charges, refunds, invoices"},
                                                                    {"shipping", "Delivery and parcels"}})},
    {"urgency", Question::Score("How urgent is this ticket?", {"low", "medium", "high"})}});
// out.answers[0].second.noul = P(yes); answers[1].second.choice = "billing"; answers[2].second.legend = "medium"
```

All questions on one state go through one NPU pass (up to 16 per pass). d1 answers three question types, all asked as
text: yes / no (`Question::Noul`), one or several of a list (`Question::Choice`) and a score on a scale
(`Question::Score`). Images (d1-2B, d1-Omni-2B) and audio clips (d1-Omni-2B) are `Part::Img(...)` / `Part::Aud(...)` in
the third argument; they are encoded once per request. Images must be exactly 512 x 512 RGB, as for the LLM.

### Images, audio and video (LFM3-VL-2B, LFM3-Audio-2B, LFM3-Omni-2B)

```cpp
LLM vl = LLM::fromPath("models/lfm3-vl-2b");                       // or lfm3-omni-2b
GenerationConfig greedy; greedy.temperature = 0;
Response r = vl.chat({Message::User({Part::Img(Image::fromFile("cats.png")),   // must be 512 x 512
                                     Part::Text("How many cats are in the image?")})}, greedy);

LLM asr = LLM::fromPath("models/lfm3-audio-2b");
Response t = asr.chat({Message::System("Perform ASR in English."),
                       Message::User({Part::Aud(Audio::fromWav("jfk.wav"))})}, greedy);
```

> [!IMPORTANT]
> **Images and video frames must be exactly 512 x 512 RGB** (LLM and d1 alike). The SDK does no resize, letterbox or
> tiling; any other size returns `Status::InvalidArgument` with, for example, "an image is 1920x1080; images must be
> exactly 512x512 RGB (resize or letterbox before passing it; the SDK does not resample)". The CLIs print the same message. `Image::fromFile` decodes PNG / JPEG / PPM / BMP; `Image::fromRgb` takes 8-bit RGB.

- **Audio:** `Audio::fromWav` or `Audio::fromPcm`; mixed to mono and resampled to 16 kHz. One audio part holds up to 30 s.
- **Video:** `Part::Vid(Video::fromFrames(frames, fps))` with decoded 512 x 512 frames; the SDK samples 2 fps.
- **Follow-up turns** re-send the whole history; the prefix cache keeps the media rows, so a text follow-up does not
  re-encode the media.

### Speech output (LFM3-Audio-2B, LFM3-Omni-2B)

```cpp
LLM m = LLM::fromPath("models/lfm3-audio-2b");                      // or lfm3-omni-2b
GenerationConfig tts;
tts.modalities = {Modality::Audio};                                 // TTS; {Text, Audio} = voice chat
Response r = m.chat({Message::System("Perform TTS in American English using a female voice."),
                     Message::User("The quick brown fox jumps over the lazy dog.")}, tts);
// r.audio: 24 kHz mono float; StreamEvent::Type::Audio streams it chunk by chunk
```

### Context length

| | Default (`max_context = 0`) | Largest `max_context` |
|---|---:|---:|
| `v73` (two NSPs) | 2,048 | 131,072 |
| `v73-1nsp` (one NSP) | 2,048 | 4,096 |

A larger context costs memory; set it only as large as the app needs. d1 models keep their single pass. CLI: `--ctx <positions>`.

## 5. Optional text speed smoke check

The `bench text` CLI needs a text file whose first line is whitespace-separated tokenizer IDs. To make one from the
public LFM3-1.7B package, run this **on the IQ9**, from the `zsdk` directory created above:

```bash
sudo apt install -y python3-venv
python3 -m venv .bench-venv
.bench-venv/bin/pip install tokenizers
.bench-venv/bin/python - <<'PY'
from pathlib import Path
from tokenizers import Tokenizer

tokenizer = Tokenizer.from_file("models/lfm3-1.7b/v73/tokenizer.json")
ids = tokenizer.encode("Paris is in France. " * 400, add_special_tokens=False).ids
Path("public_ids.txt").write_text(" ".join(map(str, ids)) + "\n")
PY
LD_LIBRARY_PATH=lib bin/zetic_lfm3_cli bench text --model models/lfm3-1.7b \
  --ids public_ids.txt --n 1024 --new 256 --reps 1 --emit-module-job public_bench_job.txt
```

The CLI trims the input to approximately 1,024 prompt rows and reports first-token latency and decode tokens/s. This
is a reproducible smoke check, not the workload used for published performance claims. Run it with other NPU users
stopped if you want a useful speed measurement.

## Limits

- One active model per process. To free the NPU, destroy the `LLM` / `Decision` object.
- Tested from `ssh` with the CLIs and the library.

## Licenses

<details>
<summary>Third-party licenses</summary>

Third-party code in the SDK library and the CLIs; the texts are in [`LICENSES/`](LICENSES) here and in the SDK tarball
(start with [`LICENSES/NOTICE.txt`](LICENSES/NOTICE.txt)):

| Component | License | File |
|---|---|---|
| tokenizers-cpp (MLC AI) | Apache-2.0 | `tokenizers-cpp-LICENSE.txt` |
| Hugging Face `tokenizers` Rust crate 0.21.2 and its crates.io dependencies | MIT and / or Apache-2.0 (or a similar permissive choice) per crate | `tokenizers-rust-notices.txt`: every crate with its license, and the license files of each linked crate |
| Oniguruma (in the `onig_sys` crate) | BSD-2-Clause | `tokenizers-rust-notices.txt` |
| esaxx and sais-lite (in the `esaxx-rs` crate) | MIT | `tokenizers-rust-notices.txt` |
| nlohmann/json 3.12.0 | MIT | `nlohmann-json-LICENSE.MIT` |
| stb_image v2.30 | public domain / MIT | `stb_image-LICENSE.txt` |

- The DSP library `lib/libhexrt_v73_skel.so` and the FastRPC stub inside the SDK library are built with the Qualcomm
  Hexagon SDK.
- The LFM3 and d1 model weights are not part of the SDK; they are under the LFM Open License v1.0 (Liquid AI), as stated
  in each Hugging Face repository.

</details>

## Integrity and provenance

<details>
<summary>Release integrity and build provenance</summary>

Every release carries the SDK tarball and its `.sha256`. `PROVENANCE.json` in the tarball records the source commit,
the build infrastructure commit, the tokenizers-cpp pin and the toolchain versions. Binary artifacts are release assets
only; they are never committed to this repository.

</details>

---

<p align="center">Packaged by <a href="https://zetic.ai">ZETIC</a> · <a href="https://ixian.zetic.ai">ixian-bench</a> · ✉️ contact@zetic.ai</p>
