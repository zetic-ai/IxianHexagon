import base64
import json

import pytest
import requests
from utils import *

server: ServerProcess


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.omni()
    server.model_alias = "lfm3-anthropic"
    server.n_predict = 256


def test_anthropic_messages_basic():
    server.start()
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 50,
        "messages": [{"role": "user", "content": "echo: Say hello"}],
    })
    assert res.status_code == 200, res.body
    assert res.body["type"] == "message"
    assert res.body["role"] == "assistant"
    assert isinstance(res.body["content"], list) and len(res.body["content"]) > 0
    assert res.body["content"][0]["type"] == "text"
    assert res.body["content"][0]["text"] == "Say hello"
    assert res.body["stop_reason"] in ["end_turn", "max_tokens"]
    usage = res.body["usage"]
    for k in ["cache_read_input_tokens", "input_tokens", "output_tokens"]:
        assert isinstance(usage[k], int)
    assert usage["output_tokens"] > 0
    assert "timings" not in res.body
    assert res.body["model"] == "lfm3-anthropic"


def test_anthropic_messages_with_system_and_parts():
    server.start()
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 50,
        "system": [{"type": "text", "text": "You are a helpful assistant."}],
        "messages": [
            {"role": "user", "content": [{"type": "text", "text": "echo: What is"}, {"type": "text", "text": " the answer?"}]},
        ],
    })
    assert res.status_code == 200
    assert res.body["content"][0]["text"] == "What is the answer?"


def test_anthropic_messages_conversation():
    server.start()
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 50,
        "messages": [
            {"role": "user", "content": "Hello"},
            {"role": "assistant", "content": "Hi there!"},
            {"role": "user", "content": "echo: How are you?"},
        ],
    })
    assert res.status_code == 200
    assert res.body["content"][0]["text"] == "How are you?"


def test_anthropic_messages_streaming():
    server.start()
    res = requests.post(server.make_url("/v1/messages"), json={
        "model": "test", "max_tokens": 30, "stream": True,
        "messages": [{"role": "user", "content": "echo: Say hello to everyone"}],
    }, stream=True)
    assert res.status_code == 200
    events = []
    names = []
    for line in res.iter_lines():
        line = line.decode()
        if line.startswith("event: "):
            names.append(line[7:])
        elif line.startswith("data: "):
            events.append(json.loads(line[6:]))
    types = [e["type"] for e in events]
    for t in ["message_start", "content_block_start", "content_block_delta", "content_block_stop", "message_delta", "message_stop"]:
        assert t in types, t
    assert names == types
    message_start = next(e for e in events if e["type"] == "message_start")
    assert message_start["message"]["role"] == "assistant"
    assert message_start["message"]["content"] == []
    assert message_start["message"]["usage"]["input_tokens"] > 0
    block_start = next(e for e in events if e["type"] == "content_block_start")
    assert block_start["index"] == 0 and block_start["content_block"]["type"] == "text"
    text = "".join(e["delta"]["text"] for e in events if e["type"] == "content_block_delta")
    assert text == "Say hello to everyone"
    message_delta = next(e for e in events if e["type"] == "message_delta")
    assert message_delta["delta"]["stop_reason"] == "end_turn"
    assert message_delta["usage"]["output_tokens"] > 0
    assert "timings" not in next(e for e in events if e["type"] == "message_stop")


def test_anthropic_count_tokens():
    server.start()
    res = server.make_request("POST", "/v1/messages/count_tokens", data={
        "model": "test", "messages": [{"role": "user", "content": "Hello world"}],
    })
    assert res.status_code == 200
    assert isinstance(res.body["input_tokens"], int) and res.body["input_tokens"] > 0
    assert "output_tokens" not in res.body
    res2 = server.make_request("POST", "/v1/messages/count_tokens", data={
        "model": "test", "system": "You are a helpful assistant.", "messages": [{"role": "user", "content": "Hello"}],
    })
    assert res2.body["input_tokens"] > 0


def test_anthropic_tool_use():
    server.start()
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 100,
        "tools": [{"name": "get_weather", "description": "Get the weather", "input_schema": {"type": "object", "properties": {"location": {"type": "string"}}, "required": ["location"]}}],
        "messages": [{"role": "user", "content": 'tool: get_weather {"location": "Paris"}'}],
    })
    assert res.status_code == 200, res.body
    assert res.body["stop_reason"] == "tool_use"
    blocks = [b for b in res.body["content"] if b["type"] == "tool_use"]
    assert len(blocks) == 1
    assert blocks[0]["name"] == "get_weather"
    assert blocks[0]["input"] == {"location": "Paris"}
    assert blocks[0]["id"]

    # the round trip with a tool_result
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 100,
        "tools": [{"name": "get_weather", "description": "Get the weather", "input_schema": {"type": "object", "properties": {"location": {"type": "string"}}}}],
        "messages": [
            {"role": "user", "content": "Weather in Paris?"},
            {"role": "assistant", "content": [{"type": "tool_use", "id": blocks[0]["id"], "name": "get_weather", "input": {"location": "Paris"}}]},
            {"role": "user", "content": [{"type": "tool_result", "tool_use_id": blocks[0]["id"], "content": "21 C"}]},
            {"role": "user", "content": "echo: It is 21 C."},
        ],
    })
    assert res.status_code == 200
    assert res.body["content"][0]["text"] == "It is 21 C."


def test_anthropic_tool_use_streaming():
    server.start()
    res = requests.post(server.make_url("/v1/messages"), json={
        "model": "test", "max_tokens": 100, "stream": True,
        "tools": [{"name": "get_weather", "input_schema": {"type": "object", "properties": {"location": {"type": "string"}}}}],
        "messages": [{"role": "user", "content": 'tool: get_weather {"location": "Paris"}'}],
    }, stream=True)
    events = [json.loads(l.decode()[6:]) for l in res.iter_lines() if l.decode().startswith("data: ")]
    starts = [e for e in events if e["type"] == "content_block_start" and e["content_block"]["type"] == "tool_use"]
    assert len(starts) == 1 and starts[0]["content_block"]["name"] == "get_weather"
    deltas = [e for e in events if e["type"] == "content_block_delta" and e["delta"]["type"] == "input_json_delta"]
    assert json.loads("".join(d["delta"]["partial_json"] for d in deltas)) == {"location": "Paris"}
    assert next(e for e in events if e["type"] == "message_delta")["delta"]["stop_reason"] == "tool_use"


def test_anthropic_thinking():
    server.start()
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 100,
        "thinking": {"type": "enabled", "budget_tokens": 1024},
        "messages": [{"role": "user", "content": "think: Paris is in France."}],
    })
    assert res.status_code == 200, res.body
    assert res.body["content"][0]["type"] == "thinking"
    assert res.body["content"][0]["thinking"] == "Paris is in France."
    assert res.body["content"][1]["type"] == "text"
    assert res.body["content"][1]["text"] == "I thought about it."


def test_anthropic_stop_sequences_and_max_tokens():
    server.start()
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 100, "stop_sequences": [" STOP"],
        "messages": [{"role": "user", "content": "echo: before STOP after"}],
    })
    assert res.body["content"][0]["text"] == "before"
    assert res.body["stop_reason"] == "end_turn"
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 3,
        "messages": [{"role": "user", "content": "Write a long story."}],
    })
    assert res.body["stop_reason"] == "max_tokens"
    assert res.body["usage"]["output_tokens"] == 3


def test_anthropic_image():
    server.start()
    with open(sample_path("cats.png"), "rb") as f:
        b64 = base64.b64encode(f.read()).decode()
    res = server.make_request("POST", "/v1/messages", data={
        "model": "test", "max_tokens": 20,
        "messages": [{"role": "user", "content": [
            {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": b64}},
            {"type": "text", "text": "What is in the picture?"},
        ]}],
    })
    assert res.status_code == 200, res.body
    assert res.body["usage"]["input_tokens"] > 256


def test_anthropic_with_sdk():
    try:
        import anthropic
    except ImportError:
        pytest.skip("anthropic package not installed")
    server.start()
    client = anthropic.Anthropic(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}")
    msg = client.messages.create(model="x", max_tokens=32, messages=[{"role": "user", "content": "echo: via the anthropic sdk"}])
    assert msg.content[0].text == "via the anthropic sdk"
    assert msg.stop_reason == "end_turn"


def test_anthropic_invalid():
    server.start()
    res = server.make_request("POST", "/v1/messages", data={"model": "test", "max_tokens": 10})
    assert res.status_code == 400
