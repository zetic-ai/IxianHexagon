import pytest
from openai import OpenAI
from utils import *

server = ServerPreset.text()

JSON_MULTIMODAL_KEY = "multimodal_data"
JSON_PROMPT_STRING_KEY = "prompt_string"


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.text()


def test_completion():
    global server
    server.start()
    res = server.make_request("POST", "/completion", data={
        "n_predict": 8,
        "prompt": "echo: I believe the meaning of life is",
        "return_tokens": True,
    })
    assert res.status_code == 200
    assert res.body["content"] == "I believe the meaning of life is"
    assert res.body["timings"]["prompt_n"] > 0
    assert res.body["timings"]["predicted_n"] == res.body["tokens_predicted"]
    assert res.body["truncated"] is False
    assert type(res.body["has_new_line"]) == bool
    assert res.body["stop_type"] == "eos"
    assert res.body["stop"] is True
    assert res.body["id_slot"] == 0
    assert len(res.body["tokens"]) > 0 and all(type(t) == int for t in res.body["tokens"])
    assert "generation_settings" in res.body
    assert res.body["generation_settings"]["n_predict"] == 8
    assert "<|im_start|>user\necho: I believe" in res.body["prompt"]
    res = server.make_request("POST", "/completion", data={"n_predict": 8, "prompt": "echo: x"})
    assert res.body["tokens"] == []


def test_completion_stream():
    global server
    server.start()
    res = server.make_stream_request("POST", "/completion", data={
        "n_predict": 64,
        "prompt": "echo: streamed completion text",
        "stream": True,
    })
    content = ""
    for data in res:
        assert "stop" in data and type(data["stop"]) == bool
        if data["stop"]:
            assert data["timings"]["prompt_n"] > 0
            assert data["timings"]["predicted_n"] > 0
            assert data["truncated"] is False
            assert data["stop_type"] == "eos"
            assert "generation_settings" in data
            assert data["generation_settings"]["seed"] == server.seed
            assert "adaptive_target" in data["generation_settings"]
            assert content == "streamed completion text"
        else:
            assert data["tokens_predicted"] > 0
            content += data["content"]


def test_completion_stream_vs_non_stream():
    global server
    server.start()
    res_stream = server.make_stream_request("POST", "/completion", data={
        "n_predict": 8, "prompt": "I believe the meaning of life is", "stream": True, "seed": 1,
    })
    res_non_stream = server.make_request("POST", "/completion", data={
        "n_predict": 8, "prompt": "I believe the meaning of life is", "seed": 1,
    })
    content_stream = "".join(data["content"] for data in res_stream)
    assert content_stream == res_non_stream.body["content"]


def test_completion_with_openai_library():
    global server
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    res = client.completions.create(model="davinci-002", prompt="echo: going to bed", max_tokens=8)
    assert res.system_fingerprint is not None and res.system_fingerprint.startswith("b")
    assert res.choices[0].finish_reason == "stop"
    assert res.choices[0].text == "going to bed"
    assert res.object == "text_completion"


def test_completion_stream_with_openai_library():
    global server
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    res = client.completions.create(model="davinci-002", prompt="echo: going to bed", max_tokens=8, stream=True)
    output_text = ""
    for data in res:
        choice = data.choices[0]
        if choice.finish_reason is None:
            assert choice.text is not None
            output_text += choice.text
    assert output_text == "going to bed"


def test_consistent_result_same_seed():
    global server
    server.start()
    last_res = None
    for _ in range(3):
        res = server.make_request("POST", "/completion", data={
            "prompt": "I believe the meaning of life is", "seed": 42, "temperature": 0.0, "cache_prompt": False,
        })
        if last_res is not None:
            assert res.body["content"] == last_res.body["content"]
        last_res = res


def test_different_result_different_seed():
    global server
    server.start()
    last_res = None
    for seed in range(3):
        res = server.make_request("POST", "/completion", data={
            "prompt": "I believe the meaning of life is", "seed": seed, "temperature": 1.0, "cache_prompt": False,
        })
        if last_res is not None:
            assert res.body["content"] != last_res.body["content"]
        last_res = res


def test_nocache_long_input_prompt():
    global server
    server.start()
    res = server.make_request("POST", "/completion", data={
        "prompt": "I believe the meaning of life is " * 400, "seed": 42, "temperature": 1.0, "cache_prompt": False,
    })
    assert res.status_code == 400
    assert res.body["error"]["type"] == "exceed_context_size_error"


def test_json_prompt_no_mtmd():
    global server
    server.start()
    res = server.make_request("POST", "/completion", data={
        "prompt": {JSON_PROMPT_STRING_KEY: "echo: object prompt"}, "seed": 42,
    })
    assert res.status_code == 200
    assert res.body["content"] == "object prompt"


def test_json_prompt_mtmd_error_when_not_supported():
    global server
    server.start()
    res = server.make_request("POST", "/completion", data={
        "prompt": {JSON_PROMPT_STRING_KEY: "I believe the meaning of life is <__media__>",
                   JSON_MULTIMODAL_KEY: ["iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="]},
    })
    assert res.status_code != 200


def test_completion_with_tokens_input():
    global server
    server.start()
    prompt_str = "echo: tokens in"
    res = server.make_request("POST", "/tokenize", data={"content": prompt_str, "add_special": False})
    tokens = res.body["tokens"]

    res = server.make_request("POST", "/completion", data={"prompt": tokens})
    assert res.status_code == 200
    assert res.body["content"] == "tokens in"

    res = server.make_request("POST", "/completion", data={"prompt": [tokens, tokens]})
    assert res.status_code == 200
    assert type(res.body) == list and len(res.body) == 2
    assert res.body[0]["content"] == res.body[1]["content"] == "tokens in"
    assert [r["index"] for r in res.body] == [0, 1]

    res = server.make_request("POST", "/completion", data={"prompt": [tokens, prompt_str]})
    assert type(res.body) == list and len(res.body) == 2
    assert res.body[0]["content"] == res.body[1]["content"]

    res = server.make_request("POST", "/completion", data={"prompt": [tokens, {JSON_PROMPT_STRING_KEY: prompt_str}]})
    assert type(res.body) == list and len(res.body) == 2
    assert res.body[0]["content"] == res.body[1]["content"]

    res = server.make_request("POST", "/completion", data={"prompt": [*tokens[:2], " in", *tokens[2:]]})
    assert res.status_code == 200
    assert type(res.body["content"]) == str


def test_completion_response_fields():
    global server
    server.start()
    res = server.make_request("POST", "/completion", data={
        "n_predict": 8, "prompt": "echo: fields", "response_fields": ["content", "generation_settings/n_predict", "prompt"],
    })
    assert res.status_code == 200
    assert res.body["content"] == "fields"
    assert res.body["generation_settings/n_predict"] == 8
    assert "echo: fields" in res.body["prompt"]
    assert len(res.body) == 3


def test_n_probs():
    global server
    server.start()
    res = server.make_request("POST", "/completion", data={
        "prompt": "echo: a b c d e f", "n_probs": 10, "temperature": 0.0, "n_predict": 5,
    })
    assert res.status_code == 200
    assert "completion_probabilities" in res.body
    assert len(res.body["completion_probabilities"]) == 5
    for tok in res.body["completion_probabilities"]:
        assert "id" in tok and tok["id"] > 0
        assert "token" in tok and type(tok["token"]) == str
        assert "logprob" in tok and tok["logprob"] <= 0.0
        assert "bytes" in tok and type(tok["bytes"]) == list
        assert len(tok["top_logprobs"]) == 10
        for prob in tok["top_logprobs"]:
            assert "id" in prob and prob["id"] > 0
            assert "token" in prob and type(prob["token"]) == str
            assert "logprob" in prob and prob["logprob"] <= 0.0
            assert "bytes" in prob and type(prob["bytes"]) == list


def test_n_probs_stream():
    global server
    server.start()
    res = server.make_stream_request("POST", "/completion", data={
        "prompt": "echo: a b c d e f", "n_probs": 10, "temperature": 0.0, "n_predict": 5, "stream": True,
    })
    for data in res:
        if data["stop"] is False:
            assert "completion_probabilities" in data
            assert len(data["completion_probabilities"]) == 1
            for tok in data["completion_probabilities"]:
                assert tok["id"] > 0
                assert tok["logprob"] <= 0.0
                assert len(tok["top_logprobs"]) == 10


def test_n_probs_post_sampling():
    global server
    server.start()
    res = server.make_request("POST", "/completion", data={
        "prompt": "echo: a b c d e f", "n_probs": 10, "temperature": 0.0, "n_predict": 5, "post_sampling_probs": True,
    })
    assert res.status_code == 200
    for tok in res.body["completion_probabilities"]:
        assert 0.0 < tok["prob"] <= 1.0
        assert len(tok["top_probs"]) == 10
        for prob in tok["top_probs"]:
            assert 0.0 < prob["prob"] <= 1.0


def test_completion_queue_two_requests_in_order():
    """two requests at once: one runs, the other waits in the queue; both succeed"""
    global server
    server.start()
    tasks = [
        (server.make_request, ("POST", "/completion", {"prompt": "sleep: 300 one", "n_predict": 8})),
        (server.make_request, ("POST", "/completion", {"prompt": "sleep: 10 two", "n_predict": 8})),
    ]
    results = parallel_function_calls(tasks)
    assert results[0].status_code == 200 and results[0].body["content"] == "one"
    assert results[1].status_code == 200 and results[1].body["content"] == "two"


def test_completion_oai_unsupported_params():
    global server
    server.start()
    res = server.make_request("POST", "/v1/completions", data={"prompt": "x", "echo": True})
    assert res.status_code == 400
    res = server.make_request("POST", "/v1/completions", data={"prompt": "x", "suffix": "y"})
    assert res.status_code == 400
    res = server.make_request("POST", "/v1/completions", data={})
    assert res.status_code == 400
