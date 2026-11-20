import pytest
import requests
from openai import OpenAI
from utils import *

server = ServerPreset.text()

TEST_API_KEY = "sk-this-is-the-secret-key"


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.text()
    server.api_key = TEST_API_KEY


@pytest.mark.parametrize("endpoint", ["/health", "/v1/health"])
def test_access_public_endpoint(endpoint: str):
    global server
    server.start()
    res = server.make_request("GET", endpoint)
    assert res.status_code == 200
    assert "error" not in res.body


def test_access_static_assets_without_api_key():
    global server
    server.start()
    for path in ["/", "/index.html", "/sw.js", "/manifest.webmanifest", "/_app/version.json"]:
        res = requests.get(server.make_url(path))
        assert res.status_code == 200, f"Expected 200 for {path}, got {res.status_code}"


@pytest.mark.parametrize("api_key", [None, "invalid-key"])
def test_incorrect_api_key(api_key: str):
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={"prompt": "I believe the meaning of life is"},
                              headers={"Authorization": f"Bearer {api_key}" if api_key else None})
    assert res.status_code == 401
    assert res.body["error"]["type"] == "authentication_error"
    res = server.make_request("GET", "/props", headers={})
    assert res.status_code == 401


def test_correct_api_key():
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={"prompt": "echo: hi"}, headers={"Authorization": f"Bearer {TEST_API_KEY}"})
    assert res.status_code == 200
    assert "content" in res.body


def test_correct_api_key_anthropic_header():
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={"prompt": "echo: hi"}, headers={"X-Api-Key": TEST_API_KEY})
    assert res.status_code == 200
    assert "content" in res.body


def test_multiple_api_keys():
    global server
    server.api_key = "key-one,key-two"
    server.start()
    for key in ["key-one", "key-two"]:
        res = server.make_request("GET", "/props", headers={"Authorization": f"Bearer {key}"})
        assert res.status_code == 200
    res = server.make_request("GET", "/props", headers={"Authorization": "Bearer key-three"})
    assert res.status_code == 401


def test_openai_library_correct_api_key():
    global server
    server.start()
    client = OpenAI(api_key=TEST_API_KEY, base_url=f"http://{server.server_host}:{server.server_port}")
    res = client.chat.completions.create(model="gpt-3.5-turbo", messages=[
        {"role": "system", "content": "You are a chatbot."},
        {"role": "user", "content": "echo: hi"},
    ])
    assert len(res.choices) == 1


@pytest.mark.parametrize("origin,cors_header,cors_header_value", [
    ("localhost", "Access-Control-Allow-Origin", "localhost"),
    ("web.mydomain.fr", "Access-Control-Allow-Origin", "web.mydomain.fr"),
    ("origin", "Access-Control-Allow-Credentials", "true"),
    ("web.mydomain.fr", "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"),
    ("web.mydomain.fr", "Access-Control-Allow-Headers", "*"),
])
def test_cors_options(origin: str, cors_header: str, cors_header_value: str):
    global server
    server.start()
    res = server.make_request("OPTIONS", "/completions", headers={
        "Origin": origin,
        "Access-Control-Request-Method": "POST",
        "Access-Control-Request-Headers": "Authorization",
    })
    assert res.status_code == 200
    assert cors_header in res.headers
    assert res.headers[cors_header] == cors_header_value


@pytest.mark.parametrize("origin", [
    "http://localhost", "http://localhost:8080", "http://127.0.0.1:3000", "http://[::1]:3000", "http://evil.com", "http://localhost.evil.com",
])
def test_cors_origins_localhost_no_cross_origin_header(origin: str):
    global server
    server.cors_origins = "localhost"
    server.start()
    res = server.make_request("OPTIONS", "/completions", headers={
        "Origin": origin, "Access-Control-Request-Method": "POST", "Access-Control-Request-Headers": "Authorization",
    })
    assert res.status_code == 200
    assert "Access-Control-Allow-Origin" not in res.headers


def _raw_post(path: str, body: str, headers: dict) -> requests.Response:
    return requests.post(server.make_url(path), data=body, headers={"Content-Type": "text/plain", **headers})


@pytest.mark.parametrize("headers,expected_status", [
    ({}, 200),
    ({"Origin": "http://127.0.0.1:{port}"}, 200),
    ({"Origin": "http://localhost:3000"}, 403),
    ({"Origin": "http://evil.com"}, 403),
    ({"Origin": "http://localhost.evil.com"}, 403),
    ({"Origin": "null"}, 403),
    ({"Sec-Fetch-Site": "cross-site", "Sec-Fetch-Mode": "no-cors"}, 403),
])
def test_cors_origins_localhost_blocks_simple_requests(headers: dict, expected_status: int):
    global server
    server.api_key = None
    server.cors_origins = "localhost"
    server.start()
    headers = {k: v.format(port=server.server_port) for k, v in headers.items()}
    res = _raw_post("/tokenize", '{"content": "hello"}', headers)
    assert res.status_code == expected_status


@pytest.mark.parametrize("host,headers,expected_status", [
    ("localhost", {}, 200),
    ("127.0.0.1", {}, 200),
    ("127.0.0.2", {}, 200),
    ("[::1]", {}, 200),
    ("0.0.0.0", {}, 200),
    ("evil.com", {"Origin": "http://evil.com:{port}"}, 403),
    ("evil.com", {"Sec-Fetch-Site": "same-origin"}, 403),
    ("127.0.0.1.evil.com", {}, 403),
    ("localhost.evil.com", {}, 403),
])
def test_cors_origins_localhost_blocks_dns_rebinding(host: str, headers: dict, expected_status: int):
    global server
    server.api_key = None
    server.cors_origins = "localhost"
    server.start()
    port = server.server_port
    headers = {k: v.format(port=port) for k, v in headers.items()}
    res = _raw_post("/tokenize", '{"content": "hello"}', {"Host": f"{host}:{port}", **headers})
    assert res.status_code == expected_status


def test_cors_origins_explicit_list_allows_its_hosts():
    global server
    server.api_key = None
    server.cors_origins = "https://ia.example.com"
    server.start()
    for host, expected_status in [("ia.example.com", 200), ("evil.com", 403)]:
        res = _raw_post("/tokenize", '{"content": "hello"}', {"Host": host, "Origin": f"https://{host}"})
        assert res.status_code == expected_status, host


def test_cors_origins_explicit_list_blocks_simple_requests():
    global server
    server.api_key = None
    server.cors_origins = "https://a.example, https://b.example"
    server.start()
    for origin, expected_status in [("https://a.example", 200), ("https://b.example", 200), ("https://c.example", 403)]:
        res = _raw_post("/tokenize", '{"content": "hello"}', {"Origin": origin})
        assert res.status_code == expected_status, origin


def test_cors_origins_wildcard_keeps_allowing_all():
    global server
    server.api_key = None
    server.cors_origins = "*"
    server.start()
    res = _raw_post("/tokenize", '{"content": "hello"}', {"Origin": "http://evil.com"})
    assert res.status_code == 200
    assert res.headers["Access-Control-Allow-Origin"] == "http://evil.com"


def test_cors_cross_site_navigation_to_ui_allowed():
    global server
    server.api_key = None
    server.cors_origins = "localhost"
    server.start()
    nav = {"Sec-Fetch-Site": "cross-site", "Sec-Fetch-Mode": "navigate"}
    assert requests.get(server.make_url("/"), headers=nav).status_code != 403
    assert requests.get(server.make_url("/props"), headers=nav).status_code == 403


def test_api_prefix():
    global server
    server.api_key = None
    server.api_prefix = "/llm"
    server.start()
    assert requests.get(server.make_url("/llm/health")).status_code == 200
    assert requests.get(server.make_url("/health")).status_code == 404
    res = requests.post(server.make_url("/llm/v1/chat/completions"), json={"max_tokens": 4, "messages": [{"role": "user", "content": "echo: hi"}]})
    assert res.status_code == 200
