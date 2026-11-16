import socket
import threading
import time

import pytest
import requests
from utils import *

server: ServerProcess


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.text()
    server.n_predict = 4096


def test_client_disconnect_cancels_generation():
    """a stream the client drops frees the NPU for the next request"""
    server.start()
    res = requests.post(server.make_url("/v1/chat/completions"), json={
        "stream": True, "max_tokens": 4000,
        "messages": [{"role": "user", "content": "stall"}],
    }, stream=True)
    assert res.status_code == 200
    it = res.iter_lines()
    for _ in range(3):
        next(it)
    res.close()  # drop the connection mid-stream

    deadline = time.time() + 5
    while time.time() < deadline:
        slots = server.make_request("GET", "/slots", quiet=True).body
        if slots[0]["is_processing"] is False:
            break
        time.sleep(0.05)
    assert slots[0]["is_processing"] is False, "the cancelled task is still running"

    # the engine answers the next request promptly
    t0 = time.time()
    res = server.make_request("POST", "/completion", data={"prompt": "echo: after cancel", "n_predict": 8})
    assert res.status_code == 200 and res.body["content"] == "after cancel"
    assert time.time() - t0 < 5


def test_queued_request_cancelled_before_start_is_skipped():
    server.start()

    def long_request():
        return server.make_request("POST", "/completion", data={"prompt": "sleep: 1500 slow", "n_predict": 4})

    t = threading.Thread(target=long_request)
    t.start()
    time.sleep(0.2)
    # queue a second request and drop it right away
    s = socket.create_connection((server.server_host, server.server_port))
    body = '{"prompt": "stall", "n_predict": 1000, "stream": true}'
    s.sendall(f"POST /completion HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n{body}".encode())
    time.sleep(0.2)
    s.close()
    t.join()
    # the third request does not wait behind the dropped one
    t0 = time.time()
    res = server.make_request("POST", "/completion", data={"prompt": "echo: quick", "n_predict": 4})
    assert res.body["content"] == "quick"
    assert time.time() - t0 < 5


def test_sse_ping_during_silence():
    global server
    server.sse_ping_interval = 1
    server.start()
    res = requests.post(server.make_url("/completion"), json={
        "stream": True, "n_predict": 4, "prompt": "sleep: 2500 after a pause",
    }, stream=True)
    saw_ping = False
    for line in res.iter_lines(decode_unicode=True):
        if line == ":":
            saw_ping = True
        if line.startswith("data: ") and '"stop":true' in line.replace(" ", ""):
            break
    assert saw_ping
