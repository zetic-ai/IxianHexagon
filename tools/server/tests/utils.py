#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Test harness for ixian-server, adapted from llama.cpp's tools/server/tests/utils.py so the same test style applies.
# The server runs against the mock engine (tools/server/mock) unless IXIAN_SERVER_BIN_PATH points at a real build and
# IXIAN_MODEL_DIR at a real model package.

import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from json import JSONDecodeError
from re import RegexFlag
from typing import Any, Callable, Iterator, List, Literal, Set, Tuple

import requests

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
SERVER_DIR = os.path.dirname(TESTS_DIR)
MOCK_MODELS_DIR = os.path.join(SERVER_DIR, "mock", "models")
SAMPLES_DIR = os.path.normpath(os.path.join(SERVER_DIR, "..", "..", "samples"))

DEFAULT_HTTP_TIMEOUT = 300
DEFAULT_REQUEST_TIMEOUT = 300


class ServerResponse:
    headers: dict
    status_code: int
    body: dict | Any


class ServerError(Exception):
    def __init__(self, code, body):
        self.code = code
        self.body = body


class ServerProcess:
    # default options
    debug: bool = False
    server_port: int = 9931
    server_host: str = "127.0.0.1"
    model_dir: str = os.path.join(MOCK_MODELS_DIR, "lfm3-omni-2b")
    model_alias: str | None = None
    model_tags: str | None = None
    model_type: str | None = None
    target: str | None = None
    temperature: float | None = None
    seed: int | None = 42
    n_ctx: int | None = None
    n_predict: int | None = None
    n_slots: int | None = None
    server_metrics: bool = False
    server_slots: bool = True
    endpoint_props: bool = False
    slot_save_path: str | None = None
    api_key: str | None = None
    no_ui: bool = False
    reasoning: Literal["on", "off", "auto"] | None = None
    reasoning_format: Literal["deepseek", "none", "deepseek-legacy"] | None = None
    chat_template_kwargs: str | None = None
    cors_origins: str | None = None
    media_path: str | None = None
    media_allow_private_urls: bool = False
    image_fit: str | None = None
    tts_voice: str | None = None
    tts_lang: str | None = None
    cache_prompt: bool | None = None
    sse_ping_interval: int | None = None
    api_prefix: str | None = None
    log_path: str | None = None
    extra_args: List[str] | None = None
    env: dict | None = None

    process: subprocess.Popen | None = None

    def __init__(self):
        if "DEBUG" in os.environ:
            self.debug = True
        if "PORT" in os.environ:
            self.server_port = int(os.environ["PORT"])
        self.external_server = "DEBUG_EXTERNAL" in os.environ

    def start(self, timeout_seconds: int = DEFAULT_HTTP_TIMEOUT) -> None:
        if self.external_server:
            print(f"[external_server]: assuming a server on {self.server_host}:{self.server_port}")
            return
        server_path = os.environ.get("IXIAN_SERVER_BIN_PATH") or os.environ.get("LLAMA_SERVER_BIN_PATH")
        if not server_path:
            server_path = os.path.join(SERVER_DIR, "build-mock", "bin", "ixian-server")
        model_dir = os.environ.get("IXIAN_MODEL_DIR", self.model_dir) if self.model_dir.startswith(MOCK_MODELS_DIR) else self.model_dir

        args: List[Any] = ["--model", model_dir, "--host", self.server_host, "--port", self.server_port]
        if self.seed is not None:
            args += ["--seed", self.seed]
        if self.temperature is not None:
            args += ["--temp", self.temperature]
        if self.model_alias:
            args += ["--alias", self.model_alias]
        if self.model_tags:
            args += ["--tags", self.model_tags]
        if self.model_type:
            args += ["--model-type", self.model_type]
        if self.target:
            args += ["--target", self.target]
        if self.n_ctx:
            args += ["--ctx-size", self.n_ctx]
        if self.n_predict:
            args += ["--n-predict", self.n_predict]
        if self.n_slots:
            args += ["--parallel", self.n_slots]
        if self.server_metrics:
            args.append("--metrics")
        args.append("--slots" if self.server_slots else "--no-slots")
        if self.endpoint_props:
            args.append("--props")
        if self.slot_save_path:
            args += ["--slot-save-path", self.slot_save_path]
        if self.api_key:
            args += ["--api-key", self.api_key]
        if self.no_ui:
            args.append("--no-ui")
        if self.reasoning:
            args += ["--reasoning", self.reasoning]
        if self.reasoning_format:
            args += ["--reasoning-format", self.reasoning_format]
        if self.chat_template_kwargs:
            args += ["--chat-template-kwargs", self.chat_template_kwargs]
        if self.cors_origins:
            args += ["--cors-origins", self.cors_origins]
        if self.media_path:
            args += ["--media-path", self.media_path]
        if self.media_allow_private_urls:
            args.append("--media-allow-private-urls")
        if self.image_fit:
            args += ["--image-fit", self.image_fit]
        if self.tts_voice:
            args += ["--tts-voice", self.tts_voice]
        if self.tts_lang:
            args += ["--tts-lang", self.tts_lang]
        if self.cache_prompt is False:
            args.append("--no-cache-prompt")
        if self.sse_ping_interval is not None:
            args += ["--sse-ping-interval", self.sse_ping_interval]
        if self.api_prefix:
            args += ["--api-prefix", self.api_prefix]
        if self.debug:
            args.append("--verbose")
        if self.extra_args:
            args += self.extra_args

        env = {**os.environ, "IXIAN_MOCK_LOAD_MS": os.environ.get("IXIAN_MOCK_LOAD_MS", "20")}
        if self.env:
            env.update(self.env)

        argv = [str(a) for a in [server_path, *args]]
        print(f"tests: starting server with: {' '.join(argv)}")
        if self.log_path:
            self._log = open(self.log_path, "w")
        else:
            self._log = sys.stdout
        self.process = subprocess.Popen(argv, stdout=self._log, stderr=self._log if self._log != sys.stdout else sys.stdout, env=env)
        server_instances.add(self)
        print(f"server pid={self.process.pid}, pytest pid={os.getpid()}")

        start_time = time.time()
        last_print = start_time
        while time.time() - start_time < timeout_seconds:
            try:
                response = self.make_request("GET", (self.api_prefix or "") + "/health", headers={"Authorization": f"Bearer {self.api_key}" if self.api_key else None}, quiet=True)
                if response.status_code == 200:
                    self.ready = True
                    return
            except Exception:
                pass
            if self.process.poll() is not None:
                raise RuntimeError(f"Server process died with return code {self.process.returncode}")
            if time.time() - last_print >= 1.0:
                print("Waiting for server to start...")
                last_print = time.time()
            time.sleep(0.01)
        raise TimeoutError(f"Server did not start within {timeout_seconds} seconds")

    def stop(self) -> None:
        if self.external_server:
            return
        if self in server_instances:
            server_instances.remove(self)
        if self.process:
            print(f"Stopping server with pid={self.process.pid}")
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                print(f"Server pid={self.process.pid} did not terminate in time, killing")
                self.process.kill()
                self.process.wait(timeout=5)
            except Exception as e:
                print(f"Error waiting for server: {e}")
            self.process = None
        if hasattr(self, "_log") and self._log != sys.stdout:
            self._log.close()

    def make_url(self, path: str, host: str | None = None) -> str:
        if host is None:
            host = self.server_host.split(",")[0].strip()
            host = {"0.0.0.0": "127.0.0.1", "::": "[::1]"}.get(host, host)
        return f"http://{host}:{self.server_port}{path}"

    def make_request(self, method: str, path: str, data: dict | Any | None = None, headers: dict | None = None,
                     timeout: float | None = DEFAULT_REQUEST_TIMEOUT, host: str | None = None, quiet: bool = False) -> ServerResponse:
        url = self.make_url(path, host)
        parse_body = False
        if method == "GET":
            response = requests.get(url, headers=headers, timeout=timeout)
            parse_body = True
        elif method == "POST":
            response = requests.post(url, headers=headers, json=data, timeout=timeout)
            parse_body = True
        elif method == "DELETE":
            response = requests.delete(url, headers=headers, timeout=timeout)
            parse_body = True
        elif method == "OPTIONS":
            response = requests.options(url, headers=headers, timeout=timeout)
        else:
            raise ValueError(f"Unimplemented method: {method}")
        result = ServerResponse()
        result.headers = dict(response.headers)
        result.status_code = response.status_code
        if parse_body:
            try:
                result.body = response.json()
            except (JSONDecodeError, requests.exceptions.JSONDecodeError):
                result.body = response.text
        else:
            result.body = None
        if not quiet:
            shown = json.dumps(result.body, indent=2) if not isinstance(result.body, str) else result.body[:500]
            print("Response from server", shown)
        return result

    def make_stream_request(self, method: str, path: str, data: dict | None = None, headers: dict | None = None,
                            host: str | None = None) -> Iterator[dict]:
        url = self.make_url(path, host)
        if method == "POST":
            response = requests.post(url, headers=headers, json=data, stream=True)
        else:
            raise ValueError(f"Unimplemented method: {method}")
        if response.status_code != 200:
            raise ServerError(response.status_code, response.json())
        for line_bytes in response.iter_lines():
            line = line_bytes.decode("utf-8")
            if "[DONE]" in line:
                break
            elif line.startswith("data: "):
                data = json.loads(line[6:])
                print("Partial response from server", json.dumps(data, indent=2))
                yield data

    def make_any_request(self, method: str, path: str, data: dict | None = None, headers: dict | None = None,
                         timeout: float | None = DEFAULT_REQUEST_TIMEOUT) -> dict:
        stream = data.get("stream", False) if data else False
        if stream:
            content: list[str] = []
            reasoning_content: list[str] = []
            tool_calls: list[dict] = []
            finish_reason = None
            for chunk in self.make_stream_request(method, path, data, headers):
                if chunk["choices"]:
                    assert len(chunk["choices"]) == 1
                    choice = chunk["choices"][0]
                    if choice["delta"].get("content") is not None:
                        assert len(choice["delta"]["content"]) > 0, "Expected non empty content delta!"
                        content.append(choice["delta"]["content"])
                    if choice["delta"].get("reasoning_content") is not None:
                        reasoning_content.append(choice["delta"]["reasoning_content"])
                    if choice.get("finish_reason") is not None:
                        finish_reason = choice["finish_reason"]
                    for tc in choice["delta"].get("tool_calls", []):
                        if tc["index"] >= len(tool_calls):
                            assert "id" in tc and tc.get("type") == "function"
                            tool_calls.append(dict(id="", type="function", function=dict(name="", arguments="")))
                        tool_call = tool_calls[tc["index"]]
                        if tc.get("id") is not None:
                            tool_call["id"] = tc["id"]
                        fct = tc["function"]
                        if fct.get("name") is not None:
                            tool_call["function"]["name"] = tool_call["function"].get("name", "") + fct["name"]
                        if fct.get("arguments") is not None:
                            tool_call["function"]["arguments"] += fct["arguments"]
                else:
                    assert "usage" in chunk, f"Expected usage in chunk: {chunk}"
                    assert "timings" in chunk, f"Expected timings in chunk: {chunk}"
            return dict(choices=[dict(index=0, finish_reason=finish_reason, message=dict(
                role="assistant",
                content="".join(content) if content else None,
                reasoning_content="".join(reasoning_content) if reasoning_content else None,
                tool_calls=tool_calls if tool_calls else None,
            ))])
        response = self.make_request(method, path, data, headers, timeout=timeout)
        assert response.status_code == 200, f"Server returned error: {response.status_code} {response.body}"
        return response.body


server_instances: Set[ServerProcess] = set()


class ServerPreset:
    @staticmethod
    def omni() -> ServerProcess:
        """LFM3-Omni-2B: text, image, audio, video in; text and speech out"""
        server = ServerProcess()
        server.model_dir = os.path.join(MOCK_MODELS_DIR, "lfm3-omni-2b")
        server.model_alias = "lfm3-omni-2b"
        server.n_ctx = 2048
        server.n_predict = 64
        server.seed = 42
        return server

    @staticmethod
    def text() -> ServerProcess:
        """LFM3-1.7B: text only"""
        server = ServerProcess()
        server.model_dir = os.path.join(MOCK_MODELS_DIR, "lfm3-1.7b")
        server.model_alias = "lfm3-1.7b"
        server.n_ctx = 2048
        server.n_predict = 64
        server.seed = 42
        return server

    @staticmethod
    def vl() -> ServerProcess:
        server = ServerProcess()
        server.model_dir = os.path.join(MOCK_MODELS_DIR, "lfm3-vl-2b")
        server.model_alias = "lfm3-vl-2b"
        server.n_ctx = 2048
        server.n_predict = 64
        return server

    @staticmethod
    def audio() -> ServerProcess:
        server = ServerProcess()
        server.model_dir = os.path.join(MOCK_MODELS_DIR, "lfm3-audio-2b")
        server.model_alias = "lfm3-audio-2b"
        server.n_ctx = 2048
        server.n_predict = 64
        return server

    @staticmethod
    def d1() -> ServerProcess:
        server = ServerProcess()
        server.model_dir = os.path.join(MOCK_MODELS_DIR, "d1-2b")
        server.model_alias = "d1-2b"
        return server

    @staticmethod
    def d1_omni() -> ServerProcess:
        server = ServerProcess()
        server.model_dir = os.path.join(MOCK_MODELS_DIR, "d1-omni-2b")
        server.model_alias = "d1-omni-2b"
        return server


def parallel_function_calls(function_list: List[Tuple[Callable[..., Any], Tuple[Any, ...]]]) -> List[Any]:
    results = [None] * len(function_list)
    exceptions = []

    def worker(index, func, args):
        try:
            results[index] = func(*args)
        except Exception as e:
            exceptions.append((index, str(e)))

    with ThreadPoolExecutor() as executor:
        futures = [executor.submit(worker, i, func, args) for i, (func, args) in enumerate(function_list)]
        for future in as_completed(futures):
            pass
    if exceptions:
        print("Exceptions occurred:")
        for index, error in exceptions:
            print(f"Function at index {index}: {error}")
    return results


def match_regex(regex: str, text: str) -> bool:
    return re.compile(regex, flags=RegexFlag.IGNORECASE | RegexFlag.MULTILINE | RegexFlag.DOTALL).search(text) is not None


def sample_path(name: str) -> str:
    return os.path.join(SAMPLES_DIR, name)


def data_url(path: str, mime: str) -> str:
    import base64
    with open(path, "rb") as f:
        return f"data:{mime};base64," + base64.b64encode(f.read()).decode("utf-8")


def is_slow_test_allowed():
    return os.environ.get("SLOW_TESTS") in ("1", "ON")
