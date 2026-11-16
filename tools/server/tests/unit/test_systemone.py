import pytest
from utils import *

server: ServerProcess

TEST_STATE = "I was charged twice for my order last week and nobody has replied."

TEST_QUESTIONS = {
    "route": {
        "type": "choice",
        "instructions": "Which team should handle this?",
        "criteria": {"billing": "payments and refunds", "shipping": None, "technical": None},
    },
    "urgency": {
        "type": "score",
        "instructions": "How urgent is this?",
        "criteria": ["can wait", "this week", "today", "right now"],
    },
    "angry": {
        "type": "noul",
        "instructions": "Is the customer angry?",
    },
}


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.d1()


def test_systemone():
    server.start()
    res = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS})
    assert res.status_code == 200, res.body
    assert res.body["model"] == server.model_alias
    assert res.body["usage"]["input_tokens"] > 0
    assert res.body["usage"]["output_tokens"] == 0
    assert res.body["timings"]["passes"] == 1

    answers = res.body["answers"]
    assert list(answers.keys()) == ["route", "urgency", "angry"]

    route = answers["route"]
    assert route["type"] == "choice"
    assert list(route["probabilities"].keys()) == ["billing", "shipping", "technical"]
    assert abs(sum(route["probabilities"].values()) - 1.0) < 1e-4
    assert route["choice"] == max(route["probabilities"], key=route["probabilities"].get)
    assert 0.0 <= route["confidence"] <= 1.0

    urgency = answers["urgency"]
    assert urgency["type"] == "score"
    assert urgency["legend"] == {"0": "can wait", "1": "this week", "2": "today", "3": "right now"}
    assert list(urgency["probabilities"].keys()) == ["0", "1", "2", "3"]
    assert abs(sum(urgency["probabilities"].values()) - 1.0) < 1e-4
    assert urgency["score"] in (0, 1, 2, 3)
    assert 0.0 <= urgency["confidence"] <= 1.0

    angry = answers["angry"]
    assert angry["type"] == "noul"
    assert 0.0 <= angry["noul"] <= 1.0


def test_systemone_json_state():
    server.start()
    questions = {"refund": {"type": "noul", "instructions": "Is a refund requested?",
                            "criteria": {"false": "no refund is asked", "true": "a refund is asked"}}}
    res_obj = server.make_request("POST", "/v1/systemone", data={"state": {"ticket": TEST_STATE, "plan": "pro"}, "questions": questions})
    assert res_obj.status_code == 200
    res_str = server.make_request("POST", "/v1/systemone", data={"state": '{"ticket":"' + TEST_STATE + '","plan":"pro"}', "questions": questions})
    assert res_str.status_code == 200
    assert res_obj.body["usage"] == res_str.body["usage"]
    assert abs(res_obj.body["answers"]["refund"]["noul"] - res_str.body["answers"]["refund"]["noul"]) < 1e-4


def test_systemone_is_deterministic_and_depends_on_state():
    server.start()
    a = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS}).body["answers"]
    b = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS}).body["answers"]
    assert a == b
    c = server.make_request("POST", "/v1/systemone", data={"state": "Where is my parcel?", "questions": TEST_QUESTIONS}).body["answers"]
    assert c != a


@pytest.mark.parametrize("data", [
    {"questions": TEST_QUESTIONS},
    {"state": TEST_STATE},
    {"state": TEST_STATE, "questions": {}},
    {"state": TEST_STATE, "questions": {"q": {"type": "unknown", "instructions": "x"}}},
    {"state": TEST_STATE, "questions": {"q": {"type": "noul"}}},
    {"state": TEST_STATE, "questions": {"q": {"type": "choice", "instructions": "x"}}},
    {"state": TEST_STATE, "questions": {"q": {"type": "choice", "instructions": "x", "criteria": {}}}},
    {"state": TEST_STATE, "questions": {"q": {"type": "score", "instructions": "x", "criteria": ["only one"]}}},
    {"state": TEST_STATE, "questions": {"q": {"type": "noul", "instructions": "x", "criteria": "yes"}}},
    {"state": TEST_STATE, "questions": {"q": "not an object"}},
    {"state": TEST_STATE, "questions": TEST_QUESTIONS, "videos": ["data:video/mp4;base64,AAAA"]},
    {"state": None, "questions": TEST_QUESTIONS},
])
def test_systemone_invalid_request(data: dict):
    server.start()
    res = server.make_request("POST", "/v1/systemone", data=data)
    assert res.status_code == 400, res.body
    assert "error" in res.body


def test_systemone_images():
    server.start()
    image = data_url(sample_path("cats.png"), "image/png")
    res_text = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS})
    assert res_text.status_code == 200

    res = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS, "images": [image]})
    assert res.status_code == 200, res.body
    assert list(res.body["answers"].keys()) == ["route", "urgency", "angry"]
    assert res.body["usage"]["input_tokens"] > res_text.body["usage"]["input_tokens"]

    # the same image, as a part of a chat message state
    res_part = server.make_request("POST", "/v1/systemone", data={
        "state": [{"role": "user", "content": [{"type": "image_url", "image_url": {"url": image}}, {"type": "text", "text": TEST_STATE}]}],
        "questions": TEST_QUESTIONS,
    })
    assert res_part.status_code == 200
    assert res_part.body["usage"]["input_tokens"] > res_text.body["usage"]["input_tokens"]

    # a null state with an image only
    res_null = server.make_request("POST", "/v1/systemone", data={"state": None, "questions": {"cats": {"type": "noul", "instructions": "Are there cats?"}}, "images": [image]})
    assert res_null.status_code == 200

    res = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS, "images": [image] * 9})
    assert res.status_code == 400
    res = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS, "images": ["https://example.com/image.png"]})
    assert res.status_code == 400
    res = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS, "files": ["data:video/mp4;base64,AAAA"]})
    assert res.status_code == 501


def test_systemone_image_is_resized():
    server.start()
    from test_vision_api import ppm_data_url
    res = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS, "images": [ppm_data_url(640, 480)]})
    assert res.status_code == 200


def test_systemone_audio_only_on_omni():
    global server
    server.start()
    audio = data_url(sample_path("jfk.wav"), "audio/wav")
    res = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS, "files": [audio]})
    assert res.status_code == 501
    server.stop()

    server = ServerPreset.d1_omni()
    server.start()
    res = server.make_request("POST", "/v1/systemone", data={"state": None, "questions": {
        "speech": {"type": "noul", "instructions": "Is a person speaking in the audio?"},
        "topic": {"type": "choice", "instructions": "What is the speech about?", "criteria": {"country": "the speaker's country", "sports": "", "cooking": ""}},
    }, "files": [audio]})
    assert res.status_code == 200, res.body
    assert list(res.body["answers"].keys()) == ["speech", "topic"]
    assert res.body["usage"]["input_tokens"] >= 138


def test_systemone_requests_jsonl_samples():
    """the samples/requests.jsonl of the SDK's zetic_d1_cli ask/serve"""
    server.start()
    with open(sample_path("requests.jsonl")) as f:
        for line in f:
            if not line.strip():
                continue
            req = json.loads(line)
            res = server.make_request("POST", "/v1/systemone", data={k: v for k, v in req.items() if k in ("state", "questions", "images", "files")})
            assert res.status_code == 200, (req["id"], res.body)
            assert set(res.body["answers"].keys()) == set(req["questions"].keys())


def test_systemone_not_a_decision_model():
    global server
    server = ServerPreset.text()
    server.start()
    res = server.make_request("POST", "/v1/systemone", data={"state": TEST_STATE, "questions": TEST_QUESTIONS})
    assert res.status_code == 501


def test_decision_model_has_no_chat():
    server.start()
    res = server.make_request("POST", "/v1/chat/completions", data={"messages": [{"role": "user", "content": "hi"}]})
    assert res.status_code == 501
    res = server.make_request("POST", "/tokenize", data={"content": "hi"})
    assert res.status_code == 501
    res = server.make_request("GET", "/models")
    assert res.body["data"][0]["architecture"]["output_modalities"] == ["decisions"]
    assert res.body["models"][0]["capabilities"][0] == "decision"
    props = server.make_request("GET", "/props").body
    assert props["ixian"]["capabilities"]["decision"] is True


def test_decision_model_type_flag():
    global server
    server.model_type = "decision"
    server.start()
    assert server.make_request("GET", "/health").status_code == 200
    server.stop()
    server.model_type = "llm"
    import pytest as _pytest
    with _pytest.raises(RuntimeError):
        server.start()
