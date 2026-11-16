import pytest
from openai import OpenAI
from utils import *

server: ServerProcess


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.text()
    server.n_predict = 128


def test_responses_with_openai_library():
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    res = client.responses.create(
        model="gpt-4.1",
        input=[
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "echo: the best book"},
        ],
        max_output_tokens=8,
        temperature=0.8,
    )
    assert res.id.startswith("resp_")
    assert res.output[0].id is not None and res.output[0].id.startswith("msg_")
    assert res.output_text == "the best book"
    assert res.usage.input_tokens > 0 and res.usage.output_tokens > 0


def test_responses_string_input_and_instructions():
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "input": "echo: plain input", "instructions": "Be short.", "max_output_tokens": 16,
    })
    assert res.status_code == 200
    assert res.body["object"] == "response"
    assert res.body["status"] == "completed"
    assert res.body["output"][0]["type"] == "message"
    assert res.body["output"][0]["content"][0]["text"] == "plain input"


def test_responses_stream_with_openai_library():
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    stream = client.responses.create(
        model="gpt-4.1",
        input=[{"role": "user", "content": "echo: streamed response text"}],
        max_output_tokens=16,
        stream=True,
    )
    gathered_text = ""
    resp_id = ""
    msg_id = ""
    completed = False
    for r in stream:
        if r.type == "response.created":
            assert r.response.id.startswith("resp_")
            resp_id = r.response.id
        if r.type == "response.in_progress":
            assert r.response.id == resp_id
        if r.type == "response.output_item.added":
            assert r.item.id.startswith("msg_")
            msg_id = r.item.id
        if r.type in ("response.content_part.added", "response.output_text.delta", "response.output_text.done", "response.content_part.done"):
            assert r.item_id == msg_id
        if r.type == "response.output_item.done":
            assert r.item.id == msg_id
        if r.type == "response.output_text.delta":
            gathered_text += r.delta
        if r.type == "response.completed":
            completed = True
            assert r.response.id.startswith("resp_")
            assert r.response.output[0].id.startswith("msg_")
            assert gathered_text == r.response.output_text == "streamed response text"
    assert completed


def test_responses_stream_with_llama_telemetry():
    server.start()
    saw_progress = False
    saw_delta_timings = False
    completed = None
    for data in server.make_stream_request("POST", "/responses", data={
        "input": "This is a test " * 10, "max_output_tokens": 8, "stream": True,
        "timings_per_token": True, "return_progress": True,
    }):
        if "prompt_progress" in data:
            assert data["type"] == "response.in_progress"
            assert data["prompt_progress"]["total"] > 0
            saw_progress = True
        if "timings" in data:
            assert "prompt_per_second" in data["timings"]
            if data["type"] == "response.output_text.delta":
                saw_delta_timings = True
        if data["type"] == "response.completed":
            completed = data
    assert saw_progress and saw_delta_timings and completed is not None
    assert "usage" in completed["response"]
    assert "timings" in completed


def test_responses_tool_call():
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "input": 'tool: get_weather {"city": "Paris"}',
        "tools": [{"type": "function", "name": "get_weather", "parameters": {"type": "object", "properties": {"city": {"type": "string"}}}}],
        "max_output_tokens": 64,
    })
    assert res.status_code == 200, res.body
    calls = [o for o in res.body["output"] if o["type"] == "function_call"]
    assert len(calls) == 1
    assert calls[0]["name"] == "get_weather"
    assert json.loads(calls[0]["arguments"]) == {"city": "Paris"}
    assert calls[0]["call_id"].startswith("call_")


def test_responses_input_tokens():
    server.start()
    res = server.make_request("POST", "/v1/responses/input_tokens", data={"input": "How many tokens is this?"})
    assert res.status_code == 200
    assert res.body["input_tokens"] > 5
    assert res.body["object"] == "response.input_tokens"


def test_responses_invalid():
    server.start()
    assert server.make_request("POST", "/v1/responses", data={}).status_code == 400
    assert server.make_request("POST", "/v1/responses", data={"input": 42}).status_code == 400
    assert server.make_request("POST", "/v1/responses", data={"input": "x", "previous_response_id": "resp_1"}).status_code == 400
