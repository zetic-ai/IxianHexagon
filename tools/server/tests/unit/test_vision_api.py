import base64
import io
import shutil
import struct
import subprocess
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
import requests
from utils import *

server: ServerProcess

IMAGE_ROWS = 256
AUDIO_ROWS_PER_SECOND = 12.5
MARKERS = 8  # the template wraps a media part in a few marker tokens (<|vision_start|> ... <|vision_end|>)


def rows_between(actual: int, base: int, media_rows: int, n_parts: int = 1) -> bool:
    return base + media_rows <= actual <= base + media_rows + MARKERS * n_parts


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.omni()


def ppm_data_url(width: int, height: int) -> str:
    # a synthetic PPM (left half red, right half blue); stb_image decodes PNM
    header = f"P6\n{width} {height}\n255\n".encode()
    rows = bytearray()
    for _ in range(height):
        rows += bytes([255, 0, 0]) * (width // 2) + bytes([0, 0, 255]) * (width - width // 2)
    return "data:image/x-portable-pixmap;base64," + base64.b64encode(header + rows).decode()


def silence_wav_b64(seconds: float, rate: int = 16000) -> str:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\x00\x00" * int(seconds * rate))
    return base64.b64encode(buf.getvalue()).decode()


def chat(messages, **extra):
    return server.make_request("POST", "/chat/completions", data={"max_tokens": 8, "messages": messages, **extra})


def text_only_rows():
    return chat([{"role": "user", "content": [{"type": "text", "text": "What is this:\n"}]}]).body["usage"]["prompt_tokens"]


def test_models_supports_multimodal_capability():
    server.start()
    for path in ["/models", "/v1/models"]:
        res = server.make_request("GET", path)
        model_info = res.body["models"][0]
        assert "completion" in model_info["capabilities"]
        assert "multimodal" in model_info["capabilities"]


def test_vision_chat_completion_512_image():
    server.start()
    base = text_only_rows()
    res = chat([{"role": "user", "content": [
        {"type": "text", "text": "What is this:\n"},
        {"type": "image_url", "image_url": {"url": data_url(sample_path("cats.png"), "image/png")}},
    ]}])
    assert res.status_code == 200
    assert res.body["choices"][0]["message"]["role"] == "assistant"
    assert rows_between(res.body["usage"]["prompt_tokens"], base, IMAGE_ROWS)


def test_vision_image_is_resized_to_512():
    """any image size is accepted: the server letterboxes to the model's 512 x 512 (the SDK itself refuses other sizes)"""
    server.start()
    base = text_only_rows()
    for w, h in [(640, 480), (64, 48), (1024, 1024), (300, 900)]:
        res = chat([{"role": "user", "content": [
            {"type": "image_url", "image_url": {"url": ppm_data_url(w, h)}},
            {"type": "text", "text": "What is this:\n"},
        ]}])
        assert res.status_code == 200, (w, h, res.body)
        assert rows_between(res.body["usage"]["prompt_tokens"], base, IMAGE_ROWS)


@pytest.mark.parametrize("fit", ["letterbox", "stretch", "crop"])
def test_vision_image_fit_modes(fit: str):
    global server
    server.image_fit = fit
    server.start()
    res = chat([{"role": "user", "content": [
        {"type": "image_url", "image_url": {"url": ppm_data_url(640, 480)}},
        {"type": "text", "text": "x"},
    ]}])
    assert res.status_code == 200
    assert server.make_request("GET", "/props").body["ixian"]["image_fit"] == fit


def test_vision_image_url_string_form_and_raw_base64():
    server.start()
    with open(sample_path("cats.png"), "rb") as f:
        raw = base64.b64encode(f.read()).decode()
    res = chat([{"role": "user", "content": [{"type": "image_url", "image_url": raw}, {"type": "text", "text": "x"}]}])
    assert res.status_code == 200
    res = chat([{"role": "user", "content": [{"type": "image_url", "image_url": {"url": raw}}, {"type": "text", "text": "x"}]}])
    assert res.status_code == 200


@pytest.mark.parametrize("image_url", [
    "malformed",
    "data:text/html;base64,aGVsbG8=",
    "data:image/png;base64,bm90YW5pbWFnZQ==",
    "https://127.0.0.1:1/404.png",
])
def test_vision_bad_images(image_url: str):
    server.start()
    res = chat([{"role": "user", "content": [{"type": "text", "text": "x"}, {"type": "image_url", "image_url": {"url": image_url}}]}])
    assert res.status_code != 200
    assert "error" in res.body


def test_vision_multiple_images_and_order():
    server.start()
    base = text_only_rows()
    res = chat([{"role": "user", "content": [
        {"type": "image_url", "image_url": {"url": ppm_data_url(32, 32)}},
        {"type": "text", "text": "and"},
        {"type": "image_url", "image_url": {"url": ppm_data_url(32, 32)}},
        {"type": "text", "text": "What is this:\n"},
    ]}])
    assert res.status_code == 200
    assert res.body["usage"]["prompt_tokens"] > base + 2 * IMAGE_ROWS


def test_audio_input_rows_and_split():
    server.start()
    base = text_only_rows()
    with open(sample_path("jfk.wav"), "rb") as f:
        b64 = base64.b64encode(f.read()).decode()
    res = chat([{"role": "user", "content": [
        {"type": "input_audio", "input_audio": {"data": b64, "format": "wav"}},
        {"type": "text", "text": "What is this:\n"},
    ]}])
    assert res.status_code == 200
    rows = res.body["usage"]["prompt_tokens"] - base
    assert 138 <= rows <= 138 + MARKERS  # ceil(11 s * 12.5) plus the markers
    # 70 s of audio is split into clips of 30 s or less
    res = chat([{"role": "user", "content": [
        {"type": "input_audio", "input_audio": {"data": silence_wav_b64(70), "format": "wav"}},
        {"type": "text", "text": "What is this:\n"},
    ]}])
    assert res.status_code == 200
    assert rows_between(res.body["usage"]["prompt_tokens"], base, 375 + 375 + 125, n_parts=3)


def test_audio_input_other_rates_and_stereo():
    server.start()
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b"\x00\x00\x00\x00" * (44100 * 2))
    res = chat([{"role": "user", "content": [
        {"type": "input_audio", "input_audio": {"data": base64.b64encode(buf.getvalue()).decode(), "format": "wav"}},
        {"type": "text", "text": "x"},
    ]}])
    assert res.status_code == 200
    assert res.body["usage"]["prompt_tokens"] > 25


def test_video_frames_extension():
    server.start()
    base = text_only_rows()
    frame = data_url(sample_path("clip1/frame_000.png"), "image/png")
    res = chat([{"role": "user", "content": [
        {"type": "video_frames", "frames": [frame, frame, frame, frame], "fps": 2},
        {"type": "text", "text": "Describe the video."},
    ]}])
    assert res.status_code == 200, res.body
    # 4 frames = 2 temporal patches of 256 rows (the text differs from the baseline by a few tokens)
    assert res.body["usage"]["prompt_tokens"] >= base + 2 * IMAGE_ROWS - 8


@pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg is needed to decode video containers")
def test_video_url_mp4_via_ffmpeg():
    server.start()
    mp4 = "/tmp/ixian-test-clip.mp4"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-framerate", "24", "-i", sample_path("clip1/frame_%03d.png"), "-pix_fmt", "yuv420p", mp4], check=True)
    res = chat([{"role": "user", "content": [
        {"type": "video_url", "video_url": {"url": data_url(mp4, "video/mp4")}},
        {"type": "text", "text": "Describe the video."},
    ]}])
    assert res.status_code == 200, res.body
    # 3 s at 2 fps = 6 frames = 3 patches
    assert res.body["usage"]["prompt_tokens"] >= 3 * IMAGE_ROWS


def test_media_on_text_model():
    global server
    server = ServerPreset.text()
    server.start()
    res = chat([{"role": "user", "content": [{"type": "image_url", "image_url": {"url": ppm_data_url(32, 32)}}, {"type": "text", "text": "x"}]}])
    assert res.status_code != 200
    res = chat([{"role": "user", "content": [{"type": "input_audio", "input_audio": {"data": silence_wav_b64(1), "format": "wav"}}, {"type": "text", "text": "x"}]}])
    assert res.status_code != 200
    assert server.make_request("GET", "/models").body["models"][0]["capabilities"] == ["completion"]


@pytest.mark.parametrize("allow_private", [False, True])
def test_vision_image_url_private_address(allow_private: bool):
    with open(sample_path("cats.png"), "rb") as f:
        img = f.read()

    class ImageHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.end_headers()
            self.wfile.write(img)

        def log_message(self, format, *args):
            pass

    target = ThreadingHTTPServer(("127.0.0.1", 0), ImageHandler)
    threading.Thread(target=target.serve_forever, daemon=True).start()
    try:
        global server
        server.media_allow_private_urls = allow_private
        server.start()
        res = chat([{"role": "user", "content": [
            {"type": "text", "text": "What is this:\n"},
            {"type": "image_url", "image_url": {"url": f"http://127.0.0.1:{target.server_port}/img.png"}},
        ]}])
        assert res.status_code == (200 if allow_private else 400)
    finally:
        target.shutdown()
        target.server_close()


@pytest.mark.parametrize("media_path, image_url, success", [
    (None, "file://cats.png", False),
    (SAMPLES_DIR, "file://cats.png", True),
    (SAMPLES_DIR, "file:////cats.png", True),
    (SAMPLES_DIR, "file://notfound.png", False),
    (SAMPLES_DIR, "file://../samples/cats.png", False),
])
def test_local_media_file(media_path, image_url, success):
    global server
    server.media_path = media_path
    server.start()
    res = chat([{"role": "user", "content": [{"type": "text", "text": "test"}, {"type": "image_url", "image_url": {"url": image_url}}]}])
    assert res.status_code == (200 if success else 400)


def test_completion_with_multimodal_data():
    global server
    server.env = {"LLAMA_MEDIA_MARKER": "<__media__>"}
    server.start()
    with open(sample_path("cats.png"), "rb") as f:
        b64 = base64.b64encode(f.read()).decode()
    res = server.make_request("POST", "/completions", data={
        "n_predict": 4,
        "prompt": {"prompt_string": "What is this: <__media__>\n", "multimodal_data": [b64]},
    })
    assert res.status_code == 200
    assert res.body["tokens_evaluated"] > IMAGE_ROWS
    assert server.make_request("GET", "/props").body["media_marker"] == "<__media__>"
