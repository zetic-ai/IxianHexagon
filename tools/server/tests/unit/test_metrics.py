import pytest
from utils import *

server = ServerPreset.text()


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.text()
    server.server_metrics = True


def fetch_metrics(server: ServerProcess) -> str:
    res = server.make_request("GET", "/metrics")
    assert res.status_code == 200
    assert "Process-Start-Time-Unix" in res.headers
    assert isinstance(res.body, str)
    return res.body


def parse_metrics(text: str) -> dict:
    out = {}
    types = {}
    for line in text.splitlines():
        if line.startswith("# TYPE "):
            _, _, name, kind = line.split(" ", 3)
            types[name] = kind
        elif (line.startswith("llamacpp:") or line.startswith("ixian:")) and "{" not in line:
            name, value = line.split(" ", 1)
            assert name in types, f"{name} has no # TYPE line"
            out[name] = (types[name], float(value))
    return out


def test_metrics_disabled():
    global server
    server.server_metrics = False
    server.start()
    res = server.make_request("GET", "/metrics")
    assert res.status_code == 501


def test_metrics_prometheus_format():
    global server
    server.start()
    server.make_request("POST", "/completion", data={"prompt": "I believe", "n_predict": 8})
    text = fetch_metrics(server)
    metrics = parse_metrics(text)
    expected_counters = [
        "llamacpp:prompt_tokens_total", "llamacpp:prompt_tokens_cached_total", "llamacpp:prompt_seconds_total",
        "llamacpp:tokens_predicted_total", "llamacpp:tokens_predicted_seconds_total", "llamacpp:n_decode_total",
        "llamacpp:n_tokens_max", "llamacpp:spec_decode_num_draft_tokens_total",
        "llamacpp:spec_decode_num_accepted_tokens_total", "llamacpp:spec_decode_num_drafts_total",
        "ixian:requests_total",
    ]
    expected_gauges = [
        "llamacpp:prompt_tokens_seconds", "llamacpp:predicted_tokens_seconds", "llamacpp:requests_processing",
        "llamacpp:requests_deferred", "llamacpp:n_busy_slots_per_decode",
    ]
    for name in expected_counters:
        assert metrics[name][0] == "counter"
    for name in expected_gauges:
        assert metrics[name][0] == "gauge"
    for name in expected_counters + expected_gauges:
        assert f"# HELP {name} " in text
    assert metrics["llamacpp:n_decode_total"][1] > 0
    assert metrics["llamacpp:requests_processing"][1] == 0
    assert metrics["ixian:requests_total"][1] == 1


def test_metrics_prompt_processed_and_cached():
    global server
    server.start()
    prompt = "the quick brown fox jumps over the lazy dog"
    n_processed = 0
    n_cached = 0
    for _ in range(2):
        res = server.make_request("POST", "/completion", data={"prompt": prompt, "n_predict": 4})
        assert res.status_code == 200
        n_processed += res.body["timings"]["prompt_n"]
        n_cached += res.body["timings"]["cache_n"]
    assert n_cached > 0
    metrics = parse_metrics(fetch_metrics(server))
    assert metrics["llamacpp:prompt_tokens_total"][1] == n_processed
    assert metrics["llamacpp:prompt_tokens_cached_total"][1] == n_cached


def test_metrics_predicted_total_matches_requests():
    global server
    server.start()
    n_predicted = 0
    for n_predict in [1, 4, 16]:
        res = server.make_request("POST", "/completion", data={"prompt": "I believe", "n_predict": n_predict})
        assert res.status_code == 200
        n_predicted += res.body["timings"]["predicted_n"]
    metrics = parse_metrics(fetch_metrics(server))
    assert metrics["llamacpp:tokens_predicted_total"][1] == n_predicted


@pytest.mark.parametrize("n_predict", [1, 8])
def test_metrics_timings_are_finite(n_predict: int):
    global server
    server.start()
    res = server.make_request("POST", "/completion", data={"prompt": "I believe", "n_predict": n_predict})
    timings = res.body["timings"]
    for key, value in timings.items():
        assert value is not None, f"{key} is null"
        assert value >= 0, f"{key} is negative"
    assert timings["prompt_ms"] > 0
    assert timings["prompt_per_token_ms"] > 0
    assert timings["predicted_n"] == n_predict


def test_metrics_slots_idle_after_completion():
    global server
    server.server_slots = True
    server.start()
    server.make_request("POST", "/completion", data={"prompt": "I believe", "n_predict": 8})
    res = server.make_request("GET", "/slots")
    assert res.status_code == 200
    for slot in res.body:
        assert slot["is_processing"] is False
        if "next_token" in slot:
            assert slot["next_token"][0]["n_remain"] == -1
            assert slot["next_token"][0]["n_decoded"] == 0


def test_slots_busy_and_fail_on_no_slot():
    global server
    server.server_slots = True
    server.start()
    import threading, time
    done = []

    def long_request():
        done.append(server.make_request("POST", "/completion", data={"prompt": "sleep: 800 one", "n_predict": 4}))

    t = threading.Thread(target=long_request)
    t.start()
    time.sleep(0.3)
    res = server.make_request("GET", "/slots")
    assert res.body[0]["is_processing"] is True
    assert res.body[0]["id_task"] >= 0
    assert "params" in res.body[0]
    res = server.make_request("GET", "/slots?fail_on_no_slot=1")
    assert res.status_code == 503
    t.join()
    assert done[0].status_code == 200
    res = server.make_request("GET", "/slots?fail_on_no_slot=1")
    assert res.status_code == 200


def test_slot_erase():
    global server
    server.slot_save_path = "/tmp/ixian-slot-save"
    server.start()
    server.make_request("POST", "/completion", data={"prompt": "the quick brown fox", "n_predict": 4})
    res = server.make_request("POST", "/slots/0?action=erase", data={})
    assert res.status_code == 200
    assert res.body["id_slot"] == 0
    assert res.body["n_erased"] >= 0
    # the cache is empty: nothing is reused
    res = server.make_request("POST", "/completion", data={"prompt": "the quick brown fox", "n_predict": 4})
    assert res.body["timings"]["cache_n"] == 0
    res = server.make_request("POST", "/slots/0?action=save", data={"filename": "x.bin"})
    assert res.status_code == 501
    res = server.make_request("POST", "/slots/1?action=erase", data={})
    assert res.status_code == 400
    res = server.make_request("POST", "/slots/0?action=bogus", data={})
    assert res.status_code == 400


def test_slot_action_without_save_path():
    global server
    server.start()
    res = server.make_request("POST", "/slots/0?action=erase", data={})
    assert res.status_code == 501
