import base64

import pytest
import requests
from utils import *

server: ServerProcess


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.omni()


def post_file(path: str, fields: dict | None = None, name: str = "jfk.wav") -> requests.Response:
    with open(sample_path(name), "rb") as f:
        files = {"file": (name, f, "audio/wav")}
        data = {k: str(v) for k, v in (fields or {}).items()}
        return requests.post(server.make_url(path), files=files, data=data, timeout=DEFAULT_REQUEST_TIMEOUT)


@pytest.mark.parametrize("path", ["/v1/audio/transcriptions", "/audio/transcriptions"])
def test_transcription_basic(path: str):
    server.start()
    res = post_file(path, {"model": "whisper-1"})
    assert res.status_code == 200, res.text
    body = res.json()
    assert body["type"] == "transcript.text.done"
    assert isinstance(body["text"], str) and len(body["text"]) > 0
    assert body["usage"]["type"] == "tokens"
    # 11 s of audio = 138 rows, plus the prompt
    assert body["usage"]["input_tokens"] > 138


def test_transcription_prompt_reaches_the_model():
    server.start()
    res = post_file("/v1/audio/transcriptions", {"prompt": "echo: transcribed by the mock"})
    assert res.status_code == 200
    assert res.json()["text"] == "transcribed by the mock"


def test_transcription_language_and_formats():
    server.start()
    res = post_file("/v1/audio/transcriptions", {"language": "ko", "response_format": "json"})
    assert res.status_code == 200
    res = post_file("/v1/audio/transcriptions", {"prompt": "echo: plain", "response_format": "text"})
    assert res.status_code == 200
    assert res.headers["Content-Type"].startswith("text/plain")
    assert res.text.strip() == "plain"
    res = post_file("/v1/audio/transcriptions", {"prompt": "echo: verbose", "response_format": "verbose_json", "language": "en"})
    assert res.status_code == 200
    body = res.json()
    assert body["text"] == "verbose" and body["task"] == "transcribe" and body["language"] == "en"
    res = post_file("/v1/audio/transcriptions", {"response_format": "srt"})
    assert res.status_code == 400


def test_transcription_stream():
    server.start()
    with open(sample_path("jfk.wav"), "rb") as f:
        res = requests.post(server.make_url("/v1/audio/transcriptions"), files={"file": ("jfk.wav", f, "audio/wav")},
                            data={"stream": "true", "prompt": "echo: streamed words here"}, stream=True)
    assert res.status_code == 200
    events = []
    for line in res.iter_lines():
        line = line.decode()
        if line.startswith("data: ") and "[DONE]" not in line:
            events.append(json.loads(line[6:]))
    assert events[-1]["type"] == "transcript.text.done"
    assert events[-1]["text"] == "streamed words here"
    deltas = [e for e in events if e["type"] == "transcript.text.delta"]
    assert len(deltas) > 0
    assert "".join(e["delta"] for e in deltas) == "streamed words here"


def test_transcription_json_body_with_base64_file():
    server.start()
    with open(sample_path("jfk.wav"), "rb") as f:
        b64 = base64.b64encode(f.read()).decode()
    res = server.make_request("POST", "/v1/audio/transcriptions", data={"file": b64, "prompt": "echo: from json"})
    assert res.status_code == 200
    assert res.body["text"] == "from json"
    res = server.make_request("POST", "/v1/audio/transcriptions", data={"file": "data:audio/wav;base64," + b64, "prompt": "echo: from data url"})
    assert res.body["text"] == "from data url"


def test_transcription_no_file():
    server.start()
    res = requests.post(server.make_url("/v1/audio/transcriptions"), data={"model": "x"})
    assert res.status_code == 400
    res = server.make_request("POST", "/v1/audio/transcriptions", data={"model": "x"})
    assert res.status_code == 400


def test_transcription_not_an_audio_file():
    server.start()
    with open(sample_path("cats.png"), "rb") as f:
        res = requests.post(server.make_url("/v1/audio/transcriptions"), files={"file": ("cats.png", f, "image/png")})
    assert res.status_code in (400, 500)


def test_transcription_text_model():
    global server
    server = ServerPreset.text()
    server.start()
    res = post_file("/v1/audio/transcriptions")
    assert res.status_code == 501
