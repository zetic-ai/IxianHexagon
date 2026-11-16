import pytest
from openai import OpenAI
from utils import *

server: ServerProcess


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.omni()


def test_chat_completion_echo():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 32,
        "messages": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "echo: Hello world from the NPU"},
        ],
    })
    assert res.status_code == 200
    assert "cmpl" in res.body["id"]
    assert res.body["object"] == "chat.completion"
    assert res.body["system_fingerprint"].startswith("b")
    assert res.body["model"] == server.model_alias
    choice = res.body["choices"][0]
    assert choice["message"]["role"] == "assistant"
    assert choice["message"]["content"] == "Hello world from the NPU"
    assert choice["finish_reason"] == "stop"
    assert res.body["usage"]["completion_tokens"] > 0
    assert res.body["usage"]["prompt_tokens"] > 5
    assert res.body["usage"]["total_tokens"] == res.body["usage"]["prompt_tokens"] + res.body["usage"]["completion_tokens"]
    assert res.body["usage"]["prompt_tokens_details"]["cached_tokens"] == 0
    assert "timings" in res.body
    assert res.body["timings"]["predicted_n"] == res.body["usage"]["completion_tokens"]


def test_chat_completion_length():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 4,
        "messages": [{"role": "user", "content": "Write a very long book."}],
    })
    assert res.status_code == 200
    assert res.body["choices"][0]["finish_reason"] == "length"
    assert res.body["usage"]["completion_tokens"] == 4


def test_chat_completion_default_n_predict_cap():
    """the server's -n caps the reply when the request asks for more"""
    global server
    server.n_predict = 5
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 100,
        "messages": [{"role": "user", "content": "Write a very long book."}],
    })
    assert res.body["usage"]["completion_tokens"] == 5
    assert res.body["choices"][0]["finish_reason"] == "length"


def test_chat_completion_cached_tokens():
    global server
    server.start()
    seq = [
        ("1 2 3 4 5 6", 0),
        ("1 2 3 4 5 6", None),  # same prompt: everything but the last rows is cached
        ("1 2 3 4 5 9", None),
        ("9 9 9 9 9 9", None),
    ]
    prev_prompt = None
    for user_prompt, n_cache in seq:
        res = server.make_request("POST", "/chat/completions", data={
            "max_tokens": 8,
            "messages": [
                {"role": "system", "content": "Test"},
                {"role": "user", "content": user_prompt},
            ],
        })
        assert res.status_code == 200
        n_prompt = res.body["usage"]["prompt_tokens"]
        cached = res.body["usage"]["prompt_tokens_details"]["cached_tokens"]
        if n_cache is not None:
            assert cached == n_cache
        else:
            assert 0 < cached <= n_prompt
        if prev_prompt is not None:
            assert n_prompt == prev_prompt
        prev_prompt = n_prompt


def test_chat_completion_no_cache_prompt():
    global server
    server.start()
    for _ in range(2):
        res = server.make_request("POST", "/chat/completions", data={
            "max_tokens": 8, "cache_prompt": False,
            "messages": [{"role": "user", "content": "same prompt twice"}],
        })
        assert res.body["usage"]["prompt_tokens_details"]["cached_tokens"] == 0


def test_chat_completion_multi_turn_reuses_reply():
    """a re-sent history with the assistant reply reuses the cache (generated-record recall)"""
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 32,
        "messages": [{"role": "user", "content": "echo: The capital of France is Paris."}],
    })
    reply = res.body["choices"][0]["message"]["content"]
    res2 = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 32,
        "messages": [
            {"role": "user", "content": "echo: The capital of France is Paris."},
            {"role": "assistant", "content": reply},
            {"role": "user", "content": "echo: And its population?"},
        ],
    })
    assert res2.status_code == 200
    usage = res2.body["usage"]
    assert usage["prompt_tokens_details"]["cached_tokens"] > res.body["usage"]["prompt_tokens"] - 2
    assert res2.body["choices"][0]["message"]["content"] == "And its population?"


def test_chat_completion_stream():
    global server
    server.model_alias = "ixian-test-model"
    server.start()
    res = server.make_stream_request("POST", "/chat/completions", data={
        "max_tokens": 32,
        "messages": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "echo: streaming works fine"},
        ],
        "stream": True,
    })
    content = ""
    last_cmpl_id = None
    got_usage = False
    for i, data in enumerate(res):
        if data["choices"]:
            choice = data["choices"][0]
            if i == 0:
                assert choice["delta"]["content"] is None
                assert choice["delta"]["role"] == "assistant"
            else:
                assert "role" not in choice["delta"]
            assert data["system_fingerprint"].startswith("b")
            assert data["model"] == "ixian-test-model"
            assert data["object"] == "chat.completion.chunk"
            if last_cmpl_id is None:
                last_cmpl_id = data["id"]
            assert last_cmpl_id == data["id"]
            if choice["finish_reason"] in ["stop", "length"]:
                assert "content" not in choice["delta"]
                assert content == "streaming works fine"
                assert choice["finish_reason"] == "stop"
            else:
                assert choice["finish_reason"] is None
                content += choice["delta"]["content"] or ""
        else:
            got_usage = True
            assert data["usage"]["prompt_tokens"] > 0
            assert data["usage"]["completion_tokens"] > 0
            assert "timings" in data
    assert got_usage


def test_chat_completion_stream_without_usage():
    global server
    server.start()
    chunks = list(server.make_stream_request("POST", "/v1/chat/completions", data={
        "max_tokens": 8, "stream": True, "stream_options": {"include_usage": False},
        "messages": [{"role": "user", "content": "echo: a b c"}],
    }))
    assert all(c["choices"] for c in chunks)
    assert chunks[-1]["choices"][0]["finish_reason"] == "stop"
    assert "timings" in chunks[-1]


def test_chat_completion_with_openai_library():
    global server
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    res = client.chat.completions.create(
        model="gpt-3.5-turbo-instruct",
        messages=[
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "echo: via the openai client"},
        ],
        max_tokens=8,
        seed=42,
        temperature=0.8,
    )
    assert res.system_fingerprint is not None and res.system_fingerprint.startswith("b")
    assert res.choices[0].finish_reason == "stop"
    assert res.choices[0].message.content == "via the openai client"


def test_chat_completion_openai_library_stream():
    global server
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    stream = client.chat.completions.create(
        model="x", messages=[{"role": "user", "content": "echo: one two three"}], max_tokens=16, stream=True,
        stream_options={"include_usage": True},
    )
    text = ""
    usage = None
    for chunk in stream:
        if chunk.choices and chunk.choices[0].delta.content:
            text += chunk.choices[0].delta.content
        if chunk.usage:
            usage = chunk.usage
    assert text == "one two three"
    assert usage is not None and usage.completion_tokens > 0


def test_chat_verbose_has_prompt():
    global server
    server.debug = True
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 8,
        "messages": [{"role": "system", "content": "Book"}, {"role": "user", "content": "echo: hi"}],
    })
    assert res.status_code == 200
    assert "__verbose" in res.body
    prompt = res.body["__verbose"]["prompt"]
    assert prompt == "<|startoftext|><|im_start|>system\nBook<|im_end|>\n<|im_start|>user\necho: hi<|im_end|>\n<|im_start|>assistant\n<think></think>"


def test_apply_chat_template():
    global server
    server.start()
    res = server.make_request("POST", "/apply-template", data={
        "messages": [
            {"role": "system", "content": "You are a test."},
            {"role": "user", "content": "Hi there"},
        ]
    })
    assert res.status_code == 200
    assert res.body["prompt"] == "<|startoftext|><|im_start|>system\nYou are a test.<|im_end|>\n<|im_start|>user\nHi there<|im_end|>\n<|im_start|>assistant\n<think></think>"
    res = server.make_request("POST", "/apply-template", data={
        "messages": [{"role": "user", "content": "Hi"}],
        "chat_template_kwargs": {"enable_thinking": True},
    })
    assert res.body["prompt"].endswith("<|im_start|>assistant\n<think>")
    res = server.make_request("POST", "/apply-template", data={
        "messages": [{"role": "user", "content": "Hi"}],
        "add_generation_prompt": False,
    })
    assert res.body["prompt"].endswith("Hi<|im_end|>\n")


@pytest.mark.parametrize("messages", [
    None,
    "string",
    [123],
    [{}],
    [{"role": 123}],
    [{"role": "system", "content": 123}],
    [{"role": "system", "content": "test"}, {}],
    [{"role": "user", "content": "test"}, {"role": "assistant", "content": "test"}, {"role": "assistant", "content": "test"}],
    [{"role": "user", "content": [{"type": "nope", "text": "x"}]}],
    [{"role": "user", "content": [{"text": "no type"}]}],
    [{"role": "assistant"}],
    [{"role": "mystery", "content": "x"}],
])
def test_invalid_chat_completion_req(messages):
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={"messages": messages})
    assert res.status_code == 400
    assert "error" in res.body
    assert res.body["error"]["type"] == "invalid_request_error"


def test_invalid_json_body():
    global server
    server.start()
    res = requests.post(server.make_url("/chat/completions"), data="{not json", headers={"Content-Type": "application/json"})
    assert res.status_code == 400
    assert res.json()["error"]["type"] == "invalid_request_error"


def test_chat_completion_with_timings_per_token():
    global server
    server.start()
    res = server.make_stream_request("POST", "/chat/completions", data={
        "max_tokens": 10,
        "messages": [{"role": "user", "content": "echo: a b c d e"}],
        "stream": True,
        "stream_options": {"include_usage": True},
        "timings_per_token": True,
    })
    stats_received = False
    for i, data in enumerate(res):
        if i == 0:
            assert data["choices"][0]["delta"]["content"] is None
            assert data["choices"][0]["delta"]["role"] == "assistant"
            assert "timings" not in data
        else:
            if data["choices"]:
                assert "role" not in data["choices"][0]["delta"]
                if data["choices"][0]["finish_reason"] is None:
                    assert "timings" in data
            else:
                assert "timings" in data
                assert "prompt_per_second" in data["timings"]
                assert "predicted_per_second" in data["timings"]
                assert "predicted_n" in data["timings"]
                assert data["timings"]["predicted_n"] <= 10
                stats_received = True
    assert stats_received


def test_logprobs():
    global server
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    res = client.chat.completions.create(
        model="x", temperature=0.0,
        messages=[{"role": "user", "content": "echo: five tokens or so"}],
        max_tokens=5, logprobs=True, top_logprobs=10,
    )
    output_text = res.choices[0].message.content
    aggregated_text = ""
    assert res.choices[0].logprobs is not None
    assert res.choices[0].logprobs.content is not None
    for token in res.choices[0].logprobs.content:
        aggregated_text += token.token
        assert token.logprob <= 0.0
        assert token.bytes is not None
        assert len(token.top_logprobs) > 0
    assert aggregated_text == output_text


def test_logprobs_stream():
    global server
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    res = client.chat.completions.create(
        model="x", temperature=0.0,
        messages=[{"role": "user", "content": "echo: five tokens or so"}],
        max_tokens=5, logprobs=True, top_logprobs=10, stream=True,
    )
    output_text = ""
    aggregated_text = ""
    for i, data in enumerate(res):
        if data.choices:
            choice = data.choices[0]
            if i == 0:
                assert choice.delta.content is None
                assert choice.delta.role == "assistant"
            else:
                assert choice.delta.role is None
                if choice.finish_reason is None:
                    if choice.delta.content:
                        output_text += choice.delta.content
                    assert choice.logprobs is not None
                    assert choice.logprobs.content is not None
                    for token in choice.logprobs.content:
                        aggregated_text += token.token
                        assert token.logprob <= 0.0
                        assert token.bytes is not None
                        assert token.top_logprobs is not None
                        assert len(token.top_logprobs) > 0
    assert aggregated_text == output_text


def test_context_size_exceeded():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "messages": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ] * 200,
    })
    assert res.status_code == 400
    assert "error" in res.body
    assert res.body["error"]["type"] == "exceed_context_size_error"
    assert res.body["error"]["n_prompt_tokens"] > 0
    assert res.body["error"]["n_ctx"] == server.n_ctx


def test_context_size_exceeded_stream():
    global server
    server.start()
    try:
        for _ in server.make_stream_request("POST", "/chat/completions", data={
            "messages": [
                {"role": "system", "content": "Book"},
                {"role": "user", "content": "What is the best book"},
            ] * 200,
            "stream": True}):
            pass
        assert False, "Should have failed"
    except ServerError as e:
        assert e.code == 400
        assert e.body["error"]["type"] == "exceed_context_size_error"
        assert e.body["error"]["n_prompt_tokens"] > 0
        assert e.body["error"]["n_ctx"] == server.n_ctx


def test_stop_sequences():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 64,
        "stop": [" STOP"],
        "messages": [{"role": "user", "content": "echo: before STOP after"}],
    })
    assert res.body["choices"][0]["message"]["content"] == "before"
    assert res.body["choices"][0]["finish_reason"] == "stop"
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 64,
        "stop": " STOP",
        "stream": False,
        "messages": [{"role": "user", "content": "echo: before STOP after"}],
    })
    assert res.body["choices"][0]["message"]["content"] == "before"


def test_stop_sequence_never_streamed():
    global server
    server.start()
    content = ""
    for data in server.make_stream_request("POST", "/chat/completions", data={
        "max_tokens": 64, "stop": ["STOP"], "stream": True,
        "messages": [{"role": "user", "content": "echo: before STOP after"}],
    }):
        if data["choices"] and data["choices"][0]["delta"].get("content"):
            content += data["choices"][0]["delta"]["content"]
    assert "STOP" not in content
    assert content.strip() == "before"


def test_reasoning_content():
    global server
    server.start()
    body = {"max_tokens": 64, "chat_template_kwargs": {"enable_thinking": True},
            "messages": [{"role": "user", "content": "think: Paris is the capital."}]}
    res = server.make_request("POST", "/chat/completions", data=body)
    msg = res.body["choices"][0]["message"]
    assert msg["reasoning_content"] == "Paris is the capital."
    assert msg["content"] == "I thought about it."
    assert "<think>" not in msg["content"]
    # off by default
    res = server.make_request("POST", "/chat/completions", data={"max_tokens": 64, "messages": body["messages"]})
    msg = res.body["choices"][0]["message"]
    assert "reasoning_content" not in msg
    assert msg["content"] == "I thought about it."
    # streamed
    body["stream"] = True
    out = server.make_any_request("POST", "/chat/completions", data=body)
    assert out["choices"][0]["message"]["reasoning_content"] == "Paris is the capital."
    assert out["choices"][0]["message"]["content"] == "I thought about it."


def test_reasoning_server_default_on_and_effort_none():
    global server
    server.reasoning = "on"
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 64, "messages": [{"role": "user", "content": "think: deep"}]})
    assert res.body["choices"][0]["message"]["reasoning_content"] == "deep"
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 64, "reasoning_effort": "none", "messages": [{"role": "user", "content": "think: deep"}]})
    assert "reasoning_content" not in res.body["choices"][0]["message"]


def test_reasoning_format_none_keeps_think_tags():
    global server
    server.reasoning = "on"
    server.reasoning_format = "none"
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 64, "messages": [{"role": "user", "content": "think: deep"}]})
    msg = res.body["choices"][0]["message"]
    assert "reasoning_content" not in msg
    assert msg["content"] == "<think>deep</think>I thought about it."
    content = ""
    for data in server.make_stream_request("POST", "/chat/completions", data={
        "max_tokens": 64, "stream": True, "messages": [{"role": "user", "content": "think: deep"}]}):
        if data["choices"] and data["choices"][0]["delta"].get("content"):
            content += data["choices"][0]["delta"]["content"]
    assert content == "<think>deep</think>I thought about it."


def test_n_completions():
    global server
    server.start()
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 8, "n": 3, "seed": 7,
        "messages": [{"role": "user", "content": "Write a story"}],
    })
    assert res.status_code == 200
    assert len(res.body["choices"]) == 3
    assert [c["index"] for c in res.body["choices"]] == [0, 1, 2]
    total = sum(1 for _ in res.body["choices"])
    assert res.body["usage"]["completion_tokens"] >= total


def test_seed_determinism():
    global server
    server.start()
    body = {"max_tokens": 16, "seed": 123, "temperature": 1.0, "cache_prompt": False,
            "messages": [{"role": "user", "content": "Tell me something"}]}
    a = server.make_request("POST", "/chat/completions", data=body).body["choices"][0]["message"]["content"]
    b = server.make_request("POST", "/chat/completions", data=body).body["choices"][0]["message"]["content"]
    assert a == b
    body["seed"] = 124
    c = server.make_request("POST", "/chat/completions", data=body).body["choices"][0]["message"]["content"]
    assert c != a


def test_response_format_soft_json():
    """json schema requests are rendered into the prompt (the NPU runtime has no grammar sampler)"""
    global server
    server.debug = True
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 32,
        "response_format": {"type": "json_schema", "json_schema": {"schema": {"type": "object", "properties": {"answer": {"type": "string"}}}}},
        "messages": [{"role": "user", "content": "json: forty-two"}],
    })
    assert res.status_code == 200
    assert res.body["choices"][0]["message"]["content"] == '{"answer": "forty-two"}'
    assert "JSON schema" in res.body["__verbose"]["prompt"]
    for bad in [{"type": "sound"}, {"type": "json_object", "schema": 123}, {"type": "json_object", "schema": {"type": 123}}, {"type": "json_object", "schema": {"type": "hiccup"}}]:
        res = server.make_request("POST", "/chat/completions", data={"max_tokens": 4, "response_format": bad, "messages": [{"role": "user", "content": "x"}]})
        assert res.status_code == 400, bad


def test_grammar_is_not_supported():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 8, "grammar": 'root ::= "a"{5,5}',
        "messages": [{"role": "user", "content": "x"}],
    })
    assert res.status_code == 501
    assert res.body["error"]["type"] == "not_supported_error"


def test_continue_final_message_not_supported():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 8, "add_generation_prompt": False, "continue_final_message": True,
        "messages": [{"role": "user", "content": "Hi"}, {"role": "assistant", "content": "Hello"}],
    })
    assert res.status_code == 501
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 8, "add_generation_prompt": True, "continue_final_message": True,
        "messages": [{"role": "user", "content": "Hi"}, {"role": "assistant", "content": "Hello"}],
    })
    assert res.status_code == 400


def test_trailing_assistant_message_is_a_full_turn():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 8,
        "messages": [{"role": "user", "content": "echo: hi"}, {"role": "assistant", "content": "Hello"}],
    })
    assert res.status_code == 200


def test_chat_completion_input_tokens():
    global server
    server.start()
    res = server.make_request("POST", "/v1/chat/completions/input_tokens", data={
        "messages": [{"role": "user", "content": "How many tokens is this?"}],
    })
    assert res.status_code == 200
    assert res.body["input_tokens"] > 5
    assert res.body["object"] == "response.input_tokens"
    res2 = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 1, "messages": [{"role": "user", "content": "How many tokens is this?"}]})
    assert res2.body["usage"]["prompt_tokens"] == res.body["input_tokens"]


def test_model_field_not_reflected():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "model": "gpt-4o", "max_tokens": 4, "messages": [{"role": "user", "content": "echo: hi"}]})
    assert res.body["model"] == server.model_alias


def test_ignored_params_do_not_fail():
    global server
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 4, "presence_penalty": 0.5, "frequency_penalty": 0.5, "logit_bias": {"1": -100}, "mirostat": 2,
        "user": "someone", "messages": [{"role": "user", "content": "echo: hi"}]})
    assert res.status_code == 200
