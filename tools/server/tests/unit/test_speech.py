import base64
import struct

import pytest
import requests
from utils import *

server: ServerProcess

BODY = {
    "model": "tts",
    "input": "Hello there.",
    "voice": "female",
    "seed": 42,
    "n_predict": 8,
}


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.omni()


def post_audio(path: str, **extra) -> requests.Response:
    return requests.post(server.make_url(path), json={**BODY, **extra}, timeout=DEFAULT_REQUEST_TIMEOUT)


def assert_wav(res: requests.Response, data_size: int | None = None):
    assert res.status_code == 200, res.text
    assert res.headers["Content-Type"] == "audio/wav"
    assert res.content[:4] == b"RIFF" and res.content[8:12] == b"WAVE"
    assert struct.unpack("<H", res.content[34:36])[0] == 16  # bits per sample
    assert struct.unpack("<I", res.content[24:28])[0] == 24000  # sample rate
    pos = res.content.index(b"data")
    size = struct.unpack("<I", res.content[pos + 4:pos + 8])[0]
    assert size == (len(res.content) - pos - 8 if data_size is None else data_size)
    assert len(res.content) > pos + 8


@pytest.mark.parametrize("path", ["/v1/audio/speech", "/audio/speech"])
def test_speech_default_format_is_wav(path: str):
    server.start()
    assert_wav(post_audio(path))


def test_speech_wav_same_as_tts():
    server.start()
    res = post_audio("/v1/audio/speech", response_format="wav")
    assert_wav(res)
    assert res.content == post_audio("/tts").content


def test_speech_pcm_is_signed_16_bit():
    server.start()
    res = post_audio("/v1/audio/speech", response_format="pcm")
    assert res.status_code == 200
    assert res.headers["Content-Type"] == "audio/pcm;rate=24000;encoding=signed-int;bits=16"
    ref = post_audio("/tts", response_format="pcm")
    assert ref.headers["Content-Type"] == "audio/pcm;rate=24000;encoding=float;bits=32"
    samples = struct.unpack(f"<{len(res.content) // 2}h", res.content)
    floats = struct.unpack(f"<{len(ref.content) // 4}f", ref.content)
    assert len(samples) == len(floats) > 0
    assert any(v != 0 for v in samples)
    for s, f in zip(samples, floats):
        assert abs(s - max(-1.0, min(1.0, f)) * 32767) <= 1


def test_speech_stream_pcm():
    server.start()
    res = post_audio("/v1/audio/speech", response_format="pcm", stream_format="audio")
    assert res.status_code == 200
    assert res.headers.get("Transfer-Encoding") == "chunked"
    assert res.content == post_audio("/v1/audio/speech", response_format="pcm").content


def test_speech_stream_wav_has_unknown_size():
    server.start()
    res = post_audio("/v1/audio/speech", stream_format="audio")
    assert_wav(res, data_size=0xFFFFFFFF)
    # the samples are the same as the non-streamed clip
    ref = post_audio("/v1/audio/speech")
    assert res.content[44:] == ref.content[44:]


def test_speech_voice():
    server.start()
    assert_wav(post_audio("/v1/audio/speech", voice="female"))
    assert_wav(post_audio("/v1/audio/speech", voice="male"))
    assert post_audio("/v1/audio/speech", voice={"id": "male"}).status_code == 200


def test_speech_multipart():
    server.start()
    fields = {k: (None, str(v)) for k, v in BODY.items()}
    res = requests.post(server.make_url("/v1/audio/speech"), files=fields, timeout=DEFAULT_REQUEST_TIMEOUT)
    assert_wav(res)
    assert res.content == post_audio("/v1/audio/speech").content


@pytest.mark.parametrize("extra", [{"speed": 1.0}, {"instructions": "Speak slowly."}])
def test_speech_accepted_params(extra: dict):
    server.start()
    assert_wav(post_audio("/v1/audio/speech", **extra))


@pytest.mark.parametrize("extra,message", [
    ({"response_format": "mp3"}, "response_format"),
    ({"response_format": "opus"}, "response_format"),
    ({"stream_format": "sse"}, "stream_format"),
    ({"speed": 1.5}, "speed"),
    ({"voice": "alloy"}, "unknown voice"),
    ({"input": ""}, "input"),
])
def test_speech_invalid_request(extra: dict, message: str):
    server.start()
    res = post_audio("/v1/audio/speech", **extra)
    assert res.status_code == 400
    assert message in res.json()["error"]["message"]


def test_speech_no_tts_model():
    global server
    server = ServerPreset.text()
    server.start()
    assert post_audio("/v1/audio/speech").status_code == 501


def test_speech_max_audio_seconds_limits_the_clip():
    server.start()
    short = requests.post(server.make_url("/tts"), json={"input": "one two three four five six seven eight nine ten", "max_audio_seconds": 0.4, "response_format": "pcm16"})
    long = requests.post(server.make_url("/tts"), json={"input": "one two three four five six seven eight nine ten", "max_audio_seconds": 10, "response_format": "pcm16"})
    assert short.status_code == 200 and long.status_code == 200
    assert len(short.content) < len(long.content)
    assert len(short.content) <= 0.4 * 24000 * 2 + 1920 * 2


def test_chat_completion_with_audio_output():
    """OpenAI audio output in chat completions: modalities + audio"""
    server.start()
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 32,
        "modalities": ["text", "audio"],
        "audio": {"voice": "female", "format": "wav"},
        "messages": [{"role": "user", "content": "echo: spoken reply"}],
    })
    assert res.status_code == 200, res.body
    msg = res.body["choices"][0]["message"]
    assert msg["content"] == "spoken reply"
    assert "audio" in msg
    wav = base64.b64decode(msg["audio"]["data"])
    assert wav[:4] == b"RIFF"
    assert msg["audio"]["transcript"] == "spoken reply"
    assert msg["audio"]["id"].startswith("audio_")
    assert res.body["timings"]["audio_frames"] > 0


def test_chat_completion_with_audio_output_stream():
    server.start()
    text = ""
    audio_bytes = 0
    for data in server.make_stream_request("POST", "/v1/chat/completions", data={
        "max_tokens": 32, "stream": True,
        "modalities": ["text", "audio"],
        "audio": {"voice": "male", "format": "pcm16"},
        "messages": [{"role": "user", "content": "echo: spoken stream"}],
    }):
        if not data["choices"]:
            continue
        delta = data["choices"][0]["delta"]
        if delta.get("content"):
            text += delta["content"]
        if delta.get("audio"):
            audio_bytes += len(base64.b64decode(delta["audio"]["data"]))
    assert text == "spoken stream"
    assert audio_bytes > 0


def test_chat_completion_audio_only_is_tts():
    server.start()
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 32, "modalities": ["audio"], "audio": {"voice": "female", "format": "pcm16"},
        "messages": [{"role": "user", "content": "Read this sentence aloud please."}],
    })
    assert res.status_code == 200
    msg = res.body["choices"][0]["message"]
    assert "audio" in msg and len(base64.b64decode(msg["audio"]["data"])) > 0


def test_chat_completion_audio_output_unsupported_model():
    global server
    server = ServerPreset.text()
    server.start()
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 8, "modalities": ["text", "audio"], "messages": [{"role": "user", "content": "hi"}],
    })
    assert res.status_code == 501
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 8, "modalities": ["smell"], "messages": [{"role": "user", "content": "hi"}],
    })
    assert res.status_code == 400
