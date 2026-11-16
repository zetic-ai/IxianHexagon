import pytest
import requests
from utils import *

server = ServerPreset.omni()


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.omni()


def test_server_start_simple():
    global server
    server.start()
    res = server.make_request("GET", "/health")
    assert res.status_code == 200
    assert res.body["status"] == "ok"
    res = server.make_request("GET", "/v1/health")
    assert res.status_code == 200


def test_server_props():
    global server
    server.start()
    res = server.make_request("GET", "/props")
    assert res.status_code == 200
    assert res.body["model_path"].endswith("lfm3-omni-2b")
    assert res.body["total_slots"] == 1
    default_val = res.body["default_generation_settings"]
    assert default_val["n_ctx"] == server.n_ctx
    assert default_val["params"]["seed"] == server.seed
    assert default_val["params"]["n_predict"] == server.n_predict
    assert res.body["modalities"] == {"vision": True, "video": True, "audio": True}
    assert res.body["build_info"].startswith("b")
    assert "<|im_start|>" in res.body["chat_template"]
    assert res.body["bos_token"] == "<|startoftext|>"
    assert res.body["voices"] == ["female", "male"]
    ix = res.body["ixian"]
    assert ix["model_id"] == "LiquidAI/LFM3-Omni-2B"
    assert ix["capabilities"]["speech_out"] is True
    assert ix["nsp_count"] == 2


def test_server_props_text_model():
    global server
    server = ServerPreset.text()
    server.start()
    res = server.make_request("GET", "/props")
    assert res.body["modalities"] == {"vision": False, "video": False, "audio": False}
    assert "voices" not in res.body


def test_server_models():
    global server
    server.start()
    for path in ["/models", "/v1/models"]:
        res = server.make_request("GET", path)
        assert res.status_code == 200
        assert res.body["object"] == "list"
        assert len(res.body["data"]) == 1
        assert res.body["data"][0]["id"] == server.model_alias
        assert res.body["data"][0]["object"] == "model"
        assert res.body["data"][0]["architecture"]["input_modalities"] == ["text", "image", "audio", "video"]
        assert res.body["data"][0]["architecture"]["output_modalities"] == ["text", "audio"]
        model_info = res.body["models"][0]
        assert "completion" in model_info["capabilities"]
        assert "multimodal" in model_info["capabilities"]


def test_server_model_aliases_and_tags():
    global server
    server.model_alias = "omni,fast,npu"
    server.model_tags = "chat,speech"
    server.start()
    res = server.make_request("GET", "/models")
    model = res.body["data"][0]
    assert set(model["aliases"]) == {"omni", "fast", "npu"}
    assert set(model["tags"]) == {"chat", "speech"}
    # the id is the first alias in alphabetical order (std::set), like llama-server
    assert model["id"] == "fast"


def test_server_slots():
    global server
    server.server_slots = False
    server.start()
    res = server.make_request("GET", "/slots")
    assert res.status_code == 501
    assert "error" in res.body
    server.stop()

    server.server_slots = True
    server.start()
    res = server.make_request("GET", "/slots")
    assert res.status_code == 200
    assert len(res.body) == 1
    assert res.body[0]["id"] == 0
    assert res.body[0]["n_ctx"] == server.n_ctx
    assert res.body[0]["is_processing"] is False
    assert "params" not in res.body[0]


def test_no_ui():
    global server
    server.start()
    url = f"http://{server.server_host}:{server.server_port}"
    res = requests.get(url)
    assert res.status_code == 200
    assert "<!doctype html>" in res.text
    server.stop()

    server.no_ui = True
    server.start()
    res = requests.get(url)
    assert res.status_code == 404
    assert res.json()["error"]["type"] == "not_found_error"


def test_unknown_route_is_json_404():
    global server
    server.start()
    res = server.make_request("GET", "/does-not-exist")
    assert res.status_code == 404
    assert res.body["error"]["code"] == 404


def test_loading_state_returns_503():
    """requests during the load are refused with 503 Loading model"""
    global server
    server.env = {"IXIAN_MOCK_LOAD_MS": "1500"}
    import subprocess, time
    # start the process without waiting for /health
    server_path = os.environ.get("IXIAN_SERVER_BIN_PATH") or os.path.join(SERVER_DIR, "build-mock", "bin", "ixian-server")
    argv = [server_path, "--model", server.model_dir, "--host", server.server_host, "--port", str(server.server_port)]
    proc = subprocess.Popen(argv, env={**os.environ, "IXIAN_MOCK_LOAD_MS": "1500"})
    server.process = proc
    server_instances.add(server)
    try:
        deadline = time.time() + 5
        saw_503 = False
        while time.time() < deadline:
            try:
                r = requests.get(server.make_url("/props"), timeout=1)
                if r.status_code == 503:
                    assert r.json()["error"]["type"] == "unavailable_error"
                    saw_503 = True
                    break
                if r.status_code == 200:
                    break
            except requests.exceptions.ConnectionError:
                time.sleep(0.01)
        assert saw_503, "never saw the 503 Loading model window"
        # eventually ready
        deadline = time.time() + 10
        while time.time() < deadline:
            r = requests.get(server.make_url("/health"), timeout=1)
            if r.status_code == 200:
                break
            time.sleep(0.05)
        assert r.status_code == 200
    finally:
        server.stop()


def test_version_flag():
    import subprocess
    server_path = os.environ.get("IXIAN_SERVER_BIN_PATH") or os.path.join(SERVER_DIR, "build-mock", "bin", "ixian-server")
    out = subprocess.run([server_path, "--version"], capture_output=True, text=True)
    assert out.returncode == 0
    assert "ixian-server" in out.stdout
    out = subprocess.run([server_path, "--help"], capture_output=True, text=True)
    assert out.returncode == 0
    assert "--model" in out.stdout and "--image-fit" in out.stdout


def test_llama_server_flags_are_accepted():
    """llama-server launch scripts keep working: GPU / batch flags are ignored with a note"""
    global server
    server.extra_args = ["-ngl", "99", "-b", "2048", "-ub", "512", "-fa", "on", "--jinja", "--no-mmproj", "-t", "8", "--mlock"]
    server.start()
    res = server.make_request("GET", "/health")
    assert res.status_code == 200
