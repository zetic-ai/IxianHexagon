import json

import pytest
from openai import OpenAI
from utils import *

server: ServerProcess

WEATHER_TOOL = {
    "type": "function",
    "function": {
        "name": "get_current_weather",
        "description": "Get the current weather in a given location",
        "parameters": {
            "type": "object",
            "properties": {
                "location": {"type": "string", "description": "The city and country/state, e.g. 'San Francisco, CA'"},
                "unit": {"type": "string", "enum": ["celsius", "fahrenheit"]},
            },
            "required": ["location"],
        },
    },
}

CALC_TOOL = {
    "type": "function",
    "function": {
        "name": "calc",
        "description": "Adds numbers",
        "parameters": {
            "type": "object",
            "properties": {"a": {"type": "number"}, "b": {"type": "integer"}, "verbose": {"type": "boolean"}, "tags": {"type": "array"}},
            "required": ["a", "b"],
        },
    },
}


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.text()
    server.n_predict = 256


@pytest.mark.parametrize("stream", [False, True])
def test_tool_call(stream: bool):
    server.start()
    body = server.make_any_request("POST", "/v1/chat/completions", data={
        "max_tokens": 128,
        "messages": [
            {"role": "system", "content": "You are a weather assistant."},
            {"role": "user", "content": 'tool: get_current_weather {"location": "Paris, France", "unit": "celsius"}'},
        ],
        "tools": [WEATHER_TOOL],
        "stream": stream,
    })
    choice = body["choices"][0]
    assert choice["finish_reason"] == "tool_calls"
    tool_calls = choice["message"]["tool_calls"]
    assert tool_calls and len(tool_calls) == 1
    tc = tool_calls[0]
    assert tc["type"] == "function"
    assert len(tc["id"]) > 0
    assert tc["function"]["name"] == "get_current_weather"
    assert json.loads(tc["function"]["arguments"]) == {"location": "Paris, France", "unit": "celsius"}
    assert choice["message"].get("content") in (None, "")


def test_tool_call_argument_types_follow_the_schema():
    server.start()
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 128,
        "messages": [{"role": "user", "content": 'tool: calc {"a": 1.5, "b": 2, "verbose": true, "tags": ["x", "y"]}'}],
        "tools": [CALC_TOOL],
    })
    tc = res.body["choices"][0]["message"]["tool_calls"][0]
    assert json.loads(tc["function"]["arguments"]) == {"a": 1.5, "b": 2, "verbose": True, "tags": ["x", "y"]}


def test_tool_call_with_openai_client():
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    res = client.chat.completions.create(
        model="x",
        messages=[{"role": "user", "content": 'tool: get_current_weather {"location": "Seoul"}'}],
        tools=[WEATHER_TOOL],
        max_tokens=128,
    )
    tc = res.choices[0].message.tool_calls[0]
    assert tc.function.name == "get_current_weather"
    assert json.loads(tc.function.arguments) == {"location": "Seoul"}
    assert res.choices[0].finish_reason == "tool_calls"


def test_tool_choice_none_hides_the_tools():
    server.start()
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 32,
        "messages": [{"role": "user", "content": 'tool: get_current_weather {"location": "Paris"}'}],
        "tools": [WEATHER_TOOL],
        "tool_choice": "none",
    })
    msg = res.body["choices"][0]["message"]
    assert "tool_calls" not in msg
    assert msg["content"] == "no tools"
    assert res.body["choices"][0]["finish_reason"] == "stop"


def test_tools_are_rendered_into_the_system_prompt():
    server.start()
    res = server.make_request("POST", "/apply-template", data={
        "messages": [{"role": "system", "content": "Be brief."}, {"role": "user", "content": "Weather in Paris?"}],
        "tools": [WEATHER_TOOL],
    })
    prompt = res.body["prompt"]
    assert prompt.startswith("<|startoftext|><|im_start|>system\nBe brief.\n\n<tools>\n")
    assert '"name": "get_current_weather"' in prompt
    assert "\n</tools><|im_end|>\n<|im_start|>user\nWeather in Paris?<|im_end|>\n<|im_start|>assistant\n<think></think>" in prompt
    # tool_choice required adds an instruction
    res = server.make_request("POST", "/apply-template", data={
        "messages": [{"role": "user", "content": "x"}], "tools": [WEATHER_TOOL], "tool_choice": "required",
    })
    assert "tool call" in res.body["prompt"]
    res = server.make_request("POST", "/apply-template", data={
        "messages": [{"role": "user", "content": "x"}], "tools": [WEATHER_TOOL],
        "tool_choice": {"type": "function", "function": {"name": "get_current_weather"}},
    })
    assert '"get_current_weather"' in res.body["prompt"]


def test_tool_history_round_trip():
    """assistant tool_calls and tool results in the history render as the LFM3 template would"""
    server.start()
    messages = [
        {"role": "user", "content": "Weather in Paris?"},
        {"role": "assistant", "content": None, "tool_calls": [
            {"id": "call_1", "type": "function", "function": {"name": "get_current_weather", "arguments": '{"location": "Paris"}'}},
        ]},
        {"role": "tool", "tool_call_id": "call_1", "content": '{"temperature": 21, "unit": "celsius"}'},
        {"role": "user", "content": "echo: It is 21 degrees in Paris."},
    ]
    res = server.make_request("POST", "/apply-template", data={"messages": messages, "tools": [WEATHER_TOOL]})
    prompt = res.body["prompt"]
    assert "<|im_start|>assistant\n<think></think><tool_call>\n<function=get_current_weather>\n<parameter=location>\nParis\n</parameter>\n</function>\n</tool_call><|im_end|>\n" in prompt
    assert '<tool_response>\n{"temperature": 21, "unit": "celsius"}\n</tool_response>' in prompt
    res = server.make_request("POST", "/v1/chat/completions", data={"max_tokens": 32, "messages": messages, "tools": [WEATHER_TOOL]})
    assert res.status_code == 200
    assert res.body["choices"][0]["message"]["content"] == "It is 21 degrees in Paris."


def test_invalid_tools():
    server.start()
    for tools in ["nope", [{"type": "retrieval"}], [{"type": "function"}], [{"type": "function", "function": {}}]]:
        res = server.make_request("POST", "/v1/chat/completions", data={
            "max_tokens": 8, "messages": [{"role": "user", "content": "x"}], "tools": tools})
        assert res.status_code == 400, tools
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 8, "messages": [{"role": "user", "content": "x"}], "tools": [WEATHER_TOOL], "tool_choice": "maybe"})
    assert res.status_code == 400
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 8, "messages": [{"role": "user", "content": "x"}, {"role": "assistant", "tool_calls": [{"type": "function"}]}, {"role": "user", "content": "y"}],
        "tools": [WEATHER_TOOL]})
    assert res.status_code == 400


def test_tool_call_streamed_deltas_shape():
    server.start()
    chunks = list(server.make_stream_request("POST", "/v1/chat/completions", data={
        "max_tokens": 128, "stream": True,
        "messages": [{"role": "user", "content": 'tool: get_current_weather {"location": "Paris"}'}],
        "tools": [WEATHER_TOOL],
    }))
    tool_chunks = [c for c in chunks if c["choices"] and c["choices"][0]["delta"].get("tool_calls")]
    assert len(tool_chunks) == 1
    tc = tool_chunks[0]["choices"][0]["delta"]["tool_calls"][0]
    assert tc["index"] == 0 and tc["type"] == "function" and tc["id"]
    assert tc["function"]["name"] == "get_current_weather"
    assert json.loads(tc["function"]["arguments"]) == {"location": "Paris"}
    finish = [c for c in chunks if c["choices"] and c["choices"][0]["finish_reason"]]
    assert finish[-1]["choices"][0]["finish_reason"] == "tool_calls"
    # no content delta ever contained the tool call block
    for c in chunks:
        if c["choices"] and c["choices"][0]["delta"].get("content"):
            assert "<tool_call>" not in c["choices"][0]["delta"]["content"]
