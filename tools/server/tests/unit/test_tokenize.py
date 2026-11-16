import pytest
from utils import *

server = ServerPreset.text()


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.text()


def test_tokenize_detokenize():
    global server
    server.start()
    content = "What is the capital of France ?"
    res_tok = server.make_request("POST", "/tokenize", data={"content": content})
    assert res_tok.status_code == 200
    assert len(res_tok.body["tokens"]) > 5
    res_detok = server.make_request("POST", "/detokenize", data={"tokens": res_tok.body["tokens"]})
    assert res_detok.status_code == 200
    assert res_detok.body["content"].strip() == content


def test_tokenize_with_bos():
    global server
    server.start()
    res_tok = server.make_request("POST", "/tokenize", data={"content": "What is the capital of France ?", "add_special": True})
    assert res_tok.status_code == 200
    assert res_tok.body["tokens"][0] == 1  # <|startoftext|>
    # not added twice
    res_tok2 = server.make_request("POST", "/tokenize", data={"content": "<|startoftext|>Hello", "add_special": True})
    assert res_tok2.body["tokens"][0] == 1 and res_tok2.body["tokens"][1] != 1


def test_tokenize_with_pieces():
    global server
    server.start()
    content = "This is a test string with unicode 媽 and emoji \U0001f917"
    res_tok = server.make_request("POST", "/tokenize", data={"content": content, "with_pieces": True})
    assert res_tok.status_code == 200
    for token in res_tok.body["tokens"]:
        assert "id" in token and token["id"] > 0
        assert "piece" in token and len(token["piece"]) > 0
    assert "".join(t["piece"] for t in res_tok.body["tokens"]) == content


def test_tokenize_special_tokens():
    global server
    server.start()
    res = server.make_request("POST", "/tokenize", data={"content": "<|im_start|>user\nhi<|im_end|>", "parse_special": True})
    assert res.body["tokens"][0] == 2 and res.body["tokens"][-1] == 3
    res = server.make_request("POST", "/tokenize", data={"content": "<|im_start|>", "parse_special": False})
    assert len(res.body["tokens"]) > 1


def test_tokenize_mixed_content():
    global server
    server.start()
    res = server.make_request("POST", "/tokenize", data={"content": ["Hello", 1, 2, " world"]})
    assert res.status_code == 200
    toks = res.body["tokens"]
    assert 1 in toks and 2 in toks and len(toks) >= 4


def test_tokenize_invalid():
    global server
    server.start()
    res = server.make_request("POST", "/tokenize", data={"content": {"nope": 1}})
    assert res.status_code == 400
    res = server.make_request("POST", "/detokenize", data={"tokens": "nope"})
    assert res.status_code == 400
    res = server.make_request("POST", "/tokenize", data={})
    assert res.status_code == 200 and res.body["tokens"] == []
