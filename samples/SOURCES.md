# Sample inputs: sources and licenses

Small inputs for the SDK examples and the CLI presets (`--data samples`). Every image and video frame is 512 x 512 RGB, the
size the SDK takes; every WAV is 16 kHz mono 16-bit PCM.

| File | Content | Source | License |
|---|---|---|---|
| `cats.png`, `cats.ppm` | two cats on a sofa with two remote controls | "Cats and remote controllers" by DocChewbacca, [Flickr photo 210383891](https://www.flickr.com/photos/st3f4n/210383891/), also COCO val2017 image `000000039769`. Changed: resized from 640 x 480 to 512 x 512 (bicubic, not letterboxed) | [CC BY-SA 2.0](https://creativecommons.org/licenses/by-sa/2.0/) |
| `color.png`, `count.png`, `ocr.png` | synthetic test images: one blue shape; three dots; the word "CAT" | made by ZETIC | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| `clip1/frame_000.png` ... `frame_071.png` | synthetic 3 s video at 24 fps: a red ball rolls from left to right past a blue square | made by ZETIC | CC0 1.0 |
| `jfk.wav` | 11 s from John F. Kennedy's inaugural address (20 January 1961): "And so, my fellow Americans, ask not what your country can do for you, ask what you can do for your country." | the `samples/jfk.wav` file of [whisper.cpp](https://github.com/ggml-org/whisper.cpp), unchanged | public domain (work of the U.S. federal government) |
| `question.wav` | spoken "What is the capital of France?" (2.7 s) | synthesized by ZETIC with the [kakao-enterprise/vits-ljs](https://huggingface.co/kakao-enterprise/vits-ljs) text-to-speech model (MIT; trained on the public-domain LJ Speech dataset) | CC0 1.0 |
| `question_cats.wav` | spoken "How many cats are in this picture?" (2.5 s) | as `question.wav` | CC0 1.0 |
| `jfk_inaugural.txt` | the full text of the same inaugural address | [Wikisource](https://en.wikisource.org/wiki/John_F._Kennedy%27s_Inaugural_Address), wiki markup removed | public domain (work of the U.S. federal government) |
| `jfk_inaugural_ids.txt` | `jfk_inaugural.txt` as LFM3 token ids on one line (1,600 ids, the first is `<\|startoftext\|>`), for `bench text --ids` and `eval ppl --ids` | made by ZETIC with the LFM3 tokenizer | public domain, as the text |
| `requests.jsonl` | four d1 requests for `zetic_d1_cli ask` / `serve`: three support tickets (text and JSON states) and one image request | written by ZETIC; the image request carries `cats.png` as a JPEG data URL | CC0 1.0, except the embedded image (CC BY-SA 2.0, as `cats.png`) |
| `requests_audio.jsonl` | one d1-Omni-2B request with `jfk.wav` as a WAV data URL | written by ZETIC | CC0 1.0; the embedded audio is public domain |

Attribution for the cat photo when you show or share it: "Cats and remote controllers" by DocChewbacca, CC BY-SA 2.0, resized.
